// Program entry. Two modes:
//   dashboard_backend --<command> ...  -> admin CLI (cli(): users, devices, backups, previews), then exit
//   dashboard_backend                  -> server: WebServer (uWS loop, main thread) + worker thread
//                                         + HaEventStream thread + backup thread.
// The worker owns HomeAssistant: polls data, runs Commands from clients, Voice and Automations,
// and publishes each new protobuf snapshot to WebServer::publish().

#include "auth.h"
#include "automations.h"
#include "backup.h"
#include "collected_state.h"
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
#include <cmath>
#include <set>
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
// HomeCollect -> Home Assistant: sensor.homecollect_<user>_* + device_tracker
// (REST states: they show up in HA at once, but are gone after an HA restart
// until the next upload from the app.)
// ---------------------------------------------------------------------------
double distance_m(double lat1, double lon1, double lat2, double lon2) {
  constexpr double R = 6371000, rad = M_PI / 180;
  double dlat = (lat2 - lat1) * rad, dlon = (lon2 - lon1) * rad;
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(lat1 * rad) * std::cos(lat2 * rad) * std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2 * R * std::asin(std::sqrt(a));
}

// Writes the HomeCollect summary of `user` to HA as sensor.homecollect_<user>_* and
// device_tracker.homecollect_<user> (home/not_home via zone.home).
void publish_to_ha(HomeAssistant &ha, const std::string &user, const dash::Activity &a) {
  std::string slug;
  for (char c : user) slug += std::isalnum((unsigned char)c) ? c : '_';
  std::string label = user;
  if (!label.empty()) label[0] = char(std::toupper((unsigned char)label[0]));
  int errors = 0;
  std::string err;
  auto sensor = [&](const char *key, const char *name, double value, const char *unit,
                    const char *icon, const char *device_class = nullptr,
                    const char *state_class = "measurement", nlohmann::json extra = {}) {
    nlohmann::json attrs = {{"friendly_name", label + " " + name},
                            {"unit_of_measurement", unit},
                            {"icon", icon},
                            {"state_class", state_class},
                            {"source", "HomeCollect"}};
    if (device_class) attrs["device_class"] = device_class;
    if (extra.is_object()) attrs.update(extra);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", value);
    std::string state = buf;
    while (state.find('.') != std::string::npos && (state.back() == '0' || state.back() == '.'))
      state.pop_back();
    if (!ha.set_state("sensor.homecollect_" + slug + "_" + key, state, attrs, err)) ++errors;
  };
  if (a.has_steps()) sensor("steps", "Steps today", a.steps(), "steps", "mdi:walk", nullptr, "total_increasing");
  if (a.has_active_kcal()) sensor("active_energy", "Active energy today", a.active_kcal(), "kcal", "mdi:fire", nullptr, "total_increasing");
  if (a.has_distance_km()) sensor("distance", "Distance today", a.distance_km(), "km", "mdi:map-marker-distance", "distance", "total_increasing");
  if (a.has_exercise_min()) sensor("exercise", "Exercise today", a.exercise_min(), "min", "mdi:run", "duration", "total_increasing");
  if (a.has_flights()) sensor("flights", "Floors today", a.flights(), "floors", "mdi:stairs-up", nullptr, "total_increasing");
  if (a.has_heart_rate()) sensor("heart_rate", "Heart rate", a.heart_rate(), "bpm", "mdi:heart-pulse");
  if (a.has_resting_hr()) sensor("resting_heart_rate", "Resting heart rate", a.resting_hr(), "bpm", "mdi:heart");
  if (a.has_hrv_ms()) sensor("hrv", "Heart rate variability", a.hrv_ms(), "ms", "mdi:heart-flash");
  if (a.has_vo2max()) sensor("vo2_max", "VO2 max", a.vo2max(), "mL/(kg·min)", "mdi:lungs");
  if (a.has_battery()) sensor("battery", "iPhone battery", std::lround(a.battery() * 100), "%", "mdi:battery", "battery", "measurement", {{"battery_state", a.battery_state()}});
  if (a.has_sleep()) {
    const auto &s = a.sleep();
    sensor("sleep", "Sleep last night", s.total(), "min", "mdi:sleep", "duration", "measurement",
           {{"deep", s.deep()}, {"rem", s.rem()}, {"core", s.core()}, {"awake", s.awake()}});
  }
  if (a.has_last_workout()) {
    const auto &w = a.last_workout();
    nlohmann::json attrs = {{"friendly_name", label + " Last workout"},
                            {"icon", "mdi:basketball"},
                            {"duration_min", std::lround(w.duration_s() / 60.0)},
                            {"end", w.end()},
                            {"source", "HomeCollect"}};
    if (w.has_kcal()) attrs["kcal"] = std::lround(w.kcal());
    if (w.has_distance_km()) attrs["distance_km"] = std::round(w.distance_km() * 100) / 100;
    if (!ha.set_state("sensor.homecollect_" + slug + "_last_workout",
                      w.activity().empty() ? "workout" : w.activity(), attrs, err))
      ++errors;
  }
  if (a.has_location()) {
    const auto &g = a.location();
    std::string state = "not_home";
    if (auto zone = ha.home_zone())
      if (distance_m(g.lat(), g.lon(), (*zone)[0], (*zone)[1]) <= (*zone)[2] + g.accuracy())
        state = "home";
    nlohmann::json attrs = {{"friendly_name", label + " (HomeCollect)"},
                            {"latitude", g.lat()},
                            {"longitude", g.lon()},
                            {"gps_accuracy", g.has_accuracy() ? g.accuracy() : 0},
                            {"source_type", "gps"},
                            {"last_seen", g.time()},
                            {"icon", "mdi:cellphone-marker"}};
    if (a.has_battery()) attrs["battery_level"] = std::lround(a.battery() * 100);
    if (!ha.set_state("device_tracker.homecollect_" + slug, state, attrs, err)) ++errors;
  }
  if (errors) std::cerr << "[Collect] Home Assistant: " << errors << " update(s) failed, e.g. " << err << "\n";
}

