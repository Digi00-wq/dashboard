// Implementation of WebServer (see web.h). Sections, top to bottom:
//   helpers (JSON responses, cookies, security headers) -> snapshot filtering per permission ->
//   static files -> run()/request helpers -> SSE /api/events -> WebSocket /ws ->
//   login + user admin -> HomeCollect (/api/collect, devices) -> app routes (history, prefs,
//   presence, briefing) -> Claude bridge proxy -> notifications.
// Every route: capture() request -> current_user() (cookie or Bearer) -> permission check.

#include "web.h"

#include "automations.h" // Automations::command_for_action
#include "http_client.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

using nlohmann::json;
namespace fs = std::filesystem;
namespace dash = dashboard_backend;
using Res = WebServer::Res;
using WS = WebServer::WS;

namespace {

constexpr size_t kMaxBody = 16 * 1024;
constexpr int kMaxConcurrentHashing = 4;
constexpr const char *kCookie = "dash_session";

// Strict CSP: only our own scripts/styles/images; no framing, no plugins.
// style-src needs 'unsafe-inline' for Solid's style="" attributes (no user
// HTML is ever rendered, so this does not open an XSS vector).
constexpr const char *kCsp =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
    "img-src 'self' data:; font-src 'self' data:;connect-src 'self'; manifest-src 'self'; "
    "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'";

// HTTP status code -> reason phrase for uWS writeStatus().
std::string_view status_text(int code) {
  switch (code) {
  case 200: return "200 OK";
  case 201: return "201 Created";
  case 204: return "204 No Content";
  case 400: return "400 Bad Request";
  case 401: return "401 Unauthorized";
  case 403: return "403 Forbidden";
  case 404: return "404 Not Found";
  case 409: return "409 Conflict";
  case 413: return "413 Payload Too Large";
  case 415: return "415 Unsupported Media Type";
  case 429: return "429 Too Many Requests";
  case 502: return "502 Bad Gateway";
  case 503: return "503 Service Unavailable";
  default: return "500 Internal Server Error";
  }
}

// Served over HTTPS (COOKIE_SECURE=1): tell browsers to stay on HTTPS. Set
// once in the WebServer constructor. No includeSubDomains / preload: other
// subdomains of the domain need not be HTTPS-only.
bool g_hsts = false;

// CSP, X-Frame-Options, nosniff etc. on every response.
void security_headers(Res *res) {
  if (g_hsts) res->writeHeader("Strict-Transport-Security", "max-age=31536000");
  res->writeHeader("X-Content-Type-Options", "nosniff")
      ->writeHeader("X-Frame-Options", "DENY")
      ->writeHeader("Referrer-Policy", "no-referrer")
      ->writeHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=()")
      ->writeHeader("Cross-Origin-Opener-Policy", "same-origin")
      ->writeHeader("Cross-Origin-Resource-Policy", "same-origin")
      ->writeHeader("Content-Security-Policy", kCsp);
}

// Sends a JSON response (optionally with Set-Cookie).
void send_json(Res *res, int code, const json &body, const std::string &set_cookie = "") {
  res->writeStatus(status_text(code));
  security_headers(res);
  res->writeHeader("Cache-Control", "no-store");
  if (!set_cookie.empty()) res->writeHeader("Set-Cookie", set_cookie);
  if (code == 204) {
    res->end();
    return;
  }
  res->writeHeader("Content-Type", "application/json")->end(body.dump());
}

// Sends {"error": code, ...extra} with the given HTTP status.
void send_error(Res *res, int code, const std::string &error, json extra = json::object()) {
  extra["error"] = error;
  send_json(res, code, extra);
}

// Set-Cookie value for a new session (HttpOnly, SameSite=Strict, Secure if COOKIE_SECURE).
std::string session_cookie(const std::string &token, int days, bool secure) {
  std::string c = std::string(kCookie) + "=" + token +
                  "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(days * 86400);
  if (secure) c += "; Secure";
  return c;
}

// Set-Cookie value that deletes the session cookie.
std::string clear_cookie(bool secure) {
  std::string c = std::string(kCookie) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
  if (secure) c += "; Secure";
  return c;
}

// Value of cookie `name` from a Cookie header.
std::string cookie_value(std::string_view header, std::string_view name) {
  size_t pos = 0;
  while (pos < header.size()) {
    size_t end = header.find(';', pos);
    std::string_view part = header.substr(pos, end == std::string_view::npos ? end : end - pos);
    while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
    if (part.size() > name.size() && part.substr(0, name.size()) == name &&
        part[name.size()] == '=')
      return std::string(part.substr(name.size() + 1));
    if (end == std::string_view::npos) break;
    pos = end + 1;
  }
  return {};
}

// User as JSON for the API (never contains the password hash).
json user_json(const auth::User &u) {
  return {{"name", u.name},
          {"displayName", u.spoken_name()},
          {"admin", u.admin},
          {"permissions", auth::perm_to_names(u.admin ? auth::ALL_FEATURES : u.perms)},
          {"disabled", u.disabled},
          {"created", u.created}};
}

// j[key] if it is a string.
std::optional<std::string> string_field(const json &j, const char *key) {
  if (!j.contains(key) || !j[key].is_string()) return std::nullopt;
  return j[key].get<std::string>();
}

// Lowercase + trimmed (usernames).
std::string lower_trim(std::string s) {
  auto b = s.find_first_not_of(" \t\r\n");
  auto e = s.find_last_not_of(" \t\r\n");
  s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Permission names array from a request -> bitmask (nullopt if invalid).
std::optional<uint32_t> perms_field(const json &j) {
  if (!j.contains("permissions")) return std::nullopt;
  if (!j["permissions"].is_array()) return std::nullopt;
  std::vector<std::string> names;
  for (const auto &p : j["permissions"])
    if (p.is_string()) names.push_back(p.get<std::string>());
  return auth::perms_from_names(names);
}

// Removes everything a mask may not see.
dash::Dashboard filter(const dash::Dashboard &full, uint32_t mask) {
  dash::Dashboard d = full;
  if (!(mask & auth::WEATHER)) d.clear_weather();
  if (!(mask & auth::LIGHTS)) d.clear_lamps();
  if (!(mask & auth::ROOM)) {
    d.clear_presence();
    d.clear_light();
  }
  if (!(mask & auth::TASKS)) d.clear_todoist();
  if (!(mask & auth::MEDIA)) d.mutable_glance()->clear_media();
  if (!(mask & auth::HEALTH)) {
    d.clear_phone();
    d.clear_sleep();
    d.mutable_glance()->clear_next_alarm();
  }
  if (!(mask & auth::SERVER)) {
    d.clear_server();
    d.mutable_glance()->clear_network();
  }
  if (!(mask & auth::ADMIN_VIEW)) d.clear_errors();
  if (!(mask & auth::ANNOUNCE)) d.clear_voice_available();
  if (!(mask & auth::HEALTH)) d.clear_activity();
  if (!(mask & auth::ADMIN_VIEW)) d.mutable_activity()->clear_location();
  if (d.has_activity() && d.activity().ByteSizeLong() == 0) d.clear_activity();
  return d;
}

// Permission a client Command needs (0 = anyone, e.g. refresh).
uint32_t required_perm(const dash::Command &cmd) {
  switch (cmd.cmd_case()) {
  case dash::Command::kLight:
  case dash::Command::kAllLights: return auth::LIGHTS;
  case dash::Command::kCompleteTask: return auth::TASKS;
  case dash::Command::kMedia: return auth::MEDIA;
  case dash::Command::kAnnounce: return auth::ANNOUNCE;
  default: return 0; // refresh
  }
}

// uWS pub/sub topic per permission mask: clients with the same mask share one filtered snapshot.
std::string topic(uint32_t mask) { return "d/" + std::to_string(mask); }

// Server-side checks shared by /ws and POST /api/command. Returns false if
// the command must be dropped. Stamps announcements with the real sender.
bool prepare_command(dash::Command &cmd, const std::string &user) {
  if (cmd.has_announce()) {
    if (cmd.announce().text().empty() || cmd.announce().text().size() > 800) return false;
    cmd.mutable_announce()->set_from(user);
  }
  return true;
}

constexpr size_t kMaxSseClients = 256;
constexpr int kSseHeartbeatMs = 25000; // well below Cloudflare's 100 s idle cut

// Base64 of the protobuf snapshot for SSE / JSON polling.
std::string base64(std::string_view in) {
  static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    out += tbl[n >> 18];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += tbl[n & 63];
  }
  if (i + 1 == in.size()) {
    uint32_t n = uint8_t(in[i]) << 16;
    out += tbl[n >> 18];
    out += tbl[(n >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
    out += tbl[n >> 18];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

// Snapshot as JSON (debug endpoint and ?format=json).
std::string to_json(const dash::Dashboard &d) {
  std::string out;
  if (!google::protobuf::util::MessageToJsonString(d, &out).ok()) return {};
  return out; // single line
}

// One SSE "message" event: base64 Dashboard protobuf (web) or JSON (iOS app).
std::string sse_event(const dash::Dashboard &d, bool as_json = false) {
  if (as_json) {
    std::string j = to_json(d);
    return j.empty() ? j : "data: " + j + "\n\n";
  }
  std::string bytes;
  if (!d.SerializeToString(&bytes)) return {};
  return "data: " + base64(bytes) + "\n\n";
}

// ---------------------------------------------------------------------------
// Static files
// ---------------------------------------------------------------------------
std::string_view mime_for(const fs::path &p) {
  static const std::map<std::string, std::string_view> types = {
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
      {".woff", "font/woff"},
      {".txt", "text/plain; charset=utf-8"},
  };
  auto it = types.find(p.extension().string());
  return it == types.end() ? "application/octet-stream" : it->second;
}

// Serves the built frontend from STATIC_DIR (no path traversal, SPA fallback to index.html).
void serve_static(const fs::path &root, Res *res, std::string_view url) {
  std::string rel(url.substr(0, url.find('?')));
  while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
  if (rel.empty()) rel = "index.html";

  // No traversal, no hidden files, no odd bytes.
  if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos ||
      rel.find('\0') != std::string::npos || rel.find("/.") != std::string::npos ||
      rel.front() == '.') {
    res->writeStatus("400 Bad Request");
    security_headers(res);
    res->end("bad path");
    return;
  }

  fs::path file = root / rel;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    if (fs::path(rel).has_extension()) {
      res->writeStatus("404 Not Found");
      security_headers(res);
      res->end("not found");
      return;
    }
    file = root / "index.html"; // SPA fallback
  }

  std::ifstream f(file, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  bool immutable = rel.rfind("assets/", 0) == 0;
  res->writeStatus("200 OK");
  security_headers(res);
  res->writeHeader("Content-Type", mime_for(file))
      ->writeHeader("Cache-Control", immutable ? "public, max-age=31536000, immutable" : "no-cache")
      ->end(ss.str());
}

} // namespace

// ---------------------------------------------------------------------------

WebServer::WebServer(const Config &cfg, CommandQueue &commands, CoverCache &covers,
                     CollectedState &collected, PrefsStore &prefs, AutomationBus &bus)
    : cfg_(cfg), commands_(commands), covers_(covers), collected_(collected), prefs_(prefs),
      bus_(bus),
      loop_(uWS::Loop::get()),
      users_(fs::path(cfg.data_dir) / "users.json"),
      sessions_(fs::path(cfg.data_dir) / "sessions.json",
                std::chrono::hours(24 * cfg.session_days)),
      collector_(fs::path(cfg.data_dir) / "collect.db") {
  g_hsts = cfg.cookie_secure;
  std::string err;
  if (!users_.load(err)) std::cerr << "[Auth] " << err << "\n";
  if (!collector_.open(err)) std::cerr << "[Collect] " << err << "\n";
  bus_.inbox.open(fs::path(cfg.data_dir) / "notifications.json");
  // Worker thread -> event loop -> the user's notification streams.
  bus_.inbox.on_new([this](const std::string &user, const json &n) {
    loop_->defer([this, user, n] { on_notification(user, n); });
  });
  for (const auto &user : collector_.users_with_data()) refresh_summary(user);
  sessions_.load();
  if (users_.empty())
    std::cout << "[Auth] No users yet. Create your admin account with:\n"
                 "         dashboard_backend --create-admin <name>\n"
                 "       (Docker: docker compose exec -it dashboard dashboard_backend "
                 "--create-admin <name>)\n";
  else
    std::cout << "[Auth] " << users_.all().size() << " user(s) loaded\n";
  websocket();
  events_route();
  collect_routes();
  routes();

  heartbeat_ = us_create_timer((struct us_loop_t *)loop_, 0, sizeof(WebServer *));
  *(WebServer **)us_timer_ext(heartbeat_) = this;
  us_timer_set(heartbeat_, &WebServer::heartbeat, kSseHeartbeatMs, kSseHeartbeatMs);
}

// Called by the worker with a new snapshot; sent to all clients from the loop thread.
void WebServer::publish(std::string bytes) {
  loop_->defer([this, bytes = std::move(bytes)]() mutable { on_snapshot(std::move(bytes)); });
}

// Binds BIND_ADDRESS:PORT and runs the event loop (blocks forever).
void WebServer::run() {
  app_.listen(cfg_.bind_address, cfg_.port, [&](auto *token) {
        if (token) {
          std::cout << "[Server] listening on http://" << cfg_.bind_address << ":" << cfg_.port
                    << "\n";
        } else {
          std::cerr << "[Server] could not bind port " << cfg_.port << "\n";
          std::exit(1);
        }
      }).run();
}

// ---------------------------------------------------------------------------
// Request helpers
// ---------------------------------------------------------------------------
WebServer::Req WebServer::capture(Res *res, uWS::HttpRequest *req) const {
  Req r;
  r.token = cookie_value(req->getHeader("cookie"), kCookie);
  r.origin = std::string(req->getHeader("origin"));
  r.host = std::string(req->getHeader("host"));
  r.content_type = std::string(req->getHeader("content-type"));
  r.authorization = std::string(req->getHeader("authorization"));
  if (cfg_.trust_proxy) {
    // Cloudflare sets CF-Connecting-IP; other proxies X-Forwarded-For.
    // Only trustworthy if the backend is reachable through the proxy alone
    // (BIND_ADDRESS=127.0.0.1), otherwise clients could spoof these.
    r.ip = std::string(req->getHeader("cf-connecting-ip"));
    if (r.ip.empty()) {
      std::string xff(req->getHeader("x-forwarded-for"));
      r.ip = xff.substr(0, xff.find(','));
    }
  }
  if (r.ip.empty()) r.ip = std::string(res->getRemoteAddressAsText());
  return r;
}

// Browsers always send Origin on cross-site POST and WebSocket requests;
// reject any that don't come from this very host (CSRF / CSWSH protection).
bool WebServer::origin_ok(const Req &r) const {
  if (r.origin.empty()) return true; // non-browser client (curl etc.)
  return r.origin == "http://" + r.host || r.origin == "https://" + r.host;
}

const auth::User *WebServer::current_user(const Req &r) {
  // Browsers can't attach an Authorization header cross-site without a CORS
  // preflight (which we never allow), so bearer tokens are CSRF-safe too.
  if (r.token.empty()) return bearer_user(r);
  users_.reload_if_changed();
  auto name = sessions_.lookup(r.token);
  if (!name) return nullptr;
  const auth::User *u = users_.find(*name);
  if (!u || u->disabled) return nullptr;
  return u;
}

// Collects the request body (size-limited), parses JSON and calls handler; rejects wrong Origin/Content-Type.
void WebServer::with_json_body(Res *res, uWS::HttpRequest *req, BodyHandler handler,
                               size_t max_body) {
  auto aborted = std::make_shared<bool>(false);
  auto body = std::make_shared<std::string>();
  auto too_big = std::make_shared<bool>(false);
  auto r = std::make_shared<Req>(capture(res, req));
  res->onAborted([aborted] { *aborted = true; });
  res->onData([=, this](std::string_view chunk, bool last) {
    if (body->size() + chunk.size() > max_body) *too_big = true;
    else body->append(chunk);
    if (!last || *aborted) return;
    if (*too_big) return send_error(res, 413, "body_too_large");
    if (!origin_ok(*r)) return send_error(res, 403, "bad_origin");
    // JSON-only API: HTML forms can't send this cross-site without CORS.
    if (r->content_type.rfind("application/json", 0) != 0)
      return send_error(res, 415, "json_required");
    json j = json::parse(*body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return send_error(res, 400, "invalid_json");
    handler(res, *r, j, aborted);
  });
}

// Runs slow work (Argon2 hashing) on a thread, then `done` back on the loop thread.
// Limited to kMaxConcurrentHashing at once (503 "busy" otherwise).
void WebServer::offload(Res *res, std::shared_ptr<bool> aborted,
                        std::function<std::string()> work,
                        std::function<void(std::string)> done) {
  if (hashing_ >= kMaxConcurrentHashing) return send_error(res, 503, "busy");
  ++hashing_;
  std::thread([this, res, aborted, work = std::move(work), done = std::move(done)]() mutable {
    std::string result = work();
    loop_->defer([this, res, aborted, result = std::move(result), done = std::move(done)]() {
      --hashing_;
      if (*aborted) return;
      res->cork([&] { done(result); });
    });
  }).detach();
}

// ---------------------------------------------------------------------------
// Live data
// ---------------------------------------------------------------------------
void WebServer::on_snapshot(std::string bytes) {
  if (!latest_.ParseFromString(bytes)) return;
  has_latest_ = true;
  std::unordered_set<uint32_t> masks;
  for (auto *ws : sockets_) masks.insert(ws->getUserData()->mask);
  for (uint32_t mask : masks) {
    std::string out;
    if (!filter(latest_, mask).SerializeToString(&out)) continue;
    app_.publish(topic(mask), out, uWS::OpCode::BINARY);
  }

  std::unordered_map<uint64_t, std::string> events; // one encode per mask + format
  for (auto &[res, c] : sse_) {
    if (c.notify_only) continue;
    uint64_t key = (uint64_t(c.mask) << 1) | uint64_t(c.json);
    auto it = events.find(key);
    if (it == events.end())
      it = events.emplace(key, sse_event(filter(latest_, c.mask), c.json)).first;
    if (!it->second.empty()) sse_write(res, c, it->second, false);
  }
}

// Writes one chunk to an SSE stream. Snapshots are full state, so under
// backpressure only the newest one is kept and sent once the socket drains;
// heartbeats (droppable) are simply skipped.
void WebServer::sse_write(Res *res, SseClient &c, const std::string &chunk, bool droppable) {
  if (c.congested) {
    if (!droppable) c.pending = chunk;
    return;
  }
  bool ok = true;
  res->cork([&] { ok = res->write(chunk); });
  if (!ok) c.congested = true; // uWS closes the stream if it stays stuck for 10 s
}

// Timer: keeps SSE streams alive through Cloudflare.
void WebServer::heartbeat(struct us_timer_t *t) {
  auto *self = *(WebServer **)us_timer_ext(t);
  for (auto &[res, c] : self->sse_) self->sse_write(res, c, ": ping\n\n", true);
}

// GET /api/events: Server-Sent Events fallback for browsers without WebSocket.
void WebServer::events_route() {
  app_.get("/api/events", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    if (sse_.size() >= kMaxSseClients) return send_error(res, 503, "too_many_streams");

    auto &c = sse_[res];
    c.user = u->name;
    c.session_hash = session_key(r);
    c.mask = u->mask();
    c.json = req->getQuery("format") == "json";

    res->onAborted([this, res] { sse_.erase(res); });
    // With onWritable set, uWS only drains buffered data when we write, so
    // always write something here (the held-back snapshot or a comment).
    res->onWritable([this, res](uintmax_t) {
      auto it = sse_.find(res);
      if (it == sse_.end()) return true;
      auto &cl = it->second;
      std::string chunk = cl.pending.empty() ? std::string(": drain\n\n") : std::move(cl.pending);
      cl.pending.clear();
      bool ok = res->write(chunk);
      cl.congested = !ok;
      return ok;
    });

    res->cork([&] {
      res->writeStatus("200 OK");
      security_headers(res);
      // no-transform + X-Accel-Buffering: ask proxies not to buffer/compress.
      res->writeHeader("Content-Type", "text/event-stream")
          ->writeHeader("Cache-Control", "no-store, no-transform")
          ->writeHeader("X-Accel-Buffering", "no");
      res->write("retry: 3000\n\n");
    });
    if (has_latest_) {
      std::string ev = sse_event(filter(latest_, c.mask), c.json);
      if (!ev.empty()) sse_write(res, c, ev, false);
    }
  });
}

// Sends the latest snapshot, filtered to this socket's permissions.
void WebServer::send_filtered(WS *ws) {
  if (!has_latest_) return;
  std::string out;
  if (!filter(latest_, ws->getUserData()->mask).SerializeToString(&out)) return;
  ws->send(out, uWS::OpCode::BINARY);
}

// Closing makes the client reconnect, which re-checks session + permissions.
void WebServer::close_sockets(const std::string &user, const std::string &session_hash) {
  std::vector<WS *> victims;
  for (auto *ws : sockets_) {
    auto *d = ws->getUserData();
    if (d->user == user && (session_hash.empty() || d->session_hash == session_hash))
      victims.push_back(ws);
  }
  for (auto *ws : victims) ws->end(4001, "session changed");

  // SSE clients reconnect on their own and get re-authenticated.
  std::vector<Res *> streams;
  for (auto &[res, c] : sse_)
    if (c.user == user && (session_hash.empty() || c.session_hash == session_hash))
      streams.push_back(res);
  for (auto *res : streams) {
    sse_.erase(res);
    res->end();
  }
}

// /ws: binary protobuf snapshots out, Commands in. Also registers login/user routes.
void WebServer::websocket() {
  app_.ws<SocketData>(
      "/ws",
      {.compression = uWS::DISABLED,
       .maxPayloadLength = 16 * 1024,
       .idleTimeout = 120,
       .sendPingsAutomatically = true,
       .upgrade =
           [this](Res *res, uWS::HttpRequest *req, struct us_socket_context_t *context) {
             Req r = capture(res, req);
             if (!origin_ok(r)) {
               res->writeStatus("403 Forbidden")->end();
               return;
             }
             const auth::User *u = current_user(r);
             if (!u) {
               res->writeStatus("401 Unauthorized")->end();
               return;
             }
             res->template upgrade<SocketData>(
                 {u->name, session_key(r), u->mask()},
                 req->getHeader("sec-websocket-key"), req->getHeader("sec-websocket-protocol"),
                 req->getHeader("sec-websocket-extensions"), context);
           },
       .open =
           [this](WS *ws) {
             sockets_.insert(ws);
             ws->subscribe(topic(ws->getUserData()->mask));
             send_filtered(ws);
           },
       .message =
           [this](WS *ws, std::string_view msg, uWS::OpCode op) {
             if (op != uWS::OpCode::BINARY) return;
             dash::Command cmd;
             if (!cmd.ParseFromArray(msg.data(), int(msg.size()))) return;
             uint32_t need = required_perm(cmd);
             if (need && !(ws->getUserData()->mask & need)) {
               std::cerr << "[WS] " << ws->getUserData()->user
                         << " tried a command without permission\n";
               return;
             }
             if (!prepare_command(cmd, ws->getUserData()->user)) return;
             commands_.push(std::move(cmd));
           },
       .close = [this](WS *ws, int, std::string_view) { sockets_.erase(ws); }});
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------
void WebServer::routes() {
  app_.get("/healthz", [](Res *res, auto *) { res->end("ok"); });

  // --- session ---
  app_.get("/api/me", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (const auth::User *u = current_user(r)) return send_json(res, 200, user_json(*u));
    users_.reload_if_changed();
    send_error(res, 401, "unauthenticated", {{"setup", users_.empty()}});
  });

  app_.post("/api/login", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
      login(res, r, j, ab);
    });
  });

