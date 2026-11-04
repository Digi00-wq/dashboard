#include "auth.h"
#include "collector.h"
#include "command_queue.h"
#include "config.h"
#include "cover_cache.h"
#include "dashboard.pb.h"
#include "ha_events.h"
#include "home_assistant.h"
#include "system_stats.h"
#include "voice.h"
#include "web.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
namespace dash = dashboard_backend;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Announcements from the dashboard ("Essen ist fertig!") -> spoken on the Echo
// ---------------------------------------------------------------------------
bool announce(const Config &cfg, HomeAssistant &ha, const dash::Announce &a, std::string &error) {
  static Clock::time_point last{};
  if (!cfg.voice_enabled()) {
    error = "announcement: voice not configured (HA_ALEXA_ENTITY / HA_ALEXA_NOTIFY)";
    return false;
  }
  if (Clock::now() - last < std::chrono::seconds(3)) return true; // anti-spam: drop silently
  // Printable text only, max ~200 characters (UTF-8 bytes kept intact).
  std::string text;
  size_t chars = 0;
  for (unsigned char c : a.text()) {
    if (c < 0x20 || c == 0x7f) c = ' ';
    if ((c & 0xC0) != 0x80 && ++chars > 200) break;
    text += char(c);
  }
  auto b = text.find_first_not_of(' '), e = text.find_last_not_of(' ');
  if (b == std::string::npos) return true;
  text = text.substr(b, e - b + 1);
  std::string from = a.from();
  if (!from.empty()) from[0] = char(std::toupper((unsigned char)from[0]));
  last = Clock::now();
  std::cout << "[Voice] announcement from " << a.from() << ": " << text << "\n";
  return ha.speak(from.empty() ? text : from + ": " + text, error);
}

