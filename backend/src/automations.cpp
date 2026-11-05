#include "automations.h"

#include "auth.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dash = dashboard_backend;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int64_t kMin = 60 * 1000, kHour = 60 * kMin, kDay = 24 * kHour;

int64_t real_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::tm local(int64_t ms) {
  std::time_t t = ms / 1000;
  std::tm tm{};
  localtime_r(&t, &tm);
  return tm;
}

std::string date_of(int64_t ms) {
  std::tm tm = local(ms);
  char buf[11];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

int minute_of_day(int64_t ms) {
  std::tm tm = local(ms);
  return tm.tm_hour * 60 + tm.tm_min;
}

// Local time `minutes` after midnight of the day `ms` falls on, plus `day_offset` days.
int64_t local_at(int64_t ms, int minutes, int day_offset = 0) {
  std::tm tm = local(ms);
  tm.tm_mday += day_offset;
  tm.tm_hour = minutes / 60;
  tm.tm_min = minutes % 60;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;
  return int64_t(std::mktime(&tm)) * 1000;
}

std::string hm(int minutes) {
  char buf[8];
  minutes = ((minutes % 1440) + 1440) % 1440;
  std::snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}
std::string hm_at(int64_t ms) { return hm(minute_of_day(ms)); }

std::string duration_text(int64_t ms) {
  int64_t m = std::max<int64_t>(0, ms / kMin);
  if (m < 120) return std::to_string(m) + " min";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld h %02lld min", (long long)(m / 60), (long long)(m % 60));
  return buf;
}

std::string sleep_hm(int minutes) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%d:%02d", minutes / 60, minutes % 60);
  return buf;
}

std::string capitalize(std::string s) {
  if (!s.empty()) s[0] = char(std::toupper((unsigned char)s[0]));
  return s;
}

std::string lower(std::string s) {
  for (auto &c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

double distance_m(double lat1, double lon1, double lat2, double lon2) {
  constexpr double R = 6371000, rad = M_PI / 180;
  double dlat = (lat2 - lat1) * rad, dlon = (lon2 - lon1) * rad;
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(lat1 * rad) * std::cos(lat2 * rad) * std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2 * R * std::asin(std::sqrt(a));
}

const std::map<std::string, std::pair<const char *, const char *>> kWeather = {
    {"clear-night", {"klar", "clear"}},
    {"cloudy", {"bewölkt", "cloudy"}},
    {"exceptional", {"Unwetter", "severe weather"}},
    {"fog", {"neblig", "foggy"}},
    {"hail", {"Hagel", "hail"}},
    {"lightning", {"Gewitter", "thunderstorm"}},
    {"lightning-rainy", {"Gewitter mit Regen", "thunderstorm with rain"}},
    {"partlycloudy", {"teilweise bewölkt", "partly cloudy"}},
    {"pouring", {"Starkregen", "heavy rain"}},
    {"rainy", {"Regen", "rain"}},
    {"snowy", {"Schnee", "snow"}},
    {"snowy-rainy", {"Schneeregen", "sleet"}},
    {"sunny", {"sonnig", "sunny"}},
    {"windy", {"windig", "windy"}},
    {"windy-variant", {"windig", "windy"}},
};

std::string weather_word(const std::string &cond, bool de) {
  auto it = kWeather.find(cond);
  if (it == kWeather.end()) return cond;
  return de ? it->second.first : it->second.second;
}

// Room presence sensor (not a person / device tracker that follows the phone).
bool is_room_sensor(const std::string &entity) {
  return !(entity.rfind("person.", 0) == 0 || entity.rfind("device_tracker.", 0) == 0);
}

std::vector<std::string> lamps_on(const dash::Dashboard &snap, bool names) {
  std::vector<std::string> out;
  for (const auto &l : snap.lamps())
    if (l.available() && l.is_on()) out.push_back(names ? l.name() : l.entity_id());
  return out;
}

std::string join(const std::vector<std::string> &v, const char *sep = ", ") {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
  return out;
}

// Median of minute values; `shift` moves the day boundary (bedtimes around
// midnight are compared relative to noon).
std::optional<int> median_minutes(std::vector<int> v, int shift) {
  if (v.empty()) return std::nullopt;
  for (auto &m : v) m = (m + shift) % 1440;
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  int med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
  return ((med - shift) % 1440 + 1440) % 1440;
}

} // namespace

// ---------------------------------------------------------------------------
// Learned sleep pattern
// ---------------------------------------------------------------------------
json SleepPattern::to_json() const {
  auto kind = [](const Kind &k) {
    json j = {{"nights", k.nights}};
    if (k.bedtime_min) j["bedtime"] = hm(*k.bedtime_min);
    if (k.wake_min) j["wake"] = hm(*k.wake_min);
    return j;
  };
  return {{"weekday", kind(weekday)}, {"weekend", kind(weekend)}, {"updatedAt", updated_at}};
}

std::optional<SleepPattern> compute_sleep_pattern(const std::string &db_file,
                                                  const std::string &user, int64_t now,
                                                  std::string &error) {
  if (!fs::exists(db_file)) {
    error = "no " + db_file;
    return std::nullopt;
  }
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(db_file.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK) {
    error = std::string("collect.db: ") + (db ? sqlite3_errmsg(db) : "open failed");
    if (db) sqlite3_close(db);
    return std::nullopt;
  }
  sqlite3_busy_timeout(db, 2000);
  sqlite3_stmt *st = nullptr;
  const char *sql = "SELECT start_ms, end_ms, CAST(value AS INTEGER), COALESCE(source, '') "
                    "FROM health WHERE user = ? AND type = 'HKCategoryTypeIdentifierSleepAnalysis' "
                    "AND end_ms >= ? AND end_ms <= ? ORDER BY start_ms";
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
    error = std::string("collect.db: ") + sqlite3_errmsg(db);
    sqlite3_close(db);
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, user.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, now - 29 * kDay);
  sqlite3_bind_int64(st, 3, now);

  struct Seg {
    int64_t start, end;
  };
  // wake date -> source -> asleep segments. A night belongs to the day its
  // sleep ends on (ending after 18:00 counts for the next morning).
  std::map<std::string, std::map<std::string, std::vector<Seg>>> nights;
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    int64_t s = sqlite3_column_int64(st, 0), e = sqlite3_column_int64(st, 1);
    int stage = sqlite3_column_int(st, 2);
    if (!(stage == 1 || stage >= 3) || e <= s) continue; // asleep stages only
    auto src = reinterpret_cast<const char *>(sqlite3_column_text(st, 3));
    nights[date_of(e + 6 * kHour)][src ? src : ""].push_back({s, e});
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  if (rc != SQLITE_DONE) {
    error = "collect.db: read failed";
    return std::nullopt;
  }

  std::string oldest = date_of(now - 28 * kDay);
  std::vector<int> bed[2], wake[2]; // [0] weekday, [1] weekend
  for (auto &[date, sources] : nights) {
    if (date <= oldest) continue;
    int64_t best_asleep = 0, best_start = 0, best_end = 0;
    for (auto &[src, segs] : sources) {
      std::sort(segs.begin(), segs.end(), [](auto &a, auto &b) { return a.start < b.start; });
      // Main block: segments with gaps <= 2 h (naps later in the day are separate).
      int64_t bs = 0, be = 0, asleep = 0;
      auto close = [&] {
        if (asleep > best_asleep) {
          best_asleep = asleep;
          best_start = bs;
          best_end = be;
        }
      };
      for (size_t i = 0; i < segs.size(); ++i) {
        const auto &g = segs[i];
        if (i == 0 || g.start - be > 2 * kHour) {
          if (i) close();
          bs = be = g.start;
          asleep = 0;
        }
        // overlapping samples of the same source: count only the new part
        asleep += std::max<int64_t>(0, g.end - std::max(g.start, be));
        be = std::max(be, g.end);
      }
      if (!segs.empty()) close();
    }
    if (best_asleep < 3 * kHour) continue;
    std::tm w = local(best_end);
    int k = (w.tm_wday >= 1 && w.tm_wday <= 5) ? 0 : 1;
    bed[k].push_back(minute_of_day(best_start));
    wake[k].push_back(minute_of_day(best_end));
  }

  SleepPattern p;
  p.updated_at = now;
  SleepPattern::Kind *kinds[2] = {&p.weekday, &p.weekend};
  for (int k = 0; k < 2; ++k) {
    kinds[k]->nights = int(wake[k].size());
    if (wake[k].size() >= 3) { // fewer nights are no "pattern"
      kinds[k]->bedtime_min = median_minutes(bed[k], 720);
      kinds[k]->wake_min = median_minutes(wake[k], 0);
    }
  }
  return p;
}