  app_.post("/api/logout", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &, std::shared_ptr<bool>) {
      if (auto name = sessions_.lookup(r.token)) {
        close_sockets(*name, auth::sha256_hex(r.token));
        sessions_.revoke(r.token);
      }
      send_json(res, 204, {}, clear_cookie(cfg_.cookie_secure));
    });
  });

  app_.post("/api/me/password", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
      change_own_password(res, r, j, ab);
    });
  });

  // --- admin: users ---
  auto admin_only = [this](Res *res, const Req &r) -> const auth::User * {
    const auth::User *u = current_user(r);
    if (!u) send_error(res, 401, "unauthenticated");
    else if (!u->admin) send_error(res, 403, "forbidden");
    else return u;
    return nullptr;
  };

  app_.get("/api/users", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (!admin_only(res, r)) return;
    json list = json::array();
    for (const auto &u : users_.all()) list.push_back(user_json(u));
    json perms = json::array();
    for (const auto &[name, bit] : auth::perm_names()) perms.push_back(name);
    send_json(res, 200, {{"users", list}, {"permissions", perms}});
  });

  app_.post("/api/users", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this, admin_only](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
      if (admin_only(res, r)) create_user(res, r, j, ab);
    });
  });

  app_.put("/api/users/:name", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    std::string name(req->getParameter(0));
    with_json_body(res, req,
                   [this, admin_only, name](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
                     if (admin_only(res, r)) update_user(res, r, name, j, ab);
                   });
  });

  app_.del("/api/users/:name", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    std::string name(req->getParameter(0));
    with_json_body(res, req, [this, admin_only, name](Res *res, Req &r, json &, std::shared_ptr<bool>) {
      const auth::User *me = admin_only(res, r);
      if (!me) return;
      if (name == me->name) return send_error(res, 409, "cannot_delete_self");
      if (!users_.remove(name)) return send_error(res, 404, "not_found");
      std::string err;
      if (!users_.save(err)) return send_error(res, 500, "save_failed");
      sessions_.revoke_user(name);
      collector_.remove_user_devices(name);
      close_sockets(name);
      std::cout << "[Auth] user deleted: " << name << "\n";
      send_json(res, 204, {});
    });
  });

  // --- data ---
  app_.get("/api/dashboard", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (!admin_only(res, r)) return;
    std::string out;
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    (void)google::protobuf::util::MessageToJsonString(latest_, &out, opts);
    res->writeStatus("200 OK");
    security_headers(res);
    res->writeHeader("Cache-Control", "no-store")
        ->writeHeader("Content-Type", "application/json")
        ->end(out);
  });

  // HTTP fallback for networks whose proxy/firewall blocks WebSockets
  // (school / corporate wifi). Same payloads and checks as /ws.
  app_.get("/api/snapshot", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    if (!has_latest_) return send_json(res, 204, {});
    bool as_json = req->getQuery("format") == "json";
    std::string out;
    if (as_json) out = to_json(filter(latest_, u->mask()));
    else if (!filter(latest_, u->mask()).SerializeToString(&out)) out.clear();
    if (out.empty()) return send_error(res, 500, "encode_failed");
    res->writeStatus("200 OK");
    security_headers(res);
    res->writeHeader("Cache-Control", "no-store")
        ->writeHeader("Content-Type", as_json ? "application/json" : "application/x-protobuf")
        ->end(out);
  });

  app_.post("/api/command", [this](Res *res, uWS::HttpRequest *req) {
    auto aborted = std::make_shared<bool>(false);
    auto body = std::make_shared<std::string>();
    auto too_big = std::make_shared<bool>(false);
    auto r = std::make_shared<Req>(capture(res, req));
    res->onAborted([aborted] { *aborted = true; });
    res->onData([=, this](std::string_view chunk, bool last) {
      if (body->size() + chunk.size() > kMaxBody) *too_big = true;
      else body->append(chunk);
      if (!last || *aborted) return;
      if (*too_big) return send_error(res, 413, "body_too_large");
      if (!origin_ok(*r)) return send_error(res, 403, "bad_origin");
      // Non-simple content types: cross-site pages can't send them without CORS.
      bool is_json = r->content_type.rfind("application/json", 0) == 0;
      if (!is_json && r->content_type.rfind("application/x-protobuf", 0) != 0)
        return send_error(res, 415, "protobuf_required");
      const auth::User *u = current_user(*r);
      if (!u) return send_error(res, 401, "unauthenticated");
      dash::Command cmd;
      if (is_json) {
        google::protobuf::util::JsonParseOptions opts;
        opts.ignore_unknown_fields = true;
        if (!google::protobuf::util::JsonStringToMessage(*body, &cmd, opts).ok())
          return send_error(res, 400, "invalid_command");
      } else if (!cmd.ParseFromArray(body->data(), int(body->size()))) {
        return send_error(res, 400, "invalid_command");
      }
      uint32_t need = required_perm(cmd);
      if (need && !(u->mask() & need)) return send_error(res, 403, "forbidden");
      if (!prepare_command(cmd, u->name)) return send_error(res, 400, "invalid_command");
      commands_.push(std::move(cmd));
      send_json(res, 204, {});
    });
  });

  app_.get("/api/cover/*", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    if (!(u->mask() & auth::MEDIA)) return send_error(res, 403, "forbidden");
    std::string entity(req->getUrl().substr(std::string_view("/api/cover/").size()));
    auto cover = covers_.get(entity);
    if (!cover) return send_error(res, 404, "not_found");
    res->writeStatus("200 OK");
    security_headers(res);
    res->writeHeader("Content-Type", cover->mime)
        ->writeHeader("Cache-Control", "private, max-age=604800, immutable")
        ->end(cover->bytes);
  });

  // Unknown API routes / methods: JSON 404 instead of the SPA.
  app_.get("/api/*", [](Res *res, auto *) { send_error(res, 404, "not_found"); });
  app_.any("/api/*", [](Res *res, auto *) { send_error(res, 404, "not_found"); });

  if (!cfg_.static_dir.empty()) {
    fs::path root = fs::absolute(cfg_.static_dir);
    app_.get("/*", [root](Res *res, uWS::HttpRequest *req) { serve_static(root, res, req->getUrl()); });
  } else {
    app_.get("/*", [](Res *res, auto *) {
      res->writeStatus("200 OK");
      security_headers(res);
      res->writeHeader("Content-Type", "text/plain")
          ->end("Dashboard backend running. Set STATIC_DIR to serve the frontend.");
    });
  }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
