#include "collector.h"

#include "auth.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <chrono>
#include <ctime>
#include <map>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string_view>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr const char *kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS devices (
  id         TEXT PRIMARY KEY,
  user       TEXT NOT NULL,
  name       TEXT NOT NULL,
  token_hash TEXT NOT NULL UNIQUE,
  created    INTEGER NOT NULL,
  last_seen  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS health (
  user     TEXT NOT NULL,
  uuid     TEXT NOT NULL,
  type     TEXT NOT NULL,
  start_ms INTEGER NOT NULL,
  end_ms   INTEGER NOT NULL,
  value    REAL,
  unit     TEXT,
  source   TEXT,
  meta     TEXT,
  PRIMARY KEY (user, uuid)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS health_type_time ON health(user, type, start_ms);
CREATE TABLE IF NOT EXISTS locations (
  user      TEXT NOT NULL,
  t         INTEGER NOT NULL,
  kind      TEXT NOT NULL,
  lat       REAL NOT NULL,
  lon       REAL NOT NULL,
  accuracy  REAL,
  altitude  REAL,
  speed     REAL,
  course    REAL,
  arrive_ms INTEGER,
  depart_ms INTEGER,
  PRIMARY KEY (user, t, kind)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS device_stats (
  user          TEXT NOT NULL,
  t             INTEGER NOT NULL,
  device        TEXT,
  battery       REAL,
  battery_state TEXT,
  low_power     INTEGER,
  storage_free  INTEGER,
  storage_total INTEGER,
  network       TEXT,
  PRIMARY KEY (user, t)
) WITHOUT ROWID;
)SQL";

// RAII prepared statement.
class Stmt {
public:
  Stmt(sqlite3 *db, const char *sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr;
  }
  ~Stmt() { sqlite3_finalize(s_); }
  explicit operator bool() const { return s_ != nullptr; }
  sqlite3_stmt *get() { return s_; }

  void reset() {
    sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
  }
  void bind(int i, const std::string &v) {
    sqlite3_bind_text(s_, i, v.data(), int(v.size()), SQLITE_TRANSIENT);
  }
  void bind(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); }
  void bind(int i, double v) { sqlite3_bind_double(s_, i, v); }
  void bind_null(int i) { sqlite3_bind_null(s_, i); }
  int step() { return sqlite3_step(s_); }
  std::string text(int col) {
    auto *p = sqlite3_column_text(s_, col);
    return p ? std::string(reinterpret_cast<const char *>(p)) : std::string();
  }
  int64_t int64(int col) { return sqlite3_column_int64(s_, col); }

private:
  sqlite3_stmt *s_ = nullptr;
};

// --- validation helpers ----------------------------------------------------
// Each returns false on a bad value; optional fields may be absent/null.

bool get_string(const json &o, const char *key, std::string &out, size_t max_len,
                bool required = true) {
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return !required;
  if (!it->is_string()) return false;
  out = it->get<std::string>();
  if (out.empty() && required) return false;
  if (out.size() > max_len) return false;
  for (unsigned char c : out)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

bool get_time(const json &o, const char *key, int64_t &out, bool required = true) {
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return !required;
  if (!it->is_number()) return false;
  double d = it->get<double>();
  // 1970 .. now + 1 day (clock skew), in Unix ms
  if (!std::isfinite(d) || d < 0 || d > double(now_ms() + 86400000LL)) return false;
  out = int64_t(d);
  return true;
}

bool get_number(const json &o, const char *key, std::optional<double> &out, double lo,
                double hi) {
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return true;
  if (!it->is_number()) return false;
  double d = it->get<double>();
  if (!std::isfinite(d) || d < lo || d > hi) return false;
  out = d;
  return true;
}

bool id_chars(const std::string &s, const char *extra) {
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && !std::strchr(extra, c)) return false;
  return true;
}

void bind_opt(Stmt &s, int i, const std::optional<double> &v) {
  if (v) s.bind(i, *v);
  else s.bind_null(i);
}

const json &array_field(const json &body, const char *key) {
  static const json empty = json::array();
  auto it = body.find(key);
  return (it != body.end() && it->is_array()) ? *it : empty;
}

} // namespace

Collector::Collector(fs::path db_file) : file_(std::move(db_file)) {}

Collector::~Collector() {
  if (db_) sqlite3_close(db_);
}

bool Collector::exec(const char *sql, std::string &error) {
  char *msg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    error = msg ? msg : "sqlite error";
    sqlite3_free(msg);
    return false;
  }
  return true;
}