// ---------------------------------------------------------------------------

Automations::Automations(const Config &cfg, HomeAssistant &ha, PrefsStore &prefs,
                         AutomationBus &bus)
    : cfg_(cfg), ha_(ha), prefs_(prefs), bus_(bus) {
  state_file_ = (fs::path(cfg_.data_dir) / "automations_state.json").string();
  load_state();
  if (const char *f = std::getenv("AUTO_FAKE_NOW_FILE"); f && *f)
    std::cerr << "[Auto] TEST MODE: time is read from " << f << "\n";
}

// Test hook: AUTO_FAKE_NOW_FILE=<file with Unix ms> replaces the clock of the
// automations (never set this in production).
int64_t Automations::now() const {
  static const char *f = std::getenv("AUTO_FAKE_NOW_FILE");
  if (f && *f) {
    std::ifstream in(f);
    long long v = 0;
    if (in >> v && v > 0) return v;
  }
  return real_now_ms();
}

void Automations::load_state() {
  state_ = json::object();
  std::ifstream in(state_file_);
  if (!in) return;
  try {
    in >> state_;
  } catch (...) {
    std::cerr << "[Auto] " << state_file_ << " unreadable, starting fresh\n";
  }
  if (!state_.is_object()) state_ = json::object();
}

void Automations::save_state() {
  std::error_code ec;
  fs::create_directories(fs::path(state_file_).parent_path(), ec);
  std::string tmp = state_file_ + ".tmp";
  {
    mode_t old = umask(0077);
    std::ofstream out(tmp, std::ios::trunc);
    umask(old);
    if (!out) {
      std::cerr << "[Auto] cannot write " << tmp << "\n";
      return;
    }
    out << state_.dump(2);
  }
  fs::rename(tmp, state_file_, ec);
  if (ec) std::cerr << "[Auto] cannot save state: " << ec.message() << "\n";
}

std::string Automations::owner() {
  int64_t n = real_now_ms();
  if (!owner_.empty() && n - owner_checked_ < 10 * kMin) return owner_;
  owner_checked_ = n;
  std::string preferred = !cfg_.owner_user.empty() ? cfg_.owner_user : cfg_.collect_dashboard_user;
  auto o = auth::resolve_owner(fs::path(cfg_.data_dir) / "users.json", preferred);
  if (!o.user.empty()) {
    owner_ = o.user;
    owner_name_ = o.spoken;
  }
  return owner_;
}

std::optional<dash::Command> Automations::command_for_action(const std::string &action) {
  if (action == "DASH_LIGHTS_OFF") {
    dash::Command c;
    c.set_all_lights(false);
    return c;
  }
  return std::nullopt;
}

bool Automations::ha_reachable(int64_t) {
  int64_t n = real_now_ms();
  if (n - reach_checked_ < 30000) return reachable_;
  reach_checked_ = n;
  std::string err;
  bool ok = ha_.ping(err);
  if (!ok && !warned_unreachable_)
    std::cerr << "[Auto] Home Assistant unreachable (" << err << ") - automations paused\n";
  if (ok && warned_unreachable_) std::cout << "[Auto] Home Assistant reachable again\n";
  warned_unreachable_ = !ok;
  return reachable_ = ok;
}