void WebServer::login(Res *res, Req &r, json &j, std::shared_ptr<bool> aborted) {
  auto username = string_field(j, "username");
  auto password = string_field(j, "password");
  if (!username || !password || password->size() > 256) return send_error(res, 400, "invalid_input");
  std::string name = lower_trim(*username);

  // Lock out by IP and by account (whichever is stricter).
  std::string ip_key = "ip:" + r.ip, user_key = "user:" + name;
  int wait = std::max(limiter_.locked_for(ip_key), limiter_.locked_for(user_key));
  if (wait > 0) return send_error(res, 429, "too_many_attempts", {{"retry_after", wait}});

  users_.reload_if_changed();
  const auth::User *u = users_.find(name);
  std::string hash = (u && !u->disabled) ? u->hash : "";
  std::string pw = *password;

  offload(
      res, aborted,
      [hash, pw]() -> std::string {
        if (hash.empty()) {
          auth::verify_dummy(pw); // same timing for unknown users
          return "0";
        }
        return auth::verify_password(hash, pw) ? "1" : "0";
      },
      [this, res, name, ip_key, user_key](std::string ok) {
        const auth::User *u = users_.find(name);
        if (ok != "1" || !u || u->disabled) {
          limiter_.failure(ip_key);
          limiter_.failure(user_key);
          std::cerr << "[Auth] failed login for '" << name << "'\n";
          return send_error(res, 401, "invalid_credentials");
        }
        limiter_.success(ip_key);
        limiter_.success(user_key);
        std::string token = sessions_.create(u->name);
        std::cout << "[Auth] login: " << u->name << "\n";
        send_json(res, 200, user_json(*u), session_cookie(token, cfg_.session_days, cfg_.cookie_secure));
      });
}

