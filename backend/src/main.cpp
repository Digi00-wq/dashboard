#include "config.h"
#include "dashboard.pb.h"
#include "ha_events.h"
#include "home_assistant.h"
#include "system_stats.h"

#include <App.h>
#include <google/protobuf/util/json_util.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
namespace dash = dashboard_backend;
using Clock = std::chrono::steady_clock;

// Per-socket state (required template parameter for uWS).
struct PerSocketData {};

// ---------------------------------------------------------------------------
// Command queue: WebSocket thread -> worker thread
// ---------------------------------------------------------------------------
class CommandQueue {
public:
  void push(dash::Command cmd) {
    {
      std::lock_guard lock(m_);
      q_.push_back(std::move(cmd));
    }
    cv_.notify_one();
  }

  // Waits until a command arrives or the deadline passes.
  std::deque<dash::Command> wait_until(Clock::time_point deadline) {
    std::unique_lock lock(m_);
    cv_.wait_until(lock, deadline, [&] { return !q_.empty(); });
    std::deque<dash::Command> out;
    out.swap(q_);
    return out;
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<dash::Command> q_;
};

// ---------------------------------------------------------------------------
// Worker: collects data, executes commands, hands snapshots to the loop
// ---------------------------------------------------------------------------
void worker(const Config &cfg, CommandQueue &commands, CoverCache &covers,
            std::function<void(std::string)> publish) {
  HomeAssistant ha(cfg, covers);
  SystemStats sys(cfg.disk_path);

  dash::Dashboard ha_part; // last Home Assistant snapshot
  std::vector<std::string> ha_errors;
  Clock::time_point next_ha{}; // run immediately
  std::vector<std::string> cmd_errors;

  for (;;) {
    auto now = Clock::now();
    if (now >= next_ha) {
      ha_part.Clear();
      ha_errors.clear();
      if (cfg.ha_enabled()) ha.fill(ha_part, ha_errors);
      next_ha = now + std::chrono::seconds(cfg.poll_interval_s);
      for (const auto &e : ha_errors) std::cerr << "[HA] " << e << "\n";
    }

    dash::Dashboard snap = ha_part;
    sys.fill(snap.mutable_server());
    snap.set_updated_at(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
    if (!cfg.ha_enabled()) snap.add_errors("Home Assistant not configured (HA_URL / HA_TOKEN)");
    for (const auto &e : ha_errors) snap.add_errors(e);
    for (const auto &e : cmd_errors) snap.add_errors(e);
    cmd_errors.clear();

    std::string bytes;
    if (snap.SerializeToString(&bytes)) publish(std::move(bytes));

    // Sleep until next server-stats tick, or wake early for a command.
    auto cmds = commands.wait_until(
        Clock::now() + std::chrono::seconds(cfg.server_poll_interval_s));
    for (const auto &cmd : cmds) {
      std::string err;
      bool ok = true;
      switch (cmd.cmd_case()) {
      case dash::Command::kLight:
        ok = ha.light_command(cmd.light(), err);
        break;
      case dash::Command::kMedia:
        ok = ha.media_command(cmd.media(), err);
        break;
      case dash::Command::kAllLights:
        ok = ha.all_lights(cmd.all_lights(), err);
        break;
      case dash::Command::kCompleteTask:
        ok = ha.complete_task(cmd.complete_task().entity_id(),
                              cmd.complete_task().uid(), err);
        break;
      default:
        break;
      }
      if (!ok) {
        std::cerr << "[CMD] " << err << "\n";
        cmd_errors.push_back(err);
      }
    }
    if (!cmds.empty()) next_ha = Clock::now(); // refresh HA right away
  }
}

// ---------------------------------------------------------------------------
// Static file serving for the built frontend
// ---------------------------------------------------------------------------
std::string_view mime_for(const fs::path &p) {
  static const std::unordered_map<std::string, std::string_view> types = {
      {".html", "text/html; charset=utf-8"},
      {".js", "text/javascript; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},
      {".svg", "image/svg+xml"},
      {".png", "image/png"},
      {".jpg", "image/jpeg"},
      {".ico", "image/x-icon"},
      {".json", "application/json"},
      {".webmanifest", "application/manifest+json"},
      {".woff2", "font/woff2"},
      {".txt", "text/plain; charset=utf-8"},
  };
  auto it = types.find(p.extension().string());
  return it == types.end() ? "application/octet-stream" : it->second;
}

bool read_whole(const fs::path &p, std::string &out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

template <bool SSL>
void serve_static(const fs::path &root, uWS::HttpResponse<SSL> *res,
                  std::string_view url) {
  std::string rel(url.substr(0, url.find('?')));
  while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
  if (rel.empty()) rel = "index.html";

  if (rel.find("..") != std::string::npos) {
    res->writeStatus("400 Bad Request")->end("bad path");
    return;
  }

  fs::path file = root / rel;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    // SPA fallback: unknown routes without extension get index.html
    if (fs::path(rel).has_extension()) {
      res->writeStatus("404 Not Found")->end("not found");
      return;
    }
    file = root / "index.html";
  }

  std::string body;
  if (!read_whole(file, body)) {
    res->writeStatus("404 Not Found")->end("not found");
    return;
  }
  // Vite puts content-hashed files into assets/ -> cache forever.
  bool immutable = rel.rfind("assets/", 0) == 0;
  res->writeHeader("Content-Type", mime_for(file))
      ->writeHeader("Cache-Control", immutable
                                         ? "public, max-age=31536000, immutable"
                                         : "no-cache")
      ->end(body);
}

// ---------------------------------------------------------------------------

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  std::cout << std::unitbuf; // flush log lines immediately (pipes, docker logs)
  const Config cfg = Config::from_env();

  std::cout << "[Config] Home Assistant: "
            << (cfg.ha_enabled() ? cfg.ha_url : "not configured") << "\n"
            << "[Config] Static dir: "
            << (cfg.static_dir.empty() ? "(none)" : cfg.static_dir) << "\n";

  uWS::App app;
  uWS::Loop *loop = uWS::Loop::get();
  std::string latest; // last snapshot; only touched on the loop thread
  CommandQueue commands;
  CoverCache covers;

  app.ws<PerSocketData>(
      "/ws",
      {.compression = uWS::DISABLED,
       .maxPayloadLength = 16 * 1024,
       .idleTimeout = 120,
       .sendPingsAutomatically = true,
       .open =
           [&latest](auto *ws) {
             ws->subscribe("dashboard");
             if (!latest.empty()) ws->send(latest, uWS::OpCode::BINARY);
           },
       .message =
           [&commands](auto *ws, std::string_view msg, uWS::OpCode op) {
             if (op != uWS::OpCode::BINARY) return;
             dash::Command cmd;
             if (!cmd.ParseFromArray(msg.data(), int(msg.size()))) {
               std::cerr << "[WS] invalid command\n";
               return;
             }
             commands.push(std::move(cmd));
           }});

  app.get("/healthz", [](auto *res, auto *) { res->end("ok"); });

  // Debug view of the current snapshot as JSON.
  app.get("/api/dashboard", [&latest](auto *res, auto *) {
    dash::Dashboard d;
    d.ParseFromString(latest);
    std::string json;
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    (void)google::protobuf::util::MessageToJsonString(d, &json, opts);
    res->writeHeader("Content-Type", "application/json")->end(json);
  });

  // Album covers (fetched from HA by the worker, see CoverCache).
  app.get("/api/cover/*", [&covers](auto *res, auto *req) {
    std::string entity(req->getUrl().substr(std::string_view("/api/cover/").size()));
    auto cover = covers.get(entity);
    if (!cover) {
      res->writeStatus("404 Not Found")->end("no cover");
      return;
    }
    res->writeHeader("Content-Type", cover->mime)
        ->writeHeader("Cache-Control", "public, max-age=604800, immutable")
        ->end(cover->bytes);
  });

  if (!cfg.static_dir.empty()) {
    fs::path root = fs::absolute(cfg.static_dir);
    app.get("/*", [root](auto *res, auto *req) {
      serve_static(root, res, req->getUrl());
    });
  } else {
    app.get("/*", [](auto *res, auto *) {
      res->writeHeader("Content-Type", "text/plain")
          ->end("Dashboard backend running. WebSocket on /ws. "
                "Set STATIC_DIR to serve the frontend.");
    });
  }

  // Worker thread publishes through the event loop (Loop::defer is thread safe).
  std::thread([&] {
    worker(cfg, commands, covers, [&](std::string bytes) {
      loop->defer([&, bytes = std::move(bytes)]() mutable {
        latest = std::move(bytes);
        app.publish("dashboard", latest, uWS::OpCode::BINARY);
      });
    });
  }).detach();

  // Live updates: HA pushes state changes over its WebSocket API; each one
  // wakes the worker for an immediate refresh. Polling stays as a fallback.
  if (cfg.ha_enabled()) {
    std::thread([&] {
      HaEventStream events(cfg, [&] {
        dash::Command refresh;
        refresh.set_refresh(true);
        commands.push(std::move(refresh));
      });
      events.run();
    }).detach();
  }

  app.listen(cfg.port,
             [&](auto *token) {
               if (token)
                 std::cout << "[Server] listening on http://0.0.0.0:"
                           << cfg.port << "\n";
               else {
                 std::cerr << "[Server] could not bind port " << cfg.port
                           << "\n";
                 std::exit(1);
               }
             })
      .run();
  return 0;
}