// Puts the HomeCollect summary into the snapshot. The app reads HealthKit
// directly, so its values replace the HA companion sensors.
void merge_activity(const Config &cfg, const std::map<std::string, dash::Activity> &all,
                    HomeAssistant &ha, dash::Dashboard &snap) {
  const dash::Activity *pick = nullptr;
  if (!cfg.collect_dashboard_user.empty()) {
    auto it = all.find(cfg.collect_dashboard_user);
    if (it != all.end()) pick = &it->second;
  } else {
    for (const auto &[user, a] : all)
      if (!pick || a.synced_at() > pick->synced_at()) pick = &a;
  }
  if (!pick) return;
  dash::Activity a = *pick;
  if (a.has_location())
    if (auto zone = ha.home_zone())
      a.mutable_location()->set_home(distance_m(a.location().lat(), a.location().lon(), (*zone)[0],
                                                (*zone)[1]) <= (*zone)[2] + a.location().accuracy());
  auto *phone = snap.mutable_phone();
  if (a.has_steps()) phone->set_steps(a.steps());
  if (a.has_heart_rate()) phone->set_heart_rate(a.heart_rate());
  if (!phone->has_battery() && a.has_battery()) phone->set_battery(std::lround(a.battery() * 100));
  if (a.has_sleep() && a.sleep().total() > 0) *snap.mutable_sleep() = a.sleep();
  *snap.mutable_activity() = std::move(a);
}