// POST /api/me/password.
void WebServer::change_own_password(Res *res, Req &r, json &j, std::shared_ptr<bool> aborted) {
  const auth::User *u = current_user(r);
  if (!u) return send_error(res, 401, "unauthenticated");
  auto current = string_field(j, "current");
  auto next = string_field(j, "new");
  if (!current || !next || current->size() > 256) return send_error(res, 400, "invalid_input");
  if (auto problem = auth::password_problem(*next))
    return send_error(res, 400, "weak_password", {{"detail", *problem}});

  std::string name = u->name, hash = u->hash, cur = *current, nxt = *next;
  offload(
      res, aborted,
      [hash, cur, nxt]() -> std::string {
        if (!auth::verify_password(hash, cur)) return "";
        return auth::hash_password(nxt);
      },
      [this, res, name](std::string new_hash) {
        auth::User *u = users_.find_mut(name);
        if (!u) return send_error(res, 401, "unauthenticated");
        if (new_hash.empty()) return send_error(res, 403, "wrong_password");
        u->hash = new_hash;
        std::string err;
        if (!users_.save(err)) return send_error(res, 500, "save_failed");
        // Log out every other device, keep this one signed in.
        sessions_.revoke_user(name);
        close_sockets(name);
        std::string token = sessions_.create(name);
        std::cout << "[Auth] password changed: " << name << "\n";
        send_json(res, 204, {}, session_cookie(token, cfg_.session_days, cfg_.cookie_secure));
      });
}