// Every notification goes to the owner's HomeCollect app (inbox + live
// stream); optionally also through the HA companion app as a backup.
bool Automations::notify(const Prefs &p, const std::string &title, const std::string &message,
                         const json &data) {
  bool delivered = false;
  if (!owner_.empty()) {
    json n = bus_.inbox.add(owner_, title, message, data);
    std::cout << "[Auto] notification #" << n.value("id", int64_t{0}) << " to " << owner_
              << ": " << title << " | " << message << "\n";
    delivered = true;
  }
  if (!p.notify_via_ha) return delivered;
  if (cfg_.notify_target.empty()) {
    if (!warned_no_target_)
      std::cout << "[Auto] HA_NOTIFY_TARGET not set - no copy via the HA app\n";
    warned_no_target_ = true;
    return delivered;
  }
  int64_t n = real_now_ms();
  if (n < retry_after_) return delivered;
  std::string err;
  if (!ha_.notify(title, message, data, err)) {
    std::cerr << "[Auto] HA push failed: " << err << "\n";
    retry_after_ = n + kMin;
    return delivered;
  }
  std::cout << "[Auto] HA push: " << title << "\n";
  return true;
}

void Automations::tick(const dash::Dashboard &snap,
                       const std::map<std::string, dash::Activity> &activity) {
  const int64_t now = this->now();
  if (auto z = ha_.home_zone()) bus_.set_home_zone(*z);

  const std::string user = owner();
  if (user.empty()) return; // no account yet
  const Prefs p = prefs_.get(user);
  const dash::Activity *act = nullptr;
  if (auto it = activity.find(user); it != activity.end()) act = &it->second;

  bool reachable = ha_reachable(now);
  sleep_pattern(user, now);
  server_issues(snap, p, now, reachable);
  morning(snap, act, p, now, reachable);
  if (!reachable) return; // never act on stale data
  presence(snap, p, now);
  lights_left_on(snap, p, now);
  wake_light(snap, act, p, now);
  bedtime(snap, p, now);
}

// ---------------------------------------------------------------------------
// 1a. Lights on, nobody in the room
// ---------------------------------------------------------------------------
void Automations::lights_left_on(const dash::Dashboard &snap, const Prefs &p, int64_t now) {
  if (!p.notify_lights_left_on || !snap.has_presence() || !snap.presence().has_is_now()) return;
  auto &st = state_["lights"];
  if (!st.is_object()) st = json::object();
  bool armed = st.value("armed", true);
  auto on = lamps_on(snap, true);
  if (on.empty() || snap.presence().is_now()) {
    if (!armed) {
      st["armed"] = true;
      save_state();
    }
    return;
  }
  if (!armed || !snap.presence().has_last_seen()) return;
  int64_t idle = now - int64_t(snap.presence().last_seen());
  if (idle < int64_t(p.lights_idle_min) * kMin) return;
  if (now - st.value("lastSent", int64_t(0)) < kHour) return;

  bool de = p.language != "en";
  std::string title = de ? "Licht brennt noch" : "Lights still on";
  std::string msg = de ? "Seit " + std::to_string(idle / kMin) + " min keine Bewegung im Zimmer (" +
                             join(on) + ")."
                       : "No motion in the room for " + std::to_string(idle / kMin) + " min (" +
                             join(on) + ").";
  json data = {{"tag", "dash-lights"},
               {"group", "dashboard"},
               {"push", {{"interruption-level", "time-sensitive"}}},
               {"actions", json::array({{{"action", "DASH_LIGHTS_OFF"},
                                         {"title", de ? "Licht aus" : "Lights off"}}})}};
  std::cout << "[Auto] lights on without motion for " << idle / kMin << " min: " << join(on) << "\n";
  if (notify(p, title, msg, data)) {
    st["armed"] = false;
    st["lastSent"] = now;
    save_state();
  }
}

// ---------------------------------------------------------------------------
// 1c. Server problems
// ---------------------------------------------------------------------------
namespace {
struct IssueDef {
  const char *key;
  int64_t delay;
  const char *title_de, *title_en, *ok_de, *ok_en;
};
const IssueDef kIssues[] = {
    {"disk", 0, "Server: Festplatte fast voll", "Server: disk almost full",
     "Server: Festplatte wieder OK", "Server: disk OK again"},
    {"ram", 5 * kMin, "Server: Arbeitsspeicher knapp", "Server: memory low",
     "Server: Arbeitsspeicher wieder OK", "Server: memory OK again"},
    {"temp", 5 * kMin, "Server: CPU zu heiss", "Server: CPU too hot",
     "Server: CPU-Temperatur wieder OK", "Server: CPU temperature OK again"},
    {"internet", 3 * kMin, "Internet ausgefallen", "Internet down", "Internet wieder da",
     "Internet back"},
    {"ha", 10 * kMin, "Home Assistant: Datenfehler", "Home Assistant: data errors",
     "Home Assistant wieder OK", "Home Assistant OK again"},
};

// nullopt = unknown (no data: keep the current state)
std::optional<bool> issue_bad(const char *key, const dash::Dashboard &snap, bool active) {
  const auto &sv = snap.server();
  std::string k = key;
  if (k == "disk") {
    if (!snap.has_server() || !sv.disk_total()) return std::nullopt;
    return double(sv.disk_used()) / sv.disk_total() > (active ? 0.88 : 0.90);
  }
  if (k == "ram") {
    if (!snap.has_server() || !sv.mem_total()) return std::nullopt;
    return double(sv.mem_used()) / sv.mem_total() > (active ? 0.90 : 0.92);
  }
  if (k == "temp") {
    if (!snap.has_server() || !sv.has_temperature()) return std::nullopt;
    return sv.temperature() > (active ? 77 : 80);
  }
  if (k == "internet") {
    if (!snap.has_glance() || !snap.glance().has_network() ||
        !snap.glance().network().has_online())
      return std::nullopt;
    return !snap.glance().network().online();
  }
  return snap.errors_size() > 0;
}

std::string issue_detail(const char *key, const dash::Dashboard &snap, bool de) {
  const auto &sv = snap.server();
  std::string k = key;
  char buf[160];
  if (k == "disk") {
    double pct = 100.0 * sv.disk_used() / std::max<uint64_t>(1, sv.disk_total());
    double free_gb = double(sv.disk_total() - sv.disk_used()) / 1e9;
    std::snprintf(buf, sizeof buf,
                  de ? "Festplatte zu %.0f %% belegt, noch %.1f GB frei." : "Disk %.0f %% used, %.1f GB free.",
                  pct, free_gb);
    return buf;
  }
  if (k == "ram") {
    double pct = 100.0 * sv.mem_used() / std::max<uint64_t>(1, sv.mem_total());
    std::snprintf(buf, sizeof buf,
                  de ? "Arbeitsspeicher seit 5 min zu %.0f %% belegt." : "Memory at %.0f %% for 5 min.",
                  pct);
    return buf;
  }
  if (k == "temp") {
    std::snprintf(buf, sizeof buf,
                  de ? "CPU-Temperatur seit 5 min über 80 °C (jetzt %.0f °C)."
                     : "CPU temperature above 80 °C for 5 min (now %.0f °C).",
                  sv.temperature());
    return buf;
  }
  if (k == "internet")
    return de ? "Seit 3 min keine Internetverbindung." : "No internet connection for 3 min.";
  std::string first = snap.errors_size() ? snap.errors(0) : "";
  if (first.size() > 120) first = first.substr(0, 120) + "...";
  std::string s = (de ? "Seit 10 min: " : "For 10 min: ") + first;
  if (snap.errors_size() > 1)
    s += " (+" + std::to_string(snap.errors_size() - 1) + (de ? " weitere)" : " more)");
  return s;
}
} // namespace