// ---------------------------------------------------------------------------
// Worker: collects data, executes commands, hands snapshots to the loop
// ---------------------------------------------------------------------------
void worker(const Config &cfg, CommandQueue &commands, CoverCache &covers,
            CollectedState &collected, PrefsStore &prefs, AutomationBus &bus,
            std::function<void(std::string)> publish) {
  HomeAssistant ha(cfg, covers);
  SystemStats sys(cfg.disk_path);
  Voice voice(cfg, ha);
  Automations automations(cfg, ha, prefs, bus);

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
    std::map<std::string, dash::Activity> activity;
    {
      std::set<std::string> changed;
      activity = collected.take(changed);
      if (cfg.ha_enabled() && cfg.collect_to_ha)
        for (const auto &user : changed) publish_to_ha(ha, user, activity[user]);
      if (cfg.ha_enabled()) merge_activity(cfg, activity, ha, snap);
      else if (!activity.empty()) {
        // without HA there is no home zone; still show the data
        auto it = activity.begin();
        *snap.mutable_activity() = it->second;
      }
    }
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
    // Notifications, morning push, presence, sleep-pattern experiment.
    if (cfg.ha_enabled()) automations.tick(snap, activity);

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
//   dashboard_backend --set-name <name> "<display name>"   ("" = default)
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

// Admin CLI commands (see usage text in main()). Returns the process exit code.
int cli(const Config &cfg, const std::string &cmd, const std::string &name,
        const std::string &arg = "") {
  auth::UserStore users(fs::path(cfg.data_dir) / "users.json");
  std::string err;
  if (!users.load(err)) {
    std::cerr << err << "\n";
    return 1;
  }
  if (cmd == "--list-users") {
    for (const auto &u : users.all())
      std::cout << u.name << "  \"" << u.spoken_name() << "\"" << (u.admin ? "  [admin]" : "")
                << (u.disabled ? "  [disabled]" : "") << "\n";
    return 0;
  }
  if (!auth::valid_username(name)) {
    std::cerr << "invalid user name (lowercase a-z, 0-9, _ . -, max 32)\n";
    return 1;
  }
  const auth::User *existing = users.find(name);
  if (cmd == "--set-name") {
    if (!existing) {
      std::cerr << "no such user: " << name << "\n";
      return 1;
    }
    auth::User u = *existing;
    if (arg.find_first_not_of(" \t") == std::string::npos) {
      u.display_name.clear();
    } else {
      auto clean = auth::clean_display_name(arg);
      if (!clean) {
        std::cerr << "invalid display name (1-40 characters, no < > & \")\n";
        return 1;
      }
      u.display_name = *clean;
    }
    users.upsert(u);
    if (!users.save(err)) {
      std::cerr << err << "\n";
      return 1;
    }
    std::cout << name << " is now called \"" << u.spoken_name() << "\"\n";
    return 0;
  }
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

// Parses config from env, runs the CLI if an argument is given, else starts all threads.
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
    if (cmd == "--create-admin" || cmd == "--set-password" || cmd == "--list-users" ||
        cmd == "--set-name")
      return cli(cfg, cmd, argc >= 3 ? argv[2] : "", argc >= 4 ? argv[3] : "");
    if (cmd == "--backup-now") {
      std::string err;
      if (!backup_now(cfg, err)) {
        std::cerr << "[Backup] failed: " << err << "\n";
        return 1;
      }
      return 0;
    }
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
    if (cmd == "--sleep-pattern") {
      // Dry run of the sleep-pattern experiment + briefing: reads HA and
      // collect.db, never switches or sends anything.
      CoverCache covers;
      HomeAssistant ha(cfg, covers);
      SystemStats sys(cfg.disk_path);
      PrefsStore prefs(fs::path(cfg.data_dir) / "prefs.json");
      AutomationBus bus;
      dash::Dashboard snap;
      std::vector<std::string> errors;
      sys.fill(snap.mutable_server());
      if (cfg.ha_enabled()) ha.fill(snap, errors);
      for (const auto &e : errors) snap.add_errors(e);
      std::cout << Automations(cfg, ha, prefs, bus).dry_run(snap, argc >= 3 ? argv[2] : "");
      return 0;
    }
    std::cerr << "usage: " << argv[0]
              << " [--create-admin <name> | --set-password <name> | --list-users |\n"
              << "   --set-name <name> \"<display name>\" |\n"
                 "    --list-devices | --revoke-device <id> | --voice-preview | --backup-now |\n"
                 "    --sleep-pattern [user]]\n";
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
  CollectedState collected;
  PrefsStore prefs(fs::path(cfg.data_dir) / "prefs.json");
  AutomationBus bus;
  WebServer server(cfg, commands, covers, collected, prefs, bus);
  start_backup_thread(cfg);

  std::thread([&] {
    worker(cfg, commands, covers, collected, prefs, bus,
           [&](std::string b) { server.publish(std::move(b)); });
  })
      .detach();

  // Live updates: HA pushes state changes over its WebSocket API; each one
  // wakes the worker for an immediate refresh. Polling stays as a fallback.
  if (cfg.ha_enabled()) {
    std::thread([&] {
      HaEventStream events(
          cfg,
          [&] {
            dash::Command refresh;
            refresh.set_refresh(true);
            commands.push(std::move(refresh));
          },
          // Button in one of our push notifications ("Licht aus").
          [&](const std::string &action) {
            if (auto cmd = Automations::command_for_action(action)) {
              std::cout << "[Auto] notification action " << action << "\n";
              commands.push(std::move(*cmd));
            }
          });
      events.run();
    }).detach();
  }

  server.run();
  return 0;
}