// POST /api/users (admin).
void WebServer::create_user(Res *res, Req &, json &j, std::shared_ptr<bool> aborted) {
  auto username = string_field(j, "username");
  auto password = string_field(j, "password");
  if (!username || !password) return send_error(res, 400, "invalid_input");
  std::string name = lower_trim(*username);
  if (!auth::valid_username(name)) return send_error(res, 400, "invalid_username");
  if (auto problem = auth::password_problem(*password))
    return send_error(res, 400, "weak_password", {{"detail", *problem}});
  if (users_.find(name)) return send_error(res, 409, "user_exists");

  auth::User u;
  u.name = name;
  if (j.contains("displayName")) {
    auto dn = string_field(j, "displayName");
    if (!dn) return send_error(res, 400, "invalid_display_name");
    if (dn->find_first_not_of(" \t\r\n") != std::string::npos) {
      auto clean = auth::clean_display_name(*dn);
      if (!clean) return send_error(res, 400, "invalid_display_name");
      u.display_name = *clean;
    }
  }
  u.admin = j.value("admin", false) == true;
  u.perms = perms_field(j).value_or(auth::kDefaultPerms);
  std::string pw = *password;
  offload(
      res, aborted, [pw] { return auth::hash_password(pw); },
      [this, res, u](std::string hash) mutable {
        if (hash.empty()) return send_error(res, 500, "hash_failed");
        if (users_.find(u.name)) return send_error(res, 409, "user_exists");
        u.hash = hash;
        users_.upsert(u);
        std::string err;
        if (!users_.save(err)) return send_error(res, 500, "save_failed");
        std::cout << "[Auth] user created: " << u.name << (u.admin ? " (admin)" : "") << "\n";
        send_json(res, 201, user_json(*users_.find(u.name)));
      });
}

// PUT /api/users/:name (admin): password, display name, admin flag, disabled, permissions.
void WebServer::update_user(Res *res, Req &r, const std::string &name, json &j,
                            std::shared_ptr<bool> aborted) {
  const auth::User *me = current_user(r);
  const auth::User *target = users_.find(name);
  if (!target) return send_error(res, 404, "not_found");

  // Validate everything before changing anything.
  std::optional<bool> admin, disabled;
  if (j.contains("admin")) {
    if (!j["admin"].is_boolean()) return send_error(res, 400, "invalid_input");
    admin = j["admin"].get<bool>();
  }
  if (j.contains("disabled")) {
    if (!j["disabled"].is_boolean()) return send_error(res, 400, "invalid_input");
    disabled = j["disabled"].get<bool>();
  }
  auto perms = perms_field(j);
  auto password = string_field(j, "password");
  if (password)
    if (auto problem = auth::password_problem(*password))
      return send_error(res, 400, "weak_password", {{"detail", *problem}});
  // displayName: "" resets to the default (capitalized username).
  std::optional<std::string> display_name;
  if (j.contains("displayName")) {
    auto dn = string_field(j, "displayName");
    if (!dn) return send_error(res, 400, "invalid_display_name");
    if (dn->find_first_not_of(" \t\r\n") == std::string::npos) {
      display_name = "";
    } else {
      auto clean = auth::clean_display_name(*dn);
      if (!clean) return send_error(res, 400, "invalid_display_name");
      display_name = *clean;
    }
  }

  bool self = me && me->name == name;
  if (self && ((admin && !*admin) || (disabled && *disabled)))
    return send_error(res, 409, "cannot_lock_out_self");

  bool want_password = password.has_value();
  auto apply = [this, res, name, admin, disabled, perms, display_name,
                want_password](std::string new_hash) {
    if (want_password && new_hash.empty()) return send_error(res, 500, "hash_failed");
    auth::User *u = users_.find_mut(name);
    if (!u) return send_error(res, 404, "not_found");
    bool revoke = false;
    if (admin) u->admin = *admin;
    if (disabled) {
      revoke |= *disabled && !u->disabled;
      u->disabled = *disabled;
    }
    if (perms) u->perms = *perms;
    if (display_name) u->display_name = *display_name;
    if (!new_hash.empty()) {
      u->hash = new_hash;
      revoke = true;
    }
    if (users_.admin_count() == 0) { // never leave the dashboard without an admin
      std::string err;
      users_.load(err); // roll back from disk
      return send_error(res, 409, "last_admin");
    }
    std::string err;
    if (!users_.save(err)) return send_error(res, 500, "save_failed");
    if (revoke) sessions_.revoke_user(name);
    close_sockets(name); // reconnect picks up the new permissions
    std::cout << "[Auth] user updated: " << name << "\n";
    send_json(res, 200, user_json(*users_.find(name)));
  };

  if (password) {
    std::string pw = *password;
    offload(res, aborted, [pw] { return auth::hash_password(pw); }, apply);
  } else {
    apply("");
  }
}