void Automations::server_issues(const dash::Dashboard &snap, const Prefs &p, int64_t now,
                                bool reachable) {
  auto &all = state_["issues"];
  if (!all.is_object()) all = json::object();
  bool de = p.language != "en";
  for (const auto &def : kIssues) {
    auto &is = all[def.key];
    if (!is.is_object()) is = json::object();
    bool active = is.value("active", false);
    auto bad = issue_bad(def.key, snap, active);
    if (!bad) continue;

    if (*bad) {
      issue_clear_since_.erase(def.key);
      auto since = issue_since_.emplace(def.key, now).first->second;
      if (!active && now - since >= def.delay) {
        active = true;
        bool allowed = p.notify_server && now - is.value("lastSent", int64_t(0)) >= 6 * kHour;
        is = {{"active", true}, {"since", since}, {"notified", false}, {"pending", allowed},
              {"lastSent", is.value("lastSent", int64_t(0))}};
        std::cout << "[Auto] server problem: " << def.key << " - " << issue_detail(def.key, snap, de)
                  << (allowed ? "" : " (no push: off or sent < 6 h ago)") << "\n";
        save_state();
      }
      if (active && is.value("pending", false) && reachable &&
          notify(p, de ? def.title_de : def.title_en, issue_detail(def.key, snap, de),
                 {{"tag", std::string("dash-server-") + def.key}, {"group", "dashboard"}})) {
        is["pending"] = false;
        is["notified"] = true;
        is["lastSent"] = now;
        save_state();
      }
      continue;
    }

    issue_since_.erase(def.key);
    if (!active) continue;
    auto clear_since = issue_clear_since_.emplace(def.key, now).first->second;
    if (now - clear_since < kMin) continue; // must stay OK for a minute
    // "wieder OK" only if the start was pushed (or was due but couldn't be
    // delivered, e.g. internet down -> say it now, with the duration).
    bool tell = p.notify_server && (is.value("notified", false) || is.value("pending", false));
    if (tell && !reachable) continue;
    int64_t started = is.value("since", now);
    std::string msg = de ? "Das Problem bestand " + duration_text(clear_since - started) +
                               " (" + hm_at(started) + " bis " + hm_at(clear_since) + ")."
                         : "The problem lasted " + duration_text(clear_since - started) + " (" +
                               hm_at(started) + " to " + hm_at(clear_since) + ").";
    if (tell && !notify(p, de ? def.ok_de : def.ok_en, msg,
                        {{"tag", std::string("dash-server-") + def.key}, {"group", "dashboard"}}))
      continue;
    std::cout << "[Auto] server problem cleared: " << def.key << "\n";
    is["active"] = false;
    is["notified"] = false;
    is["pending"] = false;
    issue_clear_since_.erase(def.key);
    save_state();
  }
}

std::vector<std::string> Automations::current_issues(const dash::Dashboard &snap,
                                                     const Prefs &p) const {
  std::vector<std::string> out;
  bool de = p.language != "en";
  const auto &sv = snap.server();
  auto active = [&](const char *k) {
    return state_.contains("issues") && state_["issues"].contains(k) &&
           state_["issues"][k].value("active", false);
  };
  char buf[96];
  if (snap.has_server() && sv.disk_total() && double(sv.disk_used()) / sv.disk_total() > 0.90) {
    std::snprintf(buf, sizeof buf, de ? "Festplatte %.0f %% belegt" : "Disk %.0f %% used",
                  100.0 * sv.disk_used() / sv.disk_total());
    out.push_back(buf);
  }
  if (active("ram")) out.push_back(de ? "Arbeitsspeicher knapp" : "Memory low");
  if (active("temp")) {
    std::snprintf(buf, sizeof buf, de ? "CPU %.0f °C" : "CPU %.0f °C", sv.temperature());
    out.push_back(buf);
  }
  if (snap.has_glance() && snap.glance().has_network() && snap.glance().network().has_online() &&
      !snap.glance().network().online())
    out.push_back(de ? "Internet offline" : "Internet offline");
  if (snap.errors_size() > 0)
    out.push_back(de ? "Home Assistant: " + std::to_string(snap.errors_size()) + " Fehler"
                     : "Home Assistant: " + std::to_string(snap.errors_size()) + " error(s)");
  return out;
}