// ---------------------------------------------------------------------------
// Worker: collects data, executes commands, hands snapshots to the loop
// ---------------------------------------------------------------------------
void worker(const Config &cfg, CommandQueue &commands, CoverCache &covers,
            std::function<void(std::string)> publish) {
  HomeAssistant ha(cfg, covers);
  SystemStats sys(cfg.disk_path);
  Voice voice(cfg, ha);

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
    snap.set_voice_available(cfg.voice_enabled());
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

    // Alexa voice commands, morning briefing, wake-up light.
    if (cfg.ha_enabled()) voice.tick(snap);

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
      case dash::Command::kAnnounce:
        ok = announce(cfg, ha, cmd.announce(), err);
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
// Admin CLI: create the first admin / reset a password without the web UI.
//   dashboard_backend --create-admin <name>
//   dashboard_backend --set-password <name>
//   dashboard_backend --list-users
//   dashboard_backend --list-devices | --revoke-device <id>   (HomeCollect app)
// ---------------------------------------------------------------------------
std::string read_password(const char *prompt) {
  std::cerr << prompt << std::flush;
  termios old{};
  bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
  if (tty) {
    termios noecho = old;
    noecho.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
  }
  std::string pw;
  std::getline(std::cin, pw);
  if (tty) {
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::cerr << "\n";
  }
  return pw;
}

int cli(const Config &cfg, const std::string &cmd, const std::string &name) {
  auth::UserStore users(fs::path(cfg.data_dir) / "users.json");
  std::string err;
  if (!users.load(err)) {
    std::cerr << err << "\n";
    return 1;
  }
  if (cmd == "--list-users") {
    for (const auto &u : users.all())
      std::cout << u.name << (u.admin ? "  [admin]" : "") << (u.disabled ? "  [disabled]" : "")
                << "\n";
    return 0;
  }
  if (!auth::valid_username(name)) {
    std::cerr << "invalid user name (lowercase a-z, 0-9, _ . -, max 32)\n";
    return 1;
  }
  const auth::User *existing = users.find(name);
  if (cmd == "--set-password" && !existing) {
    std::cerr << "no such user: " << name << "\n";
    return 1;
  }
  if (cmd == "--create-admin" && existing) {
    std::cerr << "user already exists (use --set-password)\n";
    return 1;
  }
  std::string pw = read_password("Password: ");
  if (auto problem = auth::password_problem(pw)) {
    std::cerr << *problem << "\n";
    return 1;
  }
  if (read_password("Repeat password: ") != pw) {
    std::cerr << "passwords do not match\n";
    return 1;
  }
  auth::User u = existing ? *existing : auth::User{};
  u.name = name;
  u.hash = auth::hash_password(pw);
  if (cmd == "--create-admin") u.admin = true;
  if (u.hash.empty()) {
    std::cerr << "hashing failed\n";
    return 1;
  }
  users.upsert(u);
  if (!users.save(err)) {
    std::cerr << err << "\n";
    return 1;
  }
  // A (re)set password logs that user out everywhere.
  auth::Sessions sessions(fs::path(cfg.data_dir) / "sessions.json", std::chrono::hours(1));
  sessions.load();
  sessions.revoke_user(name);
  std::cout << (cmd == "--create-admin" ? "Admin created: " : "Password set: ") << name
            << "\nStored in " << (fs::path(cfg.data_dir) / "users.json").string() << "\n";
  return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  std::cout << std::unitbuf; // flush log lines immediately (pipes, docker logs)
  const Config cfg = Config::from_env();
  if (!auth::init()) {
    std::cerr << "libsodium init failed\n";
    return 1;
  }

  if (argc >= 2) {
    std::string cmd = argv[1];
    if (cmd == "--create-admin" || cmd == "--set-password" || cmd == "--list-users")
      return cli(cfg, cmd, argc >= 3 ? argv[2] : "");
    if (cmd == "--list-devices" || cmd == "--revoke-device") {
      Collector collector(fs::path(cfg.data_dir) / "collect.db");
      std::string err;
      if (!collector.open(err)) {
        std::cerr << err << "\n";
        return 1;
      }
      if (cmd == "--list-devices") {
        for (const auto &d : collector.devices())
          std::cout << d.id << "  " << d.user << "  " << d.name << "  last seen: "
                    << (d.last_seen ? std::to_string((time(nullptr) * 1000 - d.last_seen) / 60000) +
                                          " min ago"
                                    : "never")
                    << "\n";
        return 0;
      }
      if (argc < 3 || !collector.remove_device(argv[2])) {
        std::cerr << "no such device id\n";
        return 1;
      }
      std::cout << "Device revoked: " << argv[2] << "\n";
      return 0;
    }
    if (cmd == "--voice-preview") {
      CoverCache covers;
      HomeAssistant ha(cfg, covers);
      SystemStats sys(cfg.disk_path);
      dash::Dashboard snap;
      std::vector<std::string> errors;
      sys.fill(snap.mutable_server());
      ha.fill(snap, errors);
      snap.clear_server();
      sys.fill(snap.mutable_server()); // second sample -> CPU %
      for (const auto &e : errors) snap.add_errors(e);
      std::cout << Voice(cfg, ha).preview(snap);
      return 0;
    }
    std::cerr << "usage: " << argv[0]
              << " [--create-admin <name> | --set-password <name> | --list-users |\n"
                 "    --list-devices | --revoke-device <id> | --voice-preview]\n";
    return 2;
  }

  std::cout << "[Config] Home Assistant: "
            << (cfg.ha_enabled() ? cfg.ha_url : "not configured") << "\n"
            << "[Config] Static dir: " << (cfg.static_dir.empty() ? "(none)" : cfg.static_dir)
            << "\n"
            << "[Config] Data dir: " << fs::absolute(cfg.data_dir).string() << "\n";
  if (cfg.trust_proxy && cfg.bind_address != "127.0.0.1" && cfg.bind_address != "::1")
    std::cerr << "[Config] WARNING: TRUST_PROXY=1 while listening on " << cfg.bind_address
              << " - clients on your network can fake their IP. Use BIND_ADDRESS=127.0.0.1.\n";

  CommandQueue commands;
  CoverCache covers;
  WebServer server(cfg, commands, covers);

  std::thread([&] { worker(cfg, commands, covers, [&](std::string b) { server.publish(std::move(b)); }); })
      .detach();

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

  server.run();
  return 0;
}