// ---------------------------------------------------------------------------
// iOS app: device login (also used for the native dashboard) + data upload
// ---------------------------------------------------------------------------
namespace {
constexpr size_t kMaxUpload = 8 * 1024 * 1024;
}

// Stable id of the current login: SHA-256 of the cookie token or the Bearer key.
std::string WebServer::session_key(const Req &r) {
  if (!r.token.empty()) return auth::sha256_hex(r.token);
  constexpr std::string_view prefix = "Bearer ";
  if (r.authorization.size() > prefix.size())
    return auth::sha256_hex(r.authorization.substr(prefix.size()));
  return {};
}

const auth::User *WebServer::bearer_user(const Req &r, Collector::Device *device) {
  constexpr std::string_view prefix = "Bearer ";
  if (r.authorization.size() <= prefix.size() ||
      std::string_view(r.authorization).substr(0, prefix.size()) != prefix)
    return nullptr;
  auto d = collector_.device_for_token(r.authorization.substr(prefix.size()));
  if (!d) return nullptr;
  users_.reload_if_changed();
  const auth::User *u = users_.find(d->user);
  // Disabling a user locks their devices out at once.
  if (!u || u->disabled) return nullptr;
  if (device) *device = *d;
  return u;
}

// HomeCollect upload API: /api/collect, /api/collect/status.
void WebServer::collect_routes() {
  // Username + password (once) -> long-lived device token for the app.
  app_.post("/api/devices/login", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
      device_login(res, r, j, ab);
    });
  });

  // The app signs itself out: forget this device's token.
  app_.post("/api/devices/logout", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &, std::shared_ptr<bool>) {
      Collector::Device d;
      if (!bearer_user(r, &d)) return send_error(res, 401, "unauthenticated");
      collector_.remove_device(d.id);
      close_sockets(d.user, session_key(r)); // end its live stream
      std::cout << "[Collect] device signed out: " << d.user << "/" << d.name << "\n";
      send_json(res, 204, {});
    });
  });

  app_.post("/api/collect", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(
        res, req,
        [this](Res *res, Req &r, json &j, std::shared_ptr<bool>) {
          Collector::Device d;
          const auth::User *u = bearer_user(r, &d);
          if (!u) return send_error(res, 401, "unauthenticated");
          if (!(u->mask() & auth::COLLECT)) return send_error(res, 403, "forbidden");
          std::string err;
          json counts = collector_.ingest(u->name, j, err);
          if (!err.empty()) {
            std::cerr << "[Collect] rejected upload from " << d.user << "/" << d.name << ": "
                      << err << "\n";
            return send_error(res, err.rfind("storage", 0) == 0 ? 503 : 400, err);
          }
          refresh_summary(u->name);
          send_json(res, 200, counts);
        },
        kMaxUpload);
  });

  app_.get("/api/collect/status", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    Collector::Device d;
    const auth::User *u = bearer_user(r, &d);
    if (!u) return send_error(res, 401, "unauthenticated");
    if (!(u->mask() & auth::COLLECT)) return send_error(res, 403, "forbidden");
    json out = collector_.status(u->name);
    out["user"] = u->name;
    out["deviceName"] = d.name;
    send_json(res, 200, out);
  });

  app_routes();
  claude_routes();
  notification_routes();
}

namespace {
// ?days=N: absent/empty -> 30, otherwise 1..365 digits only.
std::optional<int> days_param(std::string_view v) {
  if (v.empty()) return 30;
  if (v.size() > 3) return std::nullopt;
  int n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return std::nullopt;
    n = n * 10 + (c - '0');
  }
  if (n < 1 || n > 365) return std::nullopt;
  return n;
}

struct Date {
  int y = 0, m = 0, d = 0;
};
// Parses ?date=YYYY-MM-DD.
std::optional<Date> date_param(std::string_view v) {
  Date dt;
  if (!Collector::valid_date(v, dt.y, dt.m, dt.d)) return std::nullopt;
  return dt;
}

int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

// iOS app routes: /api/history/*, /api/prefs, /api/presence, /api/home, /api/briefing, /api/sleep-pattern.
void WebServer::app_routes() {
  // Logged in (cookie or device token) with the iPhone-data permission.
  auto collect_user = [this](Res *res, const Req &r) -> const auth::User * {
    const auth::User *u = current_user(r);
    if (!u) {
      send_error(res, 401, "unauthenticated");
      return nullptr;
    }
    if (!(u->mask() & auth::COLLECT)) {
      send_error(res, 403, "forbidden");
      return nullptr;
    }
    return u;
  };

  // ?user=<name>: admins may read another user's history, everyone else
  // only their own (a foreign name is refused, not silently ignored).
  auto subject = [this, collect_user](Res *res, const Req &r,
                                      const std::string &who) -> std::optional<std::string> {
    const auth::User *u = collect_user(res, r);
    if (!u) return std::nullopt;
    if (who.empty() || who == u->name) return u->name;
    if (!u->admin) {
      send_error(res, 403, "forbidden");
      return std::nullopt;
    }
    const auth::User *t = users_.find(who);
    if (!t) {
      send_error(res, 404, "unknown_user");
      return std::nullopt;
    }
    return t->name;
  };

  // --- admins: whose history can be viewed (everyone but yourself) ---
  app_.get("/api/history/users", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *me = current_user(r);
    if (!me) return send_error(res, 401, "unauthenticated");
    if (!me->admin) return send_error(res, 403, "forbidden");
    auto with_data = collector_.users_with_data();
    json list = json::array();
    for (const auto &u : users_.all()) {
      if (u.name == me->name) continue;
      bool has = std::find(with_data.begin(), with_data.end(), u.name) != with_data.end();
      list.push_back({{"name", u.name},
                      {"displayName", u.spoken_name()},
                      {"hasData", has},
                      {"collect", bool(u.mask() & auth::COLLECT)},
                      {"disabled", u.disabled}});
    }
    send_json(res, 200, {{"users", list}});
  });

  // --- history (own, or another user's for admins) ---
  using RangeFn = json (Collector::*)(const std::string &, int);
  auto per_range = [this, subject](const char *path, RangeFn fn) {
    app_.get(path, [this, subject, fn](Res *res, uWS::HttpRequest *req) {
      auto days = days_param(req->getQuery("days"));
      std::string who(req->getQuery("user"));
      Req r = capture(res, req);
      auto name = subject(res, r, who);
      if (!name) return;
      if (!days) return send_error(res, 400, "invalid_days");
      send_json(res, 200, (collector_.*fn)(*name, *days));
    });
  };
  per_range("/api/history/daily", &Collector::history_daily);
  per_range("/api/history/workouts", &Collector::history_workouts);
  per_range("/api/history/places", &Collector::history_places);
  using DayFn = json (Collector::*)(const std::string &, int, int, int);
  auto per_day = [this, subject](const char *path, DayFn fn) {
    app_.get(path, [this, subject, fn](Res *res, uWS::HttpRequest *req) {
      auto date = date_param(req->getQuery("date"));
      std::string who(req->getQuery("user"));
      Req r = capture(res, req);
      auto name = subject(res, r, who);
      if (!name) return;
      if (!date) return send_error(res, 400, "invalid_date");
      send_json(res, 200, (collector_.*fn)(*name, date->y, date->m, date->d));
    });
  };
  per_day("/api/history/heartrate", &Collector::history_heartrate);
  per_day("/api/history/sleep", &Collector::history_sleep);
  per_day("/api/history/locations", &Collector::history_locations);

  // --- settings of the app (any logged-in user) ---
  app_.get("/api/prefs", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    send_json(res, 200, prefs_.get(u->name).to_json());
  });
  app_.put("/api/prefs", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool>) {
      const auth::User *u = current_user(r);
      if (!u) return send_error(res, 401, "unauthenticated");
      Prefs p = prefs_.get(u->name);
      std::string err;
      if (!p.merge(j, err)) return send_error(res, 400, "invalid_prefs", {{"detail", err}});
      if (!prefs_.set(u->name, p, err)) {
        std::cerr << "[Prefs] cannot save: " << err << "\n";
        return send_error(res, 503, "storage_error");
      }
      send_json(res, 200, p.to_json());
    });
  });

  // --- iPhone entered / left the home region ---
  app_.post("/api/presence", [this, collect_user](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this, collect_user](Res *res, Req &r, json &j,
                                                  std::shared_ptr<bool>) {
      const auth::User *u = collect_user(res, r);
      if (!u) return;
      auto event = string_field(j, "event");
      if (!event || (*event != "arrive" && *event != "leave"))
        return send_error(res, 400, "invalid_event");
      auto t = j.find("t");
      // Unix ms; Swift may send it as a double.
      if (t == j.end() || !t->is_number() || !std::isfinite(t->get<double>()) ||
          t->get<double>() <= 0 || t->get<double>() > double(unix_ms_now() + 60000))
        return send_error(res, 400, "invalid_time");
      int64_t ms = int64_t(t->get<double>());
      bool arrive = *event == "arrive";
      bus_.push_presence({u->name, arrive, ms});
      std::cout << "[Presence] " << u->name << " " << *event << "\n";
      dash::Command wake;
      wake.set_refresh(true);
      commands_.push(std::move(wake)); // worker handles the event now
      send_json(res, 204, {});
    });
  });

  app_.get("/api/home", [this, collect_user](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (!collect_user(res, r)) return;
    auto z = bus_.home_zone();
    if (!z) return send_json(res, 204, {});
    send_json(res, 200, {{"lat", (*z)[0]}, {"lon", (*z)[1]}, {"radius", (*z)[2]}});
  });

  // --- computed by the worker (automations) ---
  app_.get("/api/briefing", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    auto b = bus_.briefing(u->name);
    if (!b) return send_json(res, 204, {});
    send_json(res, 200, *b);
  });
  app_.get("/api/sleep-pattern", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    auto p = bus_.sleep_pattern(u->name);
    if (!p) return send_json(res, 204, {});
    send_json(res, 200, *p);
  });
}