bool Collector::open(std::string &error) {
  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  mode_t old = umask(0077); // db + WAL files are created 0600
  int rc = sqlite3_open_v2(file_.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                           nullptr);
  umask(old);
  if (rc != SQLITE_OK) {
    error = "cannot open " + file_.string() + ": " + (db_ ? sqlite3_errmsg(db_) : "?");
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  sqlite3_busy_timeout(db_, 3000); // the CLI may hold a lock briefly
  if (!exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;",
            error) ||
      !exec(kSchema, error)) {
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------
std::string Collector::add_device(const std::string &user, const std::string &name,
                                  std::string &id_out) {
  if (!db_) return {};
  std::string token = auth::random_token();
  id_out = auth::random_token().substr(0, 8);
  Stmt s(db_, "INSERT INTO devices(id, user, name, token_hash, created) VALUES(?,?,?,?,?)");
  if (!s) return {};
  s.bind(1, id_out);
  s.bind(2, user);
  s.bind(3, name);
  s.bind(4, auth::sha256_hex(token));
  s.bind(5, now_ms());
  if (s.step() != SQLITE_DONE) return {};
  return token;
}

std::optional<Collector::Device> Collector::device_for_token(const std::string &token) {
  if (!db_ || token.size() != 64) return std::nullopt;
  Stmt s(db_, "SELECT id, user, name, created, last_seen FROM devices WHERE token_hash = ?");
  if (!s) return std::nullopt;
  s.bind(1, auth::sha256_hex(token));
  if (s.step() != SQLITE_ROW) return std::nullopt;
  Device d{s.text(0), s.text(1), s.text(2), s.int64(3), s.int64(4)};
  Stmt u(db_, "UPDATE devices SET last_seen = ? WHERE id = ?");
  if (u) {
    u.bind(1, now_ms());
    u.bind(2, d.id);
    u.step();
  }
  return d;
}

std::vector<Collector::Device> Collector::devices() {
  std::vector<Device> out;
  if (!db_) return out;
  Stmt s(db_, "SELECT id, user, name, created, last_seen FROM devices ORDER BY user, created");
  while (s && s.step() == SQLITE_ROW)
    out.push_back({s.text(0), s.text(1), s.text(2), s.int64(3), s.int64(4)});
  return out;
}

bool Collector::remove_device(const std::string &id) {
  if (!db_) return false;
  Stmt s(db_, "DELETE FROM devices WHERE id = ?");
  if (!s) return false;
  s.bind(1, id);
  return s.step() == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

int Collector::remove_user_devices(const std::string &user) {
  if (!db_) return 0;
  Stmt s(db_, "DELETE FROM devices WHERE user = ?");
  if (!s) return 0;
  s.bind(1, user);
  return s.step() == SQLITE_DONE ? sqlite3_changes(db_) : 0;
}

// ---------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------
json Collector::ingest(const std::string &user, const json &body, std::string &error) {
  if (!db_) {
    error = "storage_unavailable";
    return {};
  }
  const json &health = array_field(body, "health");
  const json &deleted = array_field(body, "deleted");
  const json &locations = array_field(body, "locations");
  const json &stats = array_field(body, "device");
  if (health.size() + deleted.size() + locations.size() + stats.size() > kMaxItems) {
    error = "too_many_items";
    return {};
  }

  Stmt ins_health(db_, "INSERT INTO health(user, uuid, type, start_ms, end_ms, value, unit, "
                       "source, meta) VALUES(?,?,?,?,?,?,?,?,?) "
                       "ON CONFLICT(user, uuid) DO NOTHING");
  Stmt del_health(db_, "DELETE FROM health WHERE user = ? AND uuid = ?");
  Stmt ins_loc(db_, "INSERT INTO locations(user, t, kind, lat, lon, accuracy, altitude, speed, "
                    "course, arrive_ms, depart_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?) "
                    // A visit is reported twice: on arrival, then with its departure.
                    "ON CONFLICT(user, t, kind) DO UPDATE SET "
                    "depart_ms = COALESCE(excluded.depart_ms, locations.depart_ms) "
                    "WHERE locations.kind = 'visit' AND locations.depart_ms IS NOT "
                    "excluded.depart_ms AND excluded.depart_ms IS NOT NULL");
  Stmt ins_stat(db_, "INSERT INTO device_stats(user, t, device, battery, battery_state, "
                     "low_power, storage_free, storage_total, network) "
                     "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(user, t) DO NOTHING");
  if (!ins_health || !del_health || !ins_loc || !ins_stat) {
    error = "storage_error";
    return {};
  }

  std::string tx_err;
  if (!exec("BEGIN IMMEDIATE", tx_err)) {
    error = "storage_busy";
    return {};
  }
  auto fail = [&](std::string why) {
    std::string ignore;
    exec("ROLLBACK", ignore);
    error = std::move(why);
    return json();
  };

  int n_health = 0, n_deleted = 0, n_loc = 0, n_stat = 0;

  for (size_t i = 0; i < health.size(); ++i) {
    const json &h = health[i];
    if (!h.is_object()) return fail("health[" + std::to_string(i) + "]: not an object");
    std::string uuid, type, unit, source;
    int64_t start = 0, end = 0;
    std::optional<double> value;
    if (!get_string(h, "uuid", uuid, 64) || !id_chars(uuid, "-") ||
        !get_string(h, "type", type, 96) || !id_chars(type, "_.") ||
        !get_time(h, "start", start) || !get_time(h, "end", end) || end < start ||
        !get_number(h, "value", value, -1e12, 1e12) ||
        !get_string(h, "unit", unit, 32, false) || !get_string(h, "source", source, 128, false))
      return fail("health[" + std::to_string(i) + "]: invalid field");
    std::string meta;
    if (auto it = h.find("meta"); it != h.end() && !it->is_null()) {
      if (!it->is_object()) return fail("health[" + std::to_string(i) + "]: meta");
      meta = it->dump();
      if (meta.size() > 4096) return fail("health[" + std::to_string(i) + "]: meta too large");
    }
    ins_health.reset();
    ins_health.bind(1, user);
    ins_health.bind(2, uuid);
    ins_health.bind(3, type);
    ins_health.bind(4, start);
    ins_health.bind(5, end);
    bind_opt(ins_health, 6, value);
    if (unit.empty()) ins_health.bind_null(7); else ins_health.bind(7, unit);
    if (source.empty()) ins_health.bind_null(8); else ins_health.bind(8, source);
    if (meta.empty()) ins_health.bind_null(9); else ins_health.bind(9, meta);
    if (ins_health.step() != SQLITE_DONE) return fail("storage_error");
    n_health += sqlite3_changes(db_);
  }

  for (size_t i = 0; i < deleted.size(); ++i) {
    const json &d = deleted[i];
    if (!d.is_string() || d.get<std::string>().size() > 64 ||
        !id_chars(d.get<std::string>(), "-"))
      return fail("deleted[" + std::to_string(i) + "]: invalid uuid");
    del_health.reset();
    del_health.bind(1, user);
    del_health.bind(2, d.get<std::string>());
    if (del_health.step() != SQLITE_DONE) return fail("storage_error");
    n_deleted += sqlite3_changes(db_);
  }

  for (size_t i = 0; i < locations.size(); ++i) {
    const json &l = locations[i];
    if (!l.is_object()) return fail("locations[" + std::to_string(i) + "]: not an object");
    int64_t t = 0, arrive = -1, depart = -1;
    std::string kind = "point";
    std::optional<double> lat, lon, acc, alt, speed, course;
    if (!get_time(l, "t", t) || !get_string(l, "kind", kind, 16, false) ||
        (kind != "point" && kind != "visit") || !get_number(l, "lat", lat, -90, 90) ||
        !get_number(l, "lon", lon, -180, 180) || !lat || !lon ||
        !get_number(l, "accuracy", acc, 0, 1e6) || !get_number(l, "altitude", alt, -1e4, 1e5) ||
        !get_number(l, "speed", speed, -1, 1e4) || !get_number(l, "course", course, -1, 360) ||
        !get_time(l, "arrive", arrive, false) || !get_time(l, "depart", depart, false))
      return fail("locations[" + std::to_string(i) + "]: invalid field");
    ins_loc.reset();
    ins_loc.bind(1, user);
    ins_loc.bind(2, t);
    ins_loc.bind(3, kind);
    ins_loc.bind(4, *lat);
    ins_loc.bind(5, *lon);
    bind_opt(ins_loc, 6, acc);
    bind_opt(ins_loc, 7, alt);
    bind_opt(ins_loc, 8, speed);
    bind_opt(ins_loc, 9, course);
    if (arrive >= 0) ins_loc.bind(10, arrive); else ins_loc.bind_null(10);
    if (depart >= 0) ins_loc.bind(11, depart); else ins_loc.bind_null(11);
    if (ins_loc.step() != SQLITE_DONE) return fail("storage_error");
    n_loc += sqlite3_changes(db_);
  }

  for (size_t i = 0; i < stats.size(); ++i) {
    const json &s = stats[i];
    if (!s.is_object()) return fail("device[" + std::to_string(i) + "]: not an object");
    int64_t t = 0;
    std::string device, state, network;
    std::optional<double> battery, free_b, total_b;
    bool low_power = false, has_low_power = false;
    if (auto it = s.find("lowPower"); it != s.end() && !it->is_null()) {
      if (!it->is_boolean()) return fail("device[" + std::to_string(i) + "]: lowPower");
      low_power = it->get<bool>();
      has_low_power = true;
    }
    if (!get_time(s, "t", t) || !get_string(s, "device", device, 64, false) ||
        !get_number(s, "battery", battery, 0, 1) ||
        !get_string(s, "batteryState", state, 16, false) ||
        !get_number(s, "storageFree", free_b, 0, 1e15) ||
        !get_number(s, "storageTotal", total_b, 0, 1e15) ||
        !get_string(s, "network", network, 16, false))
      return fail("device[" + std::to_string(i) + "]: invalid field");
    ins_stat.reset();
    ins_stat.bind(1, user);
    ins_stat.bind(2, t);
    if (device.empty()) ins_stat.bind_null(3); else ins_stat.bind(3, device);
    bind_opt(ins_stat, 4, battery);
    if (state.empty()) ins_stat.bind_null(5); else ins_stat.bind(5, state);
    if (has_low_power) ins_stat.bind(6, int64_t(low_power)); else ins_stat.bind_null(6);
    if (free_b) ins_stat.bind(7, int64_t(*free_b)); else ins_stat.bind_null(7);
    if (total_b) ins_stat.bind(8, int64_t(*total_b)); else ins_stat.bind_null(8);
    if (network.empty()) ins_stat.bind_null(9); else ins_stat.bind(9, network);
    if (ins_stat.step() != SQLITE_DONE) return fail("storage_error");
    n_stat += sqlite3_changes(db_);
  }

  if (!exec("COMMIT", tx_err)) return fail("storage_error");
  return {{"health", n_health}, {"deleted", n_deleted}, {"locations", n_loc}, {"device", n_stat}};
}

json Collector::status(const std::string &user) {
  json out = json::object();
  if (!db_) return out;
  auto summary = [&](const char *key, const char *sql) {
    Stmt s(db_, sql);
    if (!s) return;
    s.bind(1, user);
    if (s.step() == SQLITE_ROW)
      out[key] = {{"count", s.int64(0)},
                  {"newest", sqlite3_column_type(s.get(), 1) == SQLITE_NULL ? json(nullptr)
                                                                             : json(s.int64(1))}};
  };
  summary("health", "SELECT COUNT(*), MAX(end_ms) FROM health WHERE user = ?");
  summary("locations", "SELECT COUNT(*), MAX(t) FROM locations WHERE user = ?");
  summary("device", "SELECT COUNT(*), MAX(t) FROM device_stats WHERE user = ?");
  json types = json::object();
  Stmt s(db_, "SELECT type, COUNT(*) FROM health WHERE user = ? GROUP BY type ORDER BY type");
  if (s) {
    s.bind(1, user);
    while (s.step() == SQLITE_ROW) types[s.text(0)] = s.int64(1);
  }
  out["healthTypes"] = types;
  return out;
}

// ---------------------------------------------------------------------------
// Summary for the dashboard / Home Assistant
// ---------------------------------------------------------------------------
namespace {
int64_t local_today_at_ms(int hour) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  tm.tm_hour = hour;
  tm.tm_min = tm.tm_sec = 0;
  tm.tm_isdst = -1;
  return int64_t(std::mktime(&tm)) * 1000;
}
int64_t local_midnight_ms() { return local_today_at_ms(0); }
} // namespace

std::vector<std::string> Collector::users_with_data() {
  std::vector<std::string> out;
  if (!db_) return out;
  Stmt s(db_, "SELECT user FROM health UNION SELECT user FROM locations "
              "UNION SELECT user FROM device_stats");
  while (s && s.step() == SQLITE_ROW) out.push_back(s.text(0));
  return out;
}

bool Collector::summary(const std::string &user, dashboard_backend::Activity &out) {
  if (!db_) return false;
  out.Clear();
  out.set_user(user);
  bool any = false;
  const int64_t now = now_ms();

  // Today's totals. iPhone and Watch both record steps etc. for the same
  // minutes, so sum per source and take the biggest source (close to what
  // the Health app shows).
  {
    Stmt s(db_, "SELECT type, MAX(total) FROM (SELECT type, source, SUM(value) AS total "
                "FROM health WHERE user = ? AND start_ms >= ? AND type IN ("
                "'HKQuantityTypeIdentifierStepCount','HKQuantityTypeIdentifierActiveEnergyBurned',"
                "'HKQuantityTypeIdentifierDistanceWalkingRunning',"
                "'HKQuantityTypeIdentifierAppleExerciseTime','HKQuantityTypeIdentifierFlightsClimbed')"
                " GROUP BY type, source) GROUP BY type");
    if (s) {
      s.bind(1, user);
      s.bind(2, local_midnight_ms());
      while (s.step() == SQLITE_ROW) {
        std::string type = s.text(0);
        double v = sqlite3_column_double(s.get(), 1);
        any = true;
        if (type.ends_with("StepCount")) out.set_steps(int32_t(std::lround(v)));
        else if (type.ends_with("ActiveEnergyBurned")) out.set_active_kcal(float(v));
        else if (type.ends_with("DistanceWalkingRunning")) out.set_distance_km(float(v / 1000));
        else if (type.ends_with("AppleExerciseTime")) out.set_exercise_min(int32_t(std::lround(v)));
        else if (type.ends_with("FlightsClimbed")) out.set_flights(int32_t(std::lround(v)));
      }
    }
  }

  // Latest vitals (only if recent enough to mean something).
  auto latest = [&](const char *type, int64_t max_age_ms) -> std::optional<double> {
    Stmt s(db_, "SELECT value FROM health WHERE user = ? AND type = ? AND end_ms >= ? "
                "ORDER BY end_ms DESC LIMIT 1");
    if (!s) return std::nullopt;
    s.bind(1, user);
    s.bind(2, std::string(type));
    s.bind(3, now - max_age_ms);
    if (s.step() != SQLITE_ROW || sqlite3_column_type(s.get(), 0) == SQLITE_NULL)
      return std::nullopt;
    any = true;
    return sqlite3_column_double(s.get(), 0);
  };
  constexpr int64_t kHour = 3600000, kDay = 24 * kHour;
  if (auto v = latest("HKQuantityTypeIdentifierHeartRate", 6 * kHour))
    out.set_heart_rate(int32_t(std::lround(*v)));
  if (auto v = latest("HKQuantityTypeIdentifierRestingHeartRate", 3 * kDay))
    out.set_resting_hr(int32_t(std::lround(*v)));
  if (auto v = latest("HKQuantityTypeIdentifierHeartRateVariabilitySDNN", 3 * kDay))
    out.set_hrv_ms(float(*v));
  if (auto v = latest("HKQuantityTypeIdentifierVO2Max", 60 * kDay)) out.set_vo2max(float(*v));

  // Last night: sleep that ended between yesterday 18:00 and today 18:00
  // (local time), from the source with the most sleep (the Watch beats the
  // iPhone's "in bed" estimate).
  {
    Stmt s(db_, "SELECT COALESCE(source, ''), CAST(value AS INTEGER), SUM(end_ms - start_ms) "
                "FROM health WHERE user = ? AND type = 'HKCategoryTypeIdentifierSleepAnalysis' "
                "AND end_ms >= ? AND end_ms < ? GROUP BY 1, 2");
    if (s) {
      int64_t evening = local_today_at_ms(18);
      s.bind(1, user);
      s.bind(2, evening - 24 * kHour);
      s.bind(3, evening);
      std::map<std::string, std::map<int, int64_t>> by_source;
      while (s.step() == SQLITE_ROW) by_source[s.text(0)][int(s.int64(1))] += s.int64(2);
      const std::map<int, int64_t> *best = nullptr;
      int64_t best_asleep = 0;
      for (const auto &[src, stages] : by_source) {
        int64_t asleep = 0;
        for (const auto &[stage, dur] : stages)
          if (stage == 1 || stage >= 3) asleep += dur; // unspecified, core, deep, REM
        if (asleep > best_asleep) {
          best_asleep = asleep;
          best = &stages;
        }
      }
      if (best) {
        any = true;
        auto min = [&](int stage) {
          auto it = best->find(stage);
          return it == best->end() ? 0 : int32_t(it->second / 60000);
        };
        auto *sl = out.mutable_sleep();
        sl->set_total(int32_t(best_asleep / 60000));
        sl->set_core(min(3));
        sl->set_deep(min(4));
        sl->set_rem(min(5));
        sl->set_awake(min(2));
      }
    }
  }

  // Last workout (within a week).
  {
    Stmt s(db_, "SELECT end_ms, value, meta FROM health WHERE user = ? AND "
                "type = 'HKWorkoutTypeIdentifier' AND end_ms >= ? ORDER BY end_ms DESC LIMIT 1");
    if (s) {
      s.bind(1, user);
      s.bind(2, now - 7 * kDay);
      if (s.step() == SQLITE_ROW) {
        any = true;
        auto *w = out.mutable_last_workout();
        w->set_end(uint64_t(s.int64(0)));
        w->set_duration_s(uint32_t(sqlite3_column_double(s.get(), 1)));
        json meta = json::parse(s.text(2), nullptr, false);
        if (meta.is_object()) {
          if (meta.contains("activity") && meta["activity"].is_string())
            w->set_activity(meta["activity"] == "type3000" ? std::string("other") // older app builds
                                                             : meta["activity"].get<std::string>());
          if (meta.contains("energyKcal") && meta["energyKcal"].is_number())
            w->set_kcal(meta["energyKcal"].get<float>());
          if (meta.contains("distanceM") && meta["distanceM"].is_number())
            w->set_distance_km(meta["distanceM"].get<float>() / 1000);
        }
      }
    }
  }

  // Position: newest point or visit.
  {
    Stmt s(db_, "SELECT t, lat, lon, accuracy FROM locations WHERE user = ? "
                "ORDER BY MAX(t, COALESCE(depart_ms, 0)) DESC LIMIT 1");
    if (s) {
      s.bind(1, user);
      if (s.step() == SQLITE_ROW) {
        any = true;
        auto *g = out.mutable_location();
        g->set_time(uint64_t(s.int64(0)));
        g->set_lat(sqlite3_column_double(s.get(), 1));
        g->set_lon(sqlite3_column_double(s.get(), 2));
        if (sqlite3_column_type(s.get(), 3) != SQLITE_NULL)
          g->set_accuracy(float(sqlite3_column_double(s.get(), 3)));
      }
    }
  }

  // Battery.
  {
    Stmt s(db_, "SELECT battery, COALESCE(battery_state, '') FROM device_stats WHERE user = ? "
                "AND battery IS NOT NULL ORDER BY t DESC LIMIT 1");
    if (s) {
      s.bind(1, user);
      if (s.step() == SQLITE_ROW) {
        any = true;
        out.set_battery(float(sqlite3_column_double(s.get(), 0)));
        out.set_battery_state(s.text(1));
      }
    }
  }

  out.set_synced_at(uint64_t(now));
  return any;
}

// ---------------------------------------------------------------------------
// History (GET /api/history/*). "Day" = local calendar day of the server.
// SQLite's 'localtime' modifier uses the same C library time zone as
// mktime/localtime_r, so SQL day buckets match the dates built here.
// ---------------------------------------------------------------------------
namespace {
constexpr const char *kSleepType = "HKCategoryTypeIdentifierSleepAnalysis";
constexpr const char *kWorkoutType = "HKWorkoutTypeIdentifier";
constexpr int64_t kHourMs = 3600000;

// Local time of y-m-d + add_days at `hour`:00 (mktime normalizes overflow).
int64_t local_ms(int y, int m, int d, int add_days, int hour) {
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d + add_days;
  tm.tm_hour = hour;
  tm.tm_isdst = -1;
  return int64_t(std::mktime(&tm)) * 1000;
}

std::string date_str(int y, int m, int d, int add_days) {
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d + add_days;
  tm.tm_hour = 12;
  tm.tm_isdst = -1;
  std::mktime(&tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

void today(int &y, int &m, int &d) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  y = tm.tm_year + 1900;
  m = tm.tm_mon + 1;
  d = tm.tm_mday;
}

double round_to(double v, int decimals) {
  double f = std::pow(10.0, decimals);
  return std::round(v * f) / f;
}

bool is_asleep(int stage) { return stage == 1 || stage >= 3; } // unspecified, core, deep, REM

// Sleep source with the most asleep time (Watch beats the iPhone's estimate).
struct BestSleep {
  std::string source;
  const std::map<int, int64_t> *stages = nullptr;
  int64_t asleep = 0;
};
BestSleep best_sleep(const std::map<std::string, std::map<int, int64_t>> &by_source) {
  BestSleep b;
  for (const auto &[src, stages] : by_source) {
    int64_t asleep = 0;
    for (const auto &[stage, dur] : stages)
      if (is_asleep(stage)) asleep += dur;
    if (asleep > b.asleep) b = {src, &stages, asleep};
  }
  return b;
}

const char *stage_name(int v) {
  switch (v) {
  case 0: return "inBed";
  case 2: return "awake";
  case 3: return "core";
  case 4: return "deep";
  case 5: return "rem";
  default: return "asleep";
  }
}

std::optional<double> col_double(Stmt &s, int col) {
  if (sqlite3_column_type(s.get(), col) == SQLITE_NULL) return std::nullopt;
  return sqlite3_column_double(s.get(), col);
}
} // namespace

bool Collector::valid_date(std::string_view s, int &y, int &m, int &d) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  y = std::stoi(std::string(s.substr(0, 4)));
  m = std::stoi(std::string(s.substr(5, 2)));
  d = std::stoi(std::string(s.substr(8, 2)));
  if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1) return false;
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return d <= kDays[m - 1] + (m == 2 && leap ? 1 : 0);
}

json Collector::history_daily(const std::string &user, int days) {
  json out = {{"days", json::array()}};
  if (!db_ || days < 1) return out;
  int y, m, d;
  today(y, m, d);
  const int first = -(days - 1);
  const int64_t from = local_ms(y, m, d, first, 0), to = local_ms(y, m, d, 1, 0);
  std::map<std::string, json> by_day;

  // Totals: sum per source and day, then the biggest source (iPhone + Watch).
  {
    Stmt s(db_, "SELECT date(start_ms / 1000, 'unixepoch', 'localtime') AS d, type, "
                "SUM(value) FROM health WHERE user = ? AND type IN ("
                "'HKQuantityTypeIdentifierStepCount','HKQuantityTypeIdentifierActiveEnergyBurned',"
                "'HKQuantityTypeIdentifierDistanceWalkingRunning',"
                "'HKQuantityTypeIdentifierAppleExerciseTime','HKQuantityTypeIdentifierFlightsClimbed')"
                " AND start_ms >= ? AND start_ms < ? GROUP BY d, type, COALESCE(source, '')");
    if (s) {
      s.bind(1, user);
      s.bind(2, from);
      s.bind(3, to);
      std::map<std::pair<std::string, std::string>, double> best;
      while (s.step() == SQLITE_ROW) {
        auto key = std::make_pair(s.text(0), s.text(1));
        double v = sqlite3_column_double(s.get(), 2);
        auto it = best.find(key);
        if (it == best.end() || v > it->second) best[key] = v;
      }
      for (const auto &[key, v] : best) {
        json &o = by_day[key.first];
        const std::string &type = key.second;
        if (type.ends_with("StepCount")) o["steps"] = std::lround(v);
        else if (type.ends_with("ActiveEnergyBurned")) o["activeKcal"] = round_to(v, 1);
        else if (type.ends_with("DistanceWalkingRunning")) o["distanceKm"] = round_to(v / 1000, 2);
        else if (type.ends_with("AppleExerciseTime")) o["exerciseMin"] = std::lround(v);
        else if (type.ends_with("FlightsClimbed")) o["flights"] = std::lround(v);
      }
    }
  }

  // Vitals: min / max / average per day.
  {
    Stmt s(db_, "SELECT date(start_ms / 1000, 'unixepoch', 'localtime') AS d, type, "
                "MIN(value), MAX(value), AVG(value) FROM health WHERE user = ? AND type IN ("
                "'HKQuantityTypeIdentifierHeartRate','HKQuantityTypeIdentifierRestingHeartRate',"
                "'HKQuantityTypeIdentifierHeartRateVariabilitySDNN') AND value IS NOT NULL "
                "AND start_ms >= ? AND start_ms < ? GROUP BY d, type");
    if (s) {
      s.bind(1, user);
      s.bind(2, from);
      s.bind(3, to);
      while (s.step() == SQLITE_ROW) {
        json &o = by_day[s.text(0)];
        std::string type = s.text(1);
        double lo = sqlite3_column_double(s.get(), 2), hi = sqlite3_column_double(s.get(), 3),
               avg = sqlite3_column_double(s.get(), 4);
        if (type.ends_with("RestingHeartRate")) o["restingHr"] = std::lround(avg);
        else if (type.ends_with("VariabilitySDNN")) o["hrvMs"] = round_to(avg, 1);
        else {
          o["heartRateMin"] = std::lround(lo);
          o["heartRateMax"] = std::lround(hi);
          o["heartRateAvg"] = std::lround(avg);
        }
      }
    }
  }

  // Sleep belongs to the day it ended: night of day D = ends between D-1
  // 18:00 and D 18:00, i.e. the local date of (end + 6 h).
  {
    const int64_t s_from = local_ms(y, m, d, first - 1, 18), s_to = local_ms(y, m, d, 0, 18);
    Stmt s(db_, "SELECT date(end_ms / 1000, 'unixepoch', 'localtime', '+6 hours') AS d, "
                "COALESCE(source, ''), CAST(value AS INTEGER), SUM(end_ms - start_ms) "
                "FROM health WHERE user = ? AND type = ? AND start_ms >= ? AND start_ms < ? "
                "AND end_ms >= ? AND end_ms < ? GROUP BY 1, 2, 3");
    if (s) {
      s.bind(1, user);
      s.bind(2, std::string(kSleepType));
      s.bind(3, s_from - 2 * 24 * kHourMs); // lets the (user, type, start_ms) index help
      s.bind(4, s_to);
      s.bind(5, s_from);
      s.bind(6, s_to);
      std::map<std::string, std::map<std::string, std::map<int, int64_t>>> nights;
      while (s.step() == SQLITE_ROW)
        nights[s.text(0)][s.text(1)][int(s.int64(2))] += s.int64(3);
      for (const auto &[day, by_source] : nights) {
        BestSleep b = best_sleep(by_source);
        if (!b.stages) continue;
        json &o = by_day[day];
        o["sleepMin"] = b.asleep / 60000;
        static constexpr std::pair<int, const char *> kStages[] = {
            {4, "deepMin"}, {5, "remMin"}, {3, "coreMin"}, {2, "awakeMin"}};
        for (auto [stage, key] : kStages)
          if (auto it = b.stages->find(stage); it != b.stages->end()) o[key] = it->second / 60000;
      }
    }
  }

  // Workouts per start day.
  {
    Stmt s(db_, "SELECT date(start_ms / 1000, 'unixepoch', 'localtime') AS d, COUNT(*) "
                "FROM health WHERE user = ? AND type = ? AND start_ms >= ? AND start_ms < ? "
                "GROUP BY d");
    if (s) {
      s.bind(1, user);
      s.bind(2, std::string(kWorkoutType));
      s.bind(3, from);
      s.bind(4, to);
      while (s.step() == SQLITE_ROW) by_day[s.text(0)]["workouts"] = s.int64(1);
    }
  }

  for (int i = first; i <= 0; ++i) {
    std::string date = date_str(y, m, d, i);
    json o = json::object();
    if (auto it = by_day.find(date); it != by_day.end()) o = std::move(it->second);
    o["date"] = date;
    out["days"].push_back(std::move(o));
  }
  return out;
}

json Collector::history_heartrate(const std::string &user, int y, int m, int d) {
  json samples = json::array();
  const int64_t from = local_ms(y, m, d, 0, 0), to = local_ms(y, m, d, 1, 0);
  constexpr int64_t kBucket = 5 * 60000;
  Stmt s(db_, "SELECT (start_ms - ?1) / ?4 AS b, AVG(value) FROM health WHERE user = ?2 "
              "AND type = 'HKQuantityTypeIdentifierHeartRate' AND value IS NOT NULL "
              "AND start_ms >= ?1 AND start_ms < ?3 GROUP BY b ORDER BY b");
  if (db_ && s) {
    s.bind(1, from);
    s.bind(2, user);
    s.bind(3, to);
    s.bind(4, kBucket);
    while (s.step() == SQLITE_ROW)
      samples.push_back({{"t", from + s.int64(0) * kBucket},
                         {"bpm", std::lround(sqlite3_column_double(s.get(), 1))}});
  }
  return {{"samples", samples}};
}

json Collector::history_sleep(const std::string &user, int y, int m, int d) {
  json out = {{"segments", json::array()}};
  if (!db_) return out;
  const int64_t from = local_ms(y, m, d, -1, 18), to = local_ms(y, m, d, 0, 18);
  std::map<std::string, std::map<int, int64_t>> by_source;
  {
    Stmt s(db_, "SELECT COALESCE(source, ''), CAST(value AS INTEGER), SUM(end_ms - start_ms) "
                "FROM health WHERE user = ? AND type = ? AND start_ms >= ? AND start_ms < ? "
                "AND end_ms >= ? AND end_ms < ? GROUP BY 1, 2");
    if (!s) return out;
    s.bind(1, user);
    s.bind(2, std::string(kSleepType));
    s.bind(3, from - 2 * 24 * kHourMs);
    s.bind(4, to);
    s.bind(5, from);
    s.bind(6, to);
    while (s.step() == SQLITE_ROW) by_source[s.text(0)][int(s.int64(1))] += s.int64(2);
  }
  BestSleep b = best_sleep(by_source);
  if (!b.stages) return out;
  Stmt s(db_, "SELECT start_ms, end_ms, CAST(value AS INTEGER) FROM health WHERE user = ? "
              "AND type = ? AND start_ms >= ? AND start_ms < ? AND end_ms >= ? AND end_ms < ? "
              "AND COALESCE(source, '') = ? ORDER BY start_ms, end_ms");
  if (!s) return out;
  s.bind(1, user);
  s.bind(2, std::string(kSleepType));
  s.bind(3, from - 2 * 24 * kHourMs);
  s.bind(4, to);
  s.bind(5, from);
  s.bind(6, to);
  s.bind(7, b.source);
  while (s.step() == SQLITE_ROW)
    out["segments"].push_back(
        {{"start", s.int64(0)}, {"end", s.int64(1)}, {"stage", stage_name(int(s.int64(2)))}});
  out["source"] = b.source;
  return out;
}

json Collector::history_workouts(const std::string &user, int days) {
  json list = json::array();
  int y, m, d;
  today(y, m, d);
  Stmt s(db_, "SELECT uuid, start_ms, end_ms, value, meta FROM health WHERE user = ? "
              "AND type = ? AND start_ms >= ? ORDER BY start_ms DESC");
  if (db_ && s) {
    s.bind(1, user);
    s.bind(2, std::string(kWorkoutType));
    s.bind(3, local_ms(y, m, d, -(days - 1), 0));
    while (s.step() == SQLITE_ROW) {
      json w = {{"uuid", s.text(0)}, {"start", s.int64(1)}, {"end", s.int64(2)},
                {"activity", "other"}};
      if (auto v = col_double(s, 3)) w["durationS"] = std::lround(*v);
      json meta = json::parse(s.text(4), nullptr, false);
      if (meta.is_object()) {
        if (auto it = meta.find("activity"); it != meta.end() && it->is_string() &&
                                             !it->get<std::string>().empty() &&
                                             *it != "type3000") // older app builds
          w["activity"] = *it;
        if (auto it = meta.find("energyKcal"); it != meta.end() && it->is_number())
          w["kcal"] = round_to(it->get<double>(), 1);
        if (auto it = meta.find("distanceM"); it != meta.end() && it->is_number())
          w["distanceKm"] = round_to(it->get<double>() / 1000, 2);
        if (auto it = meta.find("avgHeartRate"); it != meta.end() && it->is_number())
          w["avgHeartRate"] = std::lround(it->get<double>());
      }
      list.push_back(std::move(w));
    }
  }
  return {{"workouts", list}};
}

json Collector::history_locations(const std::string &user, int y, int m, int d) {
  json points = json::array(), visits = json::array();
  const int64_t from = local_ms(y, m, d, 0, 0), to = local_ms(y, m, d, 1, 0);
  if (db_) {
    Stmt s(db_, "SELECT t, lat, lon, accuracy, speed FROM locations WHERE user = ? "
                "AND kind = 'point' AND t >= ? AND t < ? ORDER BY t");
    if (s) {
      s.bind(1, user);
      s.bind(2, from);
      s.bind(3, to);
      while (s.step() == SQLITE_ROW) {
        json p = {{"t", s.int64(0)},
                  {"lat", sqlite3_column_double(s.get(), 1)},
                  {"lon", sqlite3_column_double(s.get(), 2)}};
        if (auto v = col_double(s, 3)) p["accuracy"] = round_to(*v, 1);
        if (auto v = col_double(s, 4); v && *v >= 0) p["speed"] = round_to(*v, 2);
        points.push_back(std::move(p));
      }
    }
    if (points.size() > kMaxHistoryPoints) { // thin evenly, keep first and last
      json thin = json::array();
      const size_t n = points.size();
      for (size_t i = 0; i < kMaxHistoryPoints; ++i)
        thin.push_back(std::move(points[i * (n - 1) / (kMaxHistoryPoints - 1)]));
      points = std::move(thin);
    }
    // Visits overlapping the day (still open ones too).
    Stmt v(db_, "SELECT arrive_ms, depart_ms, lat, lon FROM locations WHERE user = ? "
                "AND kind = 'visit' AND COALESCE(arrive_ms, t) < ? "
                "AND COALESCE(depart_ms, 9223372036854775807) >= ? "
                "ORDER BY COALESCE(arrive_ms, t)");
    if (v) {
      v.bind(1, user);
      v.bind(2, to);
      v.bind(3, from);
      while (v.step() == SQLITE_ROW) {
        json o = {{"lat", sqlite3_column_double(v.get(), 2)},
                  {"lon", sqlite3_column_double(v.get(), 3)}};
        if (sqlite3_column_type(v.get(), 0) != SQLITE_NULL) o["arrive"] = v.int64(0);
        if (sqlite3_column_type(v.get(), 1) != SQLITE_NULL) o["depart"] = v.int64(1);
        visits.push_back(std::move(o));
      }
    }
  }
  return {{"points", points}, {"visits", visits}};
}