// ---------------------------------------------------------------------------
// 2. Morning briefing
// ---------------------------------------------------------------------------
json Automations::briefing(const dash::Dashboard &snap, const dash::Activity *act, const Prefs &p,
                           int64_t now) const {
  bool de = p.language != "en";
  json b = {{"generatedAt", now}};
  // Display name of the owner (users.json), else VOICE_USER.
  std::string name = !owner_name_.empty() ? owner_name_ : !cfg_.voice_user.empty() ? cfg_.voice_user : capitalize(owner_);
  int mod = minute_of_day(now);
  std::string hello = mod < 11 * 60 ? (de ? "Guten Morgen" : "Good morning")
                      : mod < 18 * 60 ? (de ? "Guten Tag" : "Good afternoon")
                                      : (de ? "Guten Abend" : "Good evening");
  if (mod < 4 * 60) hello = de ? "Hallo" : "Hello";
  b["greeting"] = name.empty() ? hello : hello + ", " + name;

  std::vector<std::string> parts, tips;

  // Sleep: HomeCollect (HealthKit) first, else what the snapshot has.
  const dash::Sleep *sl = nullptr;
  if (act && act->has_sleep() && act->sleep().total() > 0) sl = &act->sleep();
  else if (snap.has_sleep() && snap.sleep().total() > 0) sl = &snap.sleep();
  if (sl) {
    b["sleep"] = {{"total", sl->total()}, {"deep", sl->deep()}, {"rem", sl->rem()},
                  {"core", sl->core()}, {"awake", sl->awake()}};
    parts.push_back(de ? sleep_hm(sl->total()) + " h Schlaf" : sleep_hm(sl->total()) + " h sleep");
    if (sl->total() < 6 * 60)
      tips.push_back(de ? "Kurze Nacht - nimm es heute etwas ruhiger."
                        : "Short night - take it easy today.");
  }

  // Weather now + today's forecast.
  if (snap.has_weather()) {
    const auto &w = snap.weather();
    json wj = json::object();
    std::string text;
    if (w.has_temperature()) {
      wj["temperature"] = std::round(double(w.temperature()) * 10) / 10;
      text = std::to_string(int(std::lround(w.temperature()))) + " °C";
    }
    if (!w.condition().empty()) {
      wj["condition"] = w.condition();
      text += (text.empty() ? "" : ", ") + weather_word(w.condition(), de);
    }
    if (w.forecast_size() > 0) {
      const auto &f = w.forecast(0);
      // first entry is today (daily forecast), unless it's an old one
      if (f.time() == 0 || date_of(int64_t(f.time())) >= date_of(now - 12 * kHour)) {
        if (f.has_temperature()) wj["high"] = std::round(double(f.temperature()) * 10) / 10;
        if (f.has_temp_low()) wj["low"] = std::round(double(f.temp_low()) * 10) / 10;
        if (f.has_precipitation_probability())
          wj["precipitationProbability"] = f.precipitation_probability();
        if (f.has_temperature() && f.has_temp_low())
          text += " (" + std::to_string(int(std::lround(f.temp_low()))) + "/" +
                  std::to_string(int(std::lround(f.temperature()))) + " °C)";
        if (f.has_precipitation_probability() && f.precipitation_probability() > 50)
          tips.push_back(de ? "Regenschirm mitnehmen (" + std::to_string(f.precipitation_probability()) +
                                  " % Regenrisiko)."
                            : "Take an umbrella (" + std::to_string(f.precipitation_probability()) +
                                  " % chance of rain).");
        if (f.has_temp_low() && f.temp_low() <= 0)
          tips.push_back(de ? "Frost möglich - warm anziehen." : "Possible frost - dress warmly.");
      }
    }
    if (!wj.empty()) b["weather"] = wj;
    if (!text.empty()) parts.push_back(text);
  }

  // Alarm (next one within 24 h).
  if (snap.has_glance() && snap.glance().has_next_alarm()) {
    int64_t a = int64_t(snap.glance().next_alarm());
    if (a > now && a - now < kDay) {
      b["alarm"] = a;
      if (date_of(a) == date_of(now))
        parts.push_back((de ? "Wecker " : "Alarm ") + hm_at(a));
    }
  }

  // Tasks due today (and overdue).
  std::string today = date_of(now);
  json tasks = json::array();
  for (const auto &list : snap.todoist().lists())
    for (const auto &t : list.task()) {
      if (t.due().size() < 10 || t.due().substr(0, 10) > today) continue;
      tasks.push_back({{"summary", t.summary()}, {"due", t.due().substr(0, 10)}, {"list", list.name()}});
    }
  b["tasks"] = tasks;
  if (snap.has_todoist()) {
    size_t n = tasks.size();
    parts.push_back(de ? (n == 1 ? "1 Aufgabe fällig" : std::to_string(n) + " Aufgaben fällig")
                       : (n == 1 ? "1 task due" : std::to_string(n) + " tasks due"));
    if (n > 6)
      tips.push_back(de ? "Viele Aufgaben - such dir die drei wichtigsten aus."
                        : "Lots of tasks - pick the three most important ones.");
  }

  // Server.
  auto issues = current_issues(snap, p);
  b["server"] = {{"ok", issues.empty()}, {"issues", issues}};
  parts.push_back(issues.empty() ? "Server OK"
                                 : (de ? "Server: " : "Server: ") + join(issues));

  // Activity.
  if (act) {
    json a = json::object();
    if (act->has_steps()) a["steps"] = act->steps();
    if (act->has_resting_hr()) a["restingHr"] = act->resting_hr();
    if (act->has_hrv_ms()) a["hrvMs"] = std::round(double(act->hrv_ms()) * 10) / 10;
    if (!a.empty()) b["activity"] = a;
  }

  b["summary"] = join(parts, " · ");
  b["tips"] = tips;
  return b;
}