// POST /api/devices/login: username + password -> new device key (Bearer) for the iOS app.
void WebServer::device_login(Res *res, Req &r, json &j, std::shared_ptr<bool> aborted) {
  auto username = string_field(j, "username");
  auto password = string_field(j, "password");
  auto device = string_field(j, "device");
  if (!username || !password || password->size() > 256) return send_error(res, 400, "invalid_input");
  std::string name = lower_trim(*username);
  std::string dev_name = device ? device->substr(0, 64) : "iPhone";
  for (char &c : dev_name)
    if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';

  // Same brute-force protection as the web login (shared counters).
  std::string ip_key = "ip:" + r.ip, user_key = "user:" + name;
  int wait = std::max(limiter_.locked_for(ip_key), limiter_.locked_for(user_key));
  if (wait > 0) return send_error(res, 429, "too_many_attempts", {{"retry_after", wait}});

  users_.reload_if_changed();
  const auth::User *u = users_.find(name);
  std::string hash = (u && !u->disabled) ? u->hash : "";
  std::string pw = *password;

  offload(
      res, aborted,
      [hash, pw]() -> std::string {
        if (hash.empty()) {
          auth::verify_dummy(pw);
          return "0";
        }
        return auth::verify_password(hash, pw) ? "1" : "0";
      },
      [this, res, name, dev_name, ip_key, user_key](std::string ok) {
        const auth::User *u = users_.find(name);
        if (ok != "1" || !u || u->disabled) {
          limiter_.failure(ip_key);
          limiter_.failure(user_key);
          std::cerr << "[Auth] failed device login for '" << name << "'\n";
          return send_error(res, 401, "invalid_credentials");
        }
        limiter_.success(ip_key);
        limiter_.success(user_key);
        std::string id;
        std::string token = collector_.add_device(u->name, dev_name, id);
        if (token.empty()) return send_error(res, 503, "storage_unavailable");
        std::cout << "[Collect] device registered: " << u->name << "/" << dev_name << " (" << id
                  << ")\n";
        send_json(res, 200, {{"token", token}, {"user", u->name}, {"deviceId", id}});
      });
}

// New upload -> fresh summary for the dashboard and Home Assistant.
void WebServer::refresh_summary(const std::string &user) {
  dashboard_backend::Activity a;
  if (!collector_.summary(user, a)) return;
  collected_.update(user, std::move(a));
  dash::Command wake;
  wake.set_refresh(true);
  commands_.push(std::move(wake)); // worker rebuilds the snapshot now
}

