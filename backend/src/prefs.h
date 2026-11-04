#pragma once

// Per-user preferences for automations and notifications, set from the iOS
// app's settings page (GET/PUT /api/prefs). Stored in DATA_DIR/prefs.json.
// PrefsStore is thread safe (web thread writes, worker reads).

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

struct Prefs {
  std::string language = "de"; // "de" | "en": texts of notifications

  // Notifications (via HA companion app push)
  bool notify_lights_left_on = true; // lights on, no motion for lights_idle_min
  int lights_idle_min = 30;          // 5..240
  bool notify_server = true;         // disk / RAM / temperature / HA / internet problems
  bool morning_briefing = true;      // push when you get up, tap -> details in the app

  // Presence from the iPhone (arrive / leave home)
  bool arrive_lights = true;     // lights on when arriving after sunset
  bool leave_lights_off = true;  // all lights off when leaving
  bool leave_pause_media = true; // pause music when leaving

  // Experiment: learned sleep pattern
  bool adaptive_wake_light = false; // fade lights in before the usual wake time (no alarm needed)
  bool bedtime_reminder = false;    // push + dim lights before the usual bedtime

  nlohmann::json to_json() const;
  // Applies only the known keys present in `j` (partial update). Returns
  // false (and sets error) on a wrong type / out-of-range value.
  bool merge(const nlohmann::json &j, std::string &error);
};

class PrefsStore {
public:
  explicit PrefsStore(std::filesystem::path file);
  Prefs get(const std::string &user);
  bool set(const std::string &user, const Prefs &p, std::string &error);

private:
  void load_locked();
  std::filesystem::path file_;
  std::mutex m_;
  std::map<std::string, Prefs> prefs_;
  std::filesystem::file_time_type mtime_{};
};
