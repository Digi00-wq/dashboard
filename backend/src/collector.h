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

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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

  static constexpr size_t kMaxItems = 10000; // per upload

private:
  bool exec(const char *sql, std::string &error);

  std::filesystem::path file_;
  sqlite3 *db_ = nullptr;
};