void Automations::morning(const dash::Dashboard &snap, const dash::Activity *act, const Prefs &p,
                          int64_t now, bool reachable) {
  // Keep the app's briefing fresh all day.
  if (now - briefing_at_ >= 5 * kMin || now < briefing_at_) {
    bus_.set_briefing(owner_, briefing(snap, act, p, now));
    briefing_at_ = now;
  }

  // Track the Echo alarm: the sensor jumps to the next alarm once it rang.
  std::optional<int64_t> next;
  if (snap.has_glance() && snap.glance().has_next_alarm()) next = int64_t(snap.glance().next_alarm());
  if (tracked_alarm_ && now >= *tracked_alarm_) {
    state_["alarmRang"] = *tracked_alarm_;
    tracked_alarm_.reset();
    save_state();
  }
  if (next && *next > now) tracked_alarm_ = next;
  else if (!next && tracked_alarm_ && *tracked_alarm_ > now + kMin) tracked_alarm_.reset(); // deleted

  const std::string today = date_of(now);
  const int mod = minute_of_day(now);
  const bool window = mod >= 4 * 60 && mod < 12 * 60;
  const int64_t window_start = local_at(now, 4 * 60);

  // Sleep focus of the iPhone (on at night, off when you get up).
  bool focus_off_now = false, focus_waiting = false;
  if (!cfg_.focus_entity.empty() && reachable && real_now_ms() - focus_checked_ >= 5000) {
    focus_checked_ = real_now_ms();
    std::string err;
    auto s = ha_.entity_state(cfg_.focus_entity, err);
    int64_t on_since = state_.value("focusOnSince", int64_t(0));
    if (s && *s == "on" && !on_since) {
      state_["focusOnSince"] = now;
      save_state();
    } else if (s && *s == "off" && on_since) {
      state_["focusOnSince"] = 0;
      save_state();
      // "after being on at night": started before 04:00 or lasted >= 2 h
      if (window && (on_since < window_start || now - on_since >= 2 * kHour)) {
        focus_off_now = true;
        std::cout << "[Auto] focus ended (on since " << hm_at(on_since) << ")\n";
      }
    }
  }
  if (int64_t on_since = state_.value("focusOnSince", int64_t(0));
      on_since && (on_since < window_start || now - on_since >= kHour))
    focus_waiting = true; // still sleeping (focus on) -> wait for it

  if (!p.morning_briefing || !window || state_.value("briefingDay", "") == today) return;

  if (state_.value("briefingPending", "") != today) {
    std::string why;
    bool here = snap.has_presence() && snap.presence().is_now();
    int64_t rang = state_.value("alarmRang", int64_t(0));
    bool rang_today = rang >= window_start && date_of(rang) == today;
    bool alarm_ahead = tracked_alarm_ && date_of(*tracked_alarm_) == today &&
                       minute_of_day(*tracked_alarm_) < 12 * 60;
    if (focus_off_now) why = "focus off";
    else if (focus_waiting) return;
    else if (rang_today) {
      if (here && now >= rang + 5 * kMin) why = "presence after alarm " + hm_at(rang);
    } else if (!alarm_ahead && here && mod >= 5 * 60 + 30)
      why = "presence after 05:30 (no alarm)";
    if (why.empty()) return;
    std::cout << "[Auto] morning briefing due: " << why << "\n";
    state_["briefingPending"] = today;
    save_state();
  }
  if (!reachable) return;

  json b = briefing(snap, act, p, now);
  bus_.set_briefing(owner_, b);
  briefing_at_ = now;
  if (notify(p, b.value("greeting", "Guten Morgen"), b.value("summary", ""),
             {{"url", "homecollect://briefing"}, {"tag", "dash-briefing"}, {"group", "dashboard"}}) ||
      cfg_.notify_target.empty()) {
    state_["briefingDay"] = today;
    save_state();
  }
}

