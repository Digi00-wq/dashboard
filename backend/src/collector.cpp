#include "collector.h"

#include "auth.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>

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
