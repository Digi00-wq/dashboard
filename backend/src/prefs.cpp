// Implementation of per-user preferences (see prefs.h), stored in DATA_DIR/prefs.json.

#include "prefs.h"

#include "auth.h" // write_private_file

#include <fstream>

using nlohmann::json;
namespace fs = std::filesystem;

// Prefs as JSON for GET /api/prefs.
json Prefs::to_json() const {
  return {{"language", language},
          {"notifyLightsLeftOn", notify_lights_left_on},
          {"lightsIdleMin", lights_idle_min},
          {"notifyServer", notify_server},
          {"morningBriefing", morning_briefing},
          {"notifyViaHa", notify_via_ha},
          {"arriveLights", arrive_lights},
          {"leaveLightsOff", leave_lights_off},
          {"leavePauseMedia", leave_pause_media},
          {"welcomeVoice", welcome_voice},
          {"roomAlarm", room_alarm},
          {"umbrellaReminder", umbrella_reminder},
          {"homeworkReminder", homework_reminder},
          {"shoppingReminder", shopping_reminder},
          {"adaptiveWakeLight", adaptive_wake_light},
          {"bedtimeReminder", bedtime_reminder}};
}

// Applies a partial JSON update (PUT /api/prefs) with validation.
bool Prefs::merge(const json &j, std::string &error) {
  if (!j.is_object()) {
    error = "object expected";
    return false;
  }
  Prefs next = *this;
  auto flag = [&](const char *key, bool &field) {
    if (!j.contains(key)) return true;
    if (!j[key].is_boolean()) {
      error = std::string(key) + ": boolean expected";
      return false;
    }
    field = j[key].get<bool>();
    return true;
  };
  if (j.contains("language")) {
    if (!j["language"].is_string() || (j["language"] != "de" && j["language"] != "en")) {
      error = "language: \"de\" or \"en\"";
      return false;
    }
    next.language = j["language"].get<std::string>();
  }
  if (j.contains("lightsIdleMin")) {
    if (!j["lightsIdleMin"].is_number_integer() || j["lightsIdleMin"].get<int>() < 5 ||
        j["lightsIdleMin"].get<int>() > 240) {
      error = "lightsIdleMin: 5..240";
      return false;
    }
    next.lights_idle_min = j["lightsIdleMin"].get<int>();
  }
  if (!flag("notifyLightsLeftOn", next.notify_lights_left_on) ||
      !flag("notifyServer", next.notify_server) ||
      !flag("morningBriefing", next.morning_briefing) ||
      !flag("notifyViaHa", next.notify_via_ha) ||
      !flag("arriveLights", next.arrive_lights) ||
      !flag("leaveLightsOff", next.leave_lights_off) ||
      !flag("leavePauseMedia", next.leave_pause_media) ||
      !flag("welcomeVoice", next.welcome_voice) ||
      !flag("roomAlarm", next.room_alarm) ||
      !flag("umbrellaReminder", next.umbrella_reminder) ||
      !flag("homeworkReminder", next.homework_reminder) ||
      !flag("shoppingReminder", next.shopping_reminder) ||
      !flag("adaptiveWakeLight", next.adaptive_wake_light) ||
      !flag("bedtimeReminder", next.bedtime_reminder))
    return false;
  *this = next;
  return true;
}

PrefsStore::PrefsStore(fs::path file) : file_(std::move(file)) {
  std::lock_guard lock(m_);
  load_locked();
}

void PrefsStore::load_locked() {
  std::error_code ec;
  auto t = fs::last_write_time(file_, ec);
  if (ec || t == mtime_) return;
  mtime_ = t;
  std::ifstream f(file_);
  json j = json::parse(f, nullptr, false);
  if (!j.is_object()) return;
  prefs_.clear();
  for (auto &[user, pj] : j.items()) {
    Prefs p;
    std::string err;
    p.merge(pj, err); // ignores bad values, keeps defaults
    prefs_[user] = p;
  }
}

// Prefs of a user (defaults if none).
Prefs PrefsStore::get(const std::string &user) {
  std::lock_guard lock(m_);
  load_locked();
  auto it = prefs_.find(user);
  return it == prefs_.end() ? Prefs{} : it->second;
}

// Saves prefs of a user.
bool PrefsStore::set(const std::string &user, const Prefs &p, std::string &error) {
  std::lock_guard lock(m_);
  prefs_[user] = p;
  json j = json::object();
  for (const auto &[u, pr] : prefs_) j[u] = pr.to_json();
  if (!auth::write_private_file(file_, j.dump(2), error)) return false;
  std::error_code ec;
  mtime_ = fs::last_write_time(file_, ec);
  return true;
}