// ---------------------------------------------------------------------------
// 3. iPhone arrives / leaves home
// ---------------------------------------------------------------------------
void Automations::presence(const dash::Dashboard &snap, const Prefs &p, int64_t now) {
  auto events = bus_.take_presence();
  std::sort(events.begin(), events.end(), [](auto &a, auto &b) { return a.t < b.t; });
  auto &st = state_["presence"];
  if (!st.is_object()) st = json::object();

  // Leave actions run once the leave stood for 2 min without a flip back.
  auto run_leave = [&](int64_t at) {
    if (!st.contains("pendingLeave") || at - st["pendingLeave"].get<int64_t>() < 2 * kMin) return;
    int64_t lt = st["pendingLeave"].get<int64_t>();
    st.erase("pendingLeave");
    st["t"] = lt;
    st["arrive"] = false;
    save_state();
    if (now - lt > 10 * kMin) return; // e.g. restarted much later
    std::string err;
    if (p.leave_lights_off) {
      auto on = lamps_on(snap, false);
      bool someone = snap.has_presence() && snap.presence().is_now() &&
                     is_room_sensor(cfg_.presence_entity);
      if (on.empty()) std::cout << "[Auto] left home, lights already off\n";
      else if (someone) std::cout << "[Auto] left home, but someone is in the room - lights stay on\n";
      else {
        std::cout << "[Auto] left home - lights off\n";
        if (!ha_.set_lights(false, std::nullopt, 2, err)) std::cerr << "[Auto] " << err << "\n";
      }
    }
    if (p.leave_pause_media && snap.has_glance()) {
      std::vector<std::string> playing;
      for (const auto &m : snap.glance().media()) {
        std::string src = lower(m.source());
        // music on the phone itself goes with you
        if (m.state() == "playing" && src.find("iphone") == std::string::npos &&
            src.find("phone") == std::string::npos)
          playing.push_back(m.entity_id());
      }
      if (!playing.empty()) {
        std::cout << "[Auto] left home - pausing " << join(playing) << "\n";
        if (!ha_.pause_media(playing, err)) std::cerr << "[Auto] " << err << "\n";
      }
    }
  };

  for (const auto &e : events) {
    if (e.user != owner_) continue;
    run_leave(e.t);
    const char *what = e.arrive ? "arrive" : "leave";
    if (now - e.t > 10 * kMin || e.t > now + 5 * kMin) {
      std::cout << "[Auto] presence " << what << " ignored (too old)\n";
      continue;
    }
    int64_t last_t = st.value("t", int64_t(0));
    bool last_arrive = st.value("arrive", false);
    if (e.t <= last_t) continue; // duplicate / out of order
    // Debounce: a flip back within 2 min cancels the pending leave / is ignored.
    if (e.arrive && st.contains("pendingLeave")) {
      int64_t lt = st["pendingLeave"].get<int64_t>();
      if (e.t - lt < 2 * kMin) {
        std::cout << "[Auto] presence: back within 2 min, leave cancelled\n";
        st.erase("pendingLeave");
        save_state();
        continue;
      }
    }
    if (last_t && last_arrive != e.arrive && e.t - last_t < 2 * kMin) {
      std::cout << "[Auto] presence " << what << " ignored (flip back within 2 min)\n";
      continue;
    }
    if (last_t && last_arrive == e.arrive) {
      std::cout << "[Auto] presence " << what << " ignored (already " << what << ")\n";
      continue;
    }
    if (!e.arrive) {
      st["pendingLeave"] = e.t; // executed after 2 min without a flip back
      save_state();
      continue;
    }
    st.erase("pendingLeave");
    st["t"] = e.t;
    st["arrive"] = true;
    save_state();
    if (!p.arrive_lights) continue;
    auto sun = ha_.sun_below_horizon();
    if (!sun || !*sun) {
      std::cout << "[Auto] arrived home, sun is up (or unknown) - lights stay as they are\n";
      continue;
    }
    if (!lamps_on(snap, false).empty()) {
      std::cout << "[Auto] arrived home, lights already on\n";
      continue;
    }
    std::string err;
    std::cout << "[Auto] arrived home after sunset - lights on (80 %)\n";
    if (!ha_.set_lights(true, 80, 2, err)) std::cerr << "[Auto] " << err << "\n";
  }

  run_leave(now);
}

// ---------------------------------------------------------------------------
// 4. Learned sleep pattern: wake light + bedtime reminder
// ---------------------------------------------------------------------------
void Automations::sleep_pattern(const std::string &user, int64_t now) {
  if (user == pattern_user_ && std::llabs(now - pattern_at_) < kHour) return;
  pattern_user_ = user;
  pattern_at_ = now;
  std::string err;
  auto p = compute_sleep_pattern((fs::path(cfg_.data_dir) / "collect.db").string(), user, now, err);
  if (!p) {
    pattern_.reset();
    return;
  }
  pattern_ = p;
  bus_.set_sleep_pattern(user, p->to_json());
}

bool Automations::someone_home(const dash::Dashboard &snap, const dash::Activity *act,
                               int64_t now) {
  if (snap.has_presence()) {
    if (snap.presence().is_now()) return true;
    if (snap.presence().has_last_seen() && now - int64_t(snap.presence().last_seen()) < 12 * kHour)
      return true;
  }
  if (act && act->has_location()) {
    const auto &g = act->location();
    if (g.has_home()) return g.home();
    if (auto z = ha_.home_zone())
      return distance_m(g.lat(), g.lon(), (*z)[0], (*z)[1]) <= (*z)[2] + g.accuracy();
  }
  return false;
}

namespace {
const SleepPattern::Kind &kind_for(const SleepPattern &p, int64_t morning_ms) {
  int wd = local(morning_ms).tm_wday;
  return (wd >= 1 && wd <= 5) ? p.weekday : p.weekend;
}

// Next typical bedtime: the night that ends on the coming morning.
std::optional<int64_t> next_bedtime(const SleepPattern &p, int64_t now) {
  int64_t morning = minute_of_day(now) >= 12 * 60 ? local_at(now, 6 * 60, 1) : local_at(now, 6 * 60);
  const auto &k = kind_for(p, morning);
  if (!k.bedtime_min) return std::nullopt;
  return *k.bedtime_min >= 12 * 60 ? local_at(morning, *k.bedtime_min, -1)
                                   : local_at(morning, *k.bedtime_min);
}
} // namespace

void Automations::wake_light(const dash::Dashboard &snap, const dash::Activity *act,
                             const Prefs &p, int64_t now) {
  if (!p.adaptive_wake_light || !pattern_) return;
  const auto &k = kind_for(*pattern_, now);
  if (!k.wake_min) return;
  int64_t target = local_at(now, *k.wake_min), start = target - 15 * kMin;
  std::string today = date_of(now);
  if (now < start || now >= target || state_.value("wakeLightDay", "") == today) return;
  state_["wakeLightDay"] = today;
  save_state();
  if (snap.has_glance() && snap.glance().has_next_alarm()) {
    int64_t a = int64_t(snap.glance().next_alarm());
    if (a > now && a - now <= 3 * kHour) {
      std::cout << "[Auto] wake light skipped: Echo alarm at " << hm_at(a) << "\n";
      return;
    }
  }
  if (!someone_home(snap, act, now)) {
    std::cout << "[Auto] wake light skipped: nobody home\n";
    return;
  }
  if (!lamps_on(snap, false).empty()) {
    std::cout << "[Auto] wake light skipped: lights already on\n";
    return;
  }
  int fade = int((target - now) / 1000);
  std::cout << "[Auto] wake light: 10 % -> 100 % over " << fade / 60 << " min (usual wake "
            << hm(*k.wake_min) << ")\n";
  std::string err;
  if (!ha_.set_lights(true, 10, 0, err) || !ha_.set_lights(true, 100, fade, err))
    std::cerr << "[Auto] " << err << "\n";
}