// ---------------------------------------------------------------------------
// Claude tab (iOS app, admins only)
//
// The backend runs in Docker as `nobody`; Claude Code is installed and logged
// in on the host. A small bridge on the host (scripts/claude-bridge.py,
// 127.0.0.1 only, shared secret) runs `claude -p` jobs; we only check the
// login and forward. Jobs run in the background on the bridge and the app
// polls them, so long tasks survive proxy timeouts (Cloudflare: 100 s).
// ---------------------------------------------------------------------------
namespace {
// Bridge job ids: 32 lowercase hex chars (nothing else ends up in the URL).
bool valid_job_id(std::string_view id) {
  if (id.size() != 32) return false;
  for (char c : id)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool digits_only(std::string_view v) {
  if (v.empty() || v.size() > 9) return false;
  for (char c : v)
    if (c < '0' || c > '9') return false;
  return true;
}
} // namespace

// /api/claude/*: admin-only proxy to scripts/claude-bridge.py (CLAUDE_BRIDGE_URL + token).
void WebServer::claude_routes() {
  auto admin_user = [this](Res *res, const Req &r) -> const auth::User * {
    const auth::User *u = current_user(r);
    if (!u) send_error(res, 401, "unauthenticated");
    else if (!u->admin) send_error(res, 403, "forbidden");
    else return u;
    return nullptr;
  };

  // Forwards one request to the bridge on a helper thread (blocking curl)
  // and relays its JSON answer. `body` empty = GET.
  auto forward = [this](Res *res, std::shared_ptr<bool> aborted, std::string path,
                        std::string body) {
    if (!cfg_.claude_enabled()) return send_error(res, 503, "claude_disabled");
    std::string url = cfg_.claude_bridge_url + path;
    std::string auth = "Authorization: Bearer " + cfg_.claude_bridge_token;
    offload(
        res, aborted,
        [url, auth, body]() -> std::string {
          HttpClient http;
          std::vector<std::string> headers{auth, "Content-Type: application/json"};
          HttpResponse r = body.empty() ? http.get(url, headers) : http.post(url, body, headers);
          return json{{"status", r.status}, {"body", r.body}, {"error", r.error}}.dump();
        },
        [res](std::string raw) {
          json r = json::parse(raw, nullptr, false);
          long status = r.value("status", 0L);
          if (status == 0) {
            std::cerr << "[Claude] bridge unreachable: " << r.value("error", "") << "\n";
            return send_error(res, 502, "bridge_unreachable");
          }
          json payload = json::parse(r.value("body", ""), nullptr, false);
          if (payload.is_discarded()) return send_error(res, 502, "bridge_bad_response");
          int code = (status >= 200 && status < 600) ? int(status) : 502;
          send_json(res, code == 204 ? 200 : code, payload);
        });
  };

  // Is the tab usable at all? (The app hides nothing; it shows a hint.)
  app_.get("/api/claude/status", [this, admin_user, forward](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (!admin_user(res, r)) return;
    if (!cfg_.claude_enabled()) return send_json(res, 200, {{"enabled", false}});
    auto aborted = std::make_shared<bool>(false);
    res->onAborted([aborted] { *aborted = true; });
    forward(res, aborted, "/status", "");
  });

  // New prompt: {"prompt": "...", "session": "<id to continue>" (optional)}.
  app_.post("/api/claude/jobs", [this, admin_user, forward](Res *res, uWS::HttpRequest *req) {
    with_json_body(
        res, req,
        [this, admin_user, forward](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
          const auth::User *u = admin_user(res, r);
          if (!u) return;
          auto prompt = string_field(j, "prompt");
          if (!prompt || prompt->find_first_not_of(" \t\r\n") == std::string::npos)
            return send_error(res, 400, "empty_prompt");
          if (prompt->size() > 32 * 1024) return send_error(res, 413, "prompt_too_long");
          json out = {{"prompt", *prompt}, {"user", u->name}};
          if (auto session = string_field(j, "session"); session && !session->empty()) {
            if (session->size() > 64) return send_error(res, 400, "invalid_session");
            out["session"] = *session; // the bridge checks the format
          }
          std::cout << "[Claude] prompt from " << u->name << " (" << prompt->size()
                    << " chars)\n";
          forward(res, ab, "/jobs", out.dump());
        },
        64 * 1024);
  });

  // Progress of a job: GET /api/claude/jobs/<id>?after=<event index>.
  app_.get("/api/claude/jobs/:id", [this, admin_user, forward](Res *res, uWS::HttpRequest *req) {
    std::string id(req->getParameter(0));
    std::string after(req->getQuery("after"));
    Req r = capture(res, req);
    if (!admin_user(res, r)) return;
    if (!valid_job_id(id)) return send_error(res, 400, "invalid_job");
    if (!after.empty() && !digits_only(after)) return send_error(res, 400, "invalid_after");
    auto aborted = std::make_shared<bool>(false);
    res->onAborted([aborted] { *aborted = true; });
    forward(res, aborted, "/jobs/" + id + (after.empty() ? "" : "?after=" + after), "");
  });

  app_.post("/api/claude/jobs/:id/cancel", [this, admin_user, forward](Res *res,
                                                                      uWS::HttpRequest *req) {
    std::string id(req->getParameter(0));
    with_json_body(res, req,
                   [admin_user, forward, id](Res *res, Req &r, json &, std::shared_ptr<bool> ab) {
                     if (!admin_user(res, r)) return;
                     if (!valid_job_id(id)) return send_error(res, 400, "invalid_job");
                     forward(res, ab, "/jobs/" + id + "/cancel", "{}");
                   });
  });
}

// ---------------------------------------------------------------------------
// Notifications for the HomeCollect app
//
// The automations put every notification into the inbox (AutomationBus).
// The app keeps GET /api/notifications/stream open (also in the background
// while iOS lets it run) and gets `event: notification` at once; after a
// suspension it catches up with GET /api/notifications?after=<last id>.
// ---------------------------------------------------------------------------
void WebServer::on_notification(const std::string &user, const json &n) {
  std::string ev = "event: notification\nid: " + std::to_string(n.value("id", int64_t{0})) +
                   "\ndata: " + n.dump() + "\n\n";
  for (auto &[res, c] : sse_)
    if (c.notify_only && c.user == user) sse_write(res, c, ev, false);
}

// /api/notifications[/stream|/action|/test]: in-app notification inbox for HomeCollect.
void WebServer::notification_routes() {
  app_.get("/api/notifications", [this](Res *res, uWS::HttpRequest *req) {
    std::string after(req->getQuery("after"));
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    int64_t a = 0;
    if (!after.empty()) {
      if (after.size() > 15 || after.find_first_not_of("0123456789") != std::string::npos)
        return send_error(res, 400, "invalid_after");
      a = std::stoll(after);
    }
    send_json(res, 200, bus_.inbox.since(u->name, a));
  });

  // Live notifications only (no dashboard snapshots: cheap in the background).
  app_.get("/api/notifications/stream", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    if (sse_.size() >= kMaxSseClients) return send_error(res, 503, "too_many_streams");
    auto &c = sse_[res];
    c.user = u->name;
    c.session_hash = session_key(r);
    c.mask = u->mask();
    c.json = true;
    c.notify_only = true;
    res->onAborted([this, res] { sse_.erase(res); });
    res->onWritable([this, res](uintmax_t) {
      auto it = sse_.find(res);
      if (it == sse_.end()) return true;
      auto &cl = it->second;
      std::string chunk = cl.pending.empty() ? std::string(": drain\n\n") : std::move(cl.pending);
      cl.pending.clear();
      bool ok = res->write(chunk);
      cl.congested = !ok;
      return ok;
    });
    res->cork([&] {
      res->writeStatus("200 OK");
      security_headers(res);
      res->writeHeader("Content-Type", "text/event-stream")
          ->writeHeader("Cache-Control", "no-store, no-transform")
          ->writeHeader("X-Accel-Buffering", "no");
      res->write("retry: 3000\n\n");
    });
  });

  // Test notification. Body (all optional): {"user", "title", "message"}.
  // Everyone may send one to themselves; admins to anyone.
  app_.post("/api/notifications/test", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool>) {
      const auth::User *u = current_user(r);
      if (!u) return send_error(res, 401, "unauthenticated");
      std::string to = string_field(j, "user").value_or(u->name);
      if (to.empty()) to = u->name;
      if (to != u->name) {
        if (!u->admin) return send_error(res, 403, "forbidden");
        if (!users_.find(to)) return send_error(res, 404, "unknown_user");
      }
      auto text = [&](const char *key, size_t max) -> std::optional<std::string> {
        auto v = string_field(j, key);
        if (!v || v->find_first_not_of(" \t\r\n") == std::string::npos) return std::string();
        if (v->size() > max) return std::nullopt;
        return *v;
      };
      auto title = text("title", 100);
      auto message = text("message", 1000);
      if (!title || !message) return send_error(res, 400, "too_long");
      bool de = prefs_.get(to).language != "en";
      if (title->empty()) *title = "Test";
      if (message->empty())
        *message = to == u->name ? (de ? "Mitteilungen vom Dashboard funktionieren."
                                       : "Notifications from the dashboard work.")
                                 : (de ? "Test von " : "Test from ") + u->spoken_name();
      json n = bus_.inbox.add(to, *title, *message, {{"tag", "dash-test"}});
      std::cout << "[Notify] test from " << u->name << " to " << to << "\n";
      send_json(res, 200, n);
    });
  });

  // Button in a notification of the app ("Licht aus").
  app_.post("/api/notifications/action", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool>) {
      const auth::User *u = current_user(r);
      if (!u) return send_error(res, 401, "unauthenticated");
      auto action = string_field(j, "action");
      if (!action) return send_error(res, 400, "invalid_action");
      auto cmd = Automations::command_for_action(*action);
      if (!cmd) return send_error(res, 400, "unknown_action");
      uint32_t need = required_perm(*cmd);
      if (need && !(u->mask() & need)) return send_error(res, 403, "forbidden");
      std::cout << "[Notify] action " << *action << " from " << u->name << "\n";
      commands_.push(std::move(*cmd));
      send_json(res, 204, {});
    });
  });
}
