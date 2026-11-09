#pragma once

// Personal data collected by the iOS companion app (HomeCollect):
// HealthKit samples, location history and phone stats, stored in SQLite
// (DATA_DIR/collect.db, mode 0600).
//
// Devices authenticate with a bearer token they get once via username +
// password. Only the token's SHA-256 is stored. All SQL uses prepared
// statements with bound parameters (no string-built queries).
//
// Not thread safe: use one instance per thread (the CLI opens its own).

#include "dashboard.pb.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <array>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

class Collector {
public:
  explicit Collector(std::filesystem::path db_file);
  ~Collector();
  Collector(const Collector &) = delete;
  Collector &operator=(const Collector &) = delete;

  bool open(std::string &error);
  bool is_open() const { return db_ != nullptr; }

  struct Device {
    std::string id, user, name;
    int64_t created = 0, last_seen = 0; // Unix ms
  };
  // Registers a device and returns its raw token (shown/sent exactly once).
  std::string add_device(const std::string &user, const std::string &name, std::string &id_out);
  std::optional<Device> device_for_token(const std::string &token);
  std::vector<Device> devices();
  bool remove_device(const std::string &id);
  int remove_user_devices(const std::string &user);

  // Validates and stores one upload. Returns per-category counts, or sets
  // `error` (nothing is stored then: one transaction per upload).
  nlohmann::json ingest(const std::string &user, const nlohmann::json &body, std::string &error);
  // Counts and newest timestamps, for the app's sync screen.
  nlohmann::json status(const std::string &user);
  // Today's totals, latest vitals, last night's sleep, last workout,
  // position and battery. False if the user never uploaded anything.
  bool summary(const std::string &user, dashboard_backend::Activity &out);
  std::vector<std::string> users_with_data();

  // History for the app (GET /api/history/*), JSON as sent to the client.
  // Days are local calendar days of the server; times are Unix ms.
  // Strict "YYYY-MM-DD" (2000..2100, real calendar date).
  static bool valid_date(std::string_view s, int &y, int &m, int &d);
  nlohmann::json history_daily(const std::string &user, int days); // incl. today
  nlohmann::json history_heartrate(const std::string &user, int y, int m, int d);
  nlohmann::json history_sleep(const std::string &user, int y, int m, int d);
  nlohmann::json history_workouts(const std::string &user, int days);
  nlohmann::json history_locations(const std::string &user, int y, int m, int d);
  // Places you spent time at in the last `days` days (visits clustered within
  // ~150 m), most time first, plus the newest known position.
  nlohmann::json history_places(const std::string &user, int days);

  // Work place guessed from the location history: the place (not home) where
  // the user spent the most weekday time between 07:00 and 19:00 in the last
  // 28 days. Needs >= 6 h there on >= 3 different days, else nullopt.
  // home = {lat, lon, radius m} of the home zone (places near it are skipped).
  struct WorkZone {
    double lat = 0, lon = 0, radius = 0; // radius in metres (for the geofence)
    int hours = 0, days = 0;             // evidence: weekday daytime hours / days
  };
  std::optional<WorkZone> work_place(const std::string &user, const std::array<double, 3> &home);

  static constexpr size_t kMaxItems = 10000; // per upload
  static constexpr size_t kMaxHistoryPoints = 5000; // per day in history_locations

private:
  bool exec(const char *sql, std::string &error);

  // Location history as "stays": CLVisits from the phone plus pauses of
  // >= 5 min between two position reports (iOS only reports real moves).
  struct LocPos {
    int64_t t;
    double lat, lon;
  };
  struct LocStay {
    int64_t start, end;
    double lat, lon;
  };
  // Fills the point timeline (oldest first) and the stays since `from`.
  void load_stays(const std::string &user, int64_t from, int64_t now,
                  std::vector<LocPos> &timeline, std::vector<LocStay> &stays);

  std::filesystem::path file_;
  sqlite3 *db_ = nullptr;
};