void Automations::bedtime(const dash::Dashboard &snap, const Prefs &p, int64_t now) {
  if (!p.bedtime_reminder || !pattern_) return;
  auto bed = next_bedtime(*pattern_, now);
  if (!bed || now < *bed - 30 * kMin || now >= *bed - 20 * kMin) return;
  if (state_.value("bedtimeTarget", int64_t(0)) == *bed) return;
  bool de = p.language != "en";
  std::string msg = de ? "In 30 Minuten ist deine übliche Schlafenszeit (" + hm_at(*bed) + ")."
                       : "Your usual bedtime is in 30 minutes (" + hm_at(*bed) + ").";
  if (!notify(p, de ? "Schlafenszeit" : "Bedtime", msg, {{"tag", "dash-bedtime"}, {"group", "dashboard"}}) &&
      !cfg_.notify_target.empty())
    return; // retry on the next tick
  state_["bedtimeTarget"] = *bed;
  save_state();
  std::vector<std::string> dim;
  for (const auto &l : snap.lamps())
    if (l.available() && l.is_on() && l.supports_brightness()) dim.push_back(l.entity_id());
  if (dim.empty()) return;
  std::cout << "[Auto] bedtime in 30 min - dimming " << join(dim) << " to 30 %\n";
  std::string err;
  if (!ha_.set_lights(dim, true, 30, 60, err)) std::cerr << "[Auto] " << err << "\n";
}

// ---------------------------------------------------------------------------
// Dry run (CLI)
// ---------------------------------------------------------------------------
std::string Automations::dry_run(const dash::Dashboard &snap, const std::string &user_in) {
  std::ostringstream o;
  const int64_t now = this->now();
  owner();
  std::string user = user_in.empty() ? owner_ : user_in;
  if (user.empty()) return "No user (set OWNER_USER or create an admin).\n";
  owner_ = user; // greeting name for the preview
  owner_name_ = auth::resolve_owner(fs::path(cfg_.data_dir) / "users.json", user).spoken;
  Prefs p = prefs_.get(user);
  std::string err;
  std::string db = (fs::path(cfg_.data_dir) / "collect.db").string();
  auto pat = compute_sleep_pattern(db, user, now, err);
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  o << "Now: " << date_of(now) << " " << hm_at(now) << " (" << days[local(now).tm_wday] << ")\n"
    << "User: " << user << "\n\n";
  if (!pat) {
    o << "Sleep pattern: not available (" << err << ")\n";
  } else {
    auto line = [&](const char *name, const SleepPattern::Kind &k) {
      o << "  " << name << ": ";
      if (k.bedtime_min) o << "bedtime " << hm(*k.bedtime_min) << ", wake " << hm(*k.wake_min);
      else o << "not enough data";
      o << " (" << k.nights << " nights >= 3 h, need 3)\n";
    };
    o << "Sleep pattern (last 28 nights, median):\n";
    line("weekday (Mon-Fri mornings)", pat->weekday);
    line("weekend (Sat/Sun mornings)", pat->weekend);
    o << "  JSON: " << pat->to_json().dump() << "\n";
  }

  o << "\nPrefs: adaptive_wake_light=" << (p.adaptive_wake_light ? "on" : "off")
    << ", bedtime_reminder=" << (p.bedtime_reminder ? "on" : "off")
    << ", morning_briefing=" << (p.morning_briefing ? "on" : "off")
    << ", language=" << p.language << "\n";
  o << "Push target: " << (cfg_.notify_target.empty() ? "(none - HA_NOTIFY_TARGET empty)" : "notify." + cfg_.notify_target)
    << "\n\nToday (dry run, nothing is done):\n";

  // Wake light
  o << "  Wake light: ";
  const SleepPattern::Kind *k = pat ? &kind_for(*pat, now) : nullptr;
  if (!pat || !k->wake_min) o << "no predicted wake time\n";
  else {
    int64_t target = local_at(now, *k->wake_min);
    o << "usual wake " << hm(*k->wake_min) << " -> fade 10 % -> 100 % from "
      << hm_at(target - 15 * kMin) << " to " << hm(*k->wake_min);
    std::vector<std::string> why;
    if (!p.adaptive_wake_light) why.push_back("pref adaptive_wake_light is off");
    if (now >= target) why.push_back("already past for today");
    if (snap.has_glance() && snap.glance().has_next_alarm()) {
      int64_t a = int64_t(snap.glance().next_alarm()), from = std::max(now, target - 15 * kMin);
      if (a > from && a - from <= 3 * kHour) why.push_back("Echo alarm at " + hm_at(a) + " (alarm wins)");
    }
    if (!someone_home(snap, nullptr, now)) why.push_back("nobody home right now");
    o << (why.empty() ? "  => WOULD RUN" : "  => would NOT run: " + join(why, "; ")) << "\n";
  }

  // Bedtime
  o << "  Bedtime reminder: ";
  auto bed = pat ? next_bedtime(*pat, now) : std::nullopt;
  if (!bed) o << "no typical bedtime\n";
  else {
    bool de = p.language != "en";
    o << "typical bedtime " << hm_at(*bed) << " -> push at " << hm_at(*bed - 30 * kMin) << ": \""
      << (de ? "In 30 Minuten ist deine übliche Schlafenszeit (" : "Your usual bedtime is in 30 minutes (")
      << hm_at(*bed) << ").\" + dim lights that are on to 30 %"
      << (p.bedtime_reminder ? "  => WOULD RUN" : "  => would NOT run: pref bedtime_reminder is off")
      << "\n";
  }

  // Briefing
  json b = briefing(snap, nullptr, p, now);
  o << "  Morning briefing push (04:00-12:00, once): trigger = "
    << (cfg_.focus_entity.empty() ? "room presence after the alarm / after 05:30"
                                  : "focus " + cfg_.focus_entity + " on -> off (fallback: presence)")
    << (p.morning_briefing ? "" : "  [pref off]") << "\n"
    << "    title:   " << b.value("greeting", "") << "\n"
    << "    message: " << b.value("summary", "") << "\n"
    << "    JSON:    " << b.dump() << "\n";
  return o.str();
}
