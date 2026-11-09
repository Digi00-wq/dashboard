#pragma once

// Hand-over between the web thread (HTTP routes) and the worker thread
// (automations). Thread safe.

#include "notification_inbox.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct PresenceEvent {
  std::string user;
  bool arrive = false;          // false = left
  int64_t t = 0;                // Unix ms (from the phone)
  std::string place = "home";   // "home" | "work"
  std::string source = "geofence"; // "geofence" | "wifi" | "shortcut" (for logs / HA)
};

// One entry of the automation history (overview page).
struct AutomationEvent {
  int64_t t = 0;   // Unix ms
  std::string key; // automation key, e.g. "homework" (see automation_catalog())
  std::string text;
};

class AutomationBus {
public:
  // worker -> app: notifications (GET /api/notifications[/stream])
  NotificationInbox inbox;

  // worker -> web: what the automations did (newest last, max 300)
  void log(AutomationEvent e) {
    std::lock_guard lock(m_);
    last_[e.key] = e;
    events_.push_back(std::move(e));
    while (events_.size() > 300) events_.pop_front();
  }
  std::vector<AutomationEvent> events() {
    std::lock_guard lock(m_);
    return {events_.begin(), events_.end()};
  }
  std::map<std::string, AutomationEvent> last_events() {
    std::lock_guard lock(m_);
    return last_;
  }

  // worker -> web: current state for the overview (presence, HA automations...)
  void set_status(const std::string &key, nlohmann::json v) {
    std::lock_guard lock(m_);
    status_[key] = std::move(v);
  }
  nlohmann::json status() {
    std::lock_guard lock(m_);
    return status_;
  }

  // web -> worker: copy of a test notification through the HA app
  struct HaCopy {
    std::string user, title, message;
    int64_t id = 0; // inbox id (delivery result is marked there)
  };
  void push_ha_copy(HaCopy c) {
    std::lock_guard lock(m_);
    ha_copies_.push_back(std::move(c));
  }
  std::vector<HaCopy> take_ha_copies() {
    std::lock_guard lock(m_);
    std::vector<HaCopy> out;
    out.swap(ha_copies_);
    return out;
  }

  // web -> worker: iPhone arrived at / left home or work (POST /api/presence)
  void push_presence(PresenceEvent e) {
    std::lock_guard lock(m_);
    presence_.push_back(std::move(e));
  }
  std::vector<PresenceEvent> take_presence() {
    std::lock_guard lock(m_);
    std::vector<PresenceEvent> out;
    out.swap(presence_);
    return out;
  }

  // worker -> web: current morning briefing per user (GET /api/briefing)
  void set_briefing(const std::string &user, nlohmann::json b) {
    std::lock_guard lock(m_);
    briefing_[user] = std::move(b);
  }
  std::optional<nlohmann::json> briefing(const std::string &user) {
    std::lock_guard lock(m_);
    auto it = briefing_.find(user);
    if (it == briefing_.end()) return std::nullopt;
    return it->second;
  }

  // worker -> web: learned sleep pattern per user (GET /api/sleep-pattern)
  void set_sleep_pattern(const std::string &user, nlohmann::json p) {
    std::lock_guard lock(m_);
    pattern_[user] = std::move(p);
  }
  std::optional<nlohmann::json> sleep_pattern(const std::string &user) {
    std::lock_guard lock(m_);
    auto it = pattern_.find(user);
    if (it == pattern_.end()) return std::nullopt;
    return it->second;
  }

  // worker -> web: HA home zone {lat, lon, radius m} (GET /api/home)
  void set_home_zone(std::array<double, 3> z) {
    std::lock_guard lock(m_);
    home_ = z;
  }
  std::optional<std::array<double, 3>> home_zone() {
    std::lock_guard lock(m_);
    return home_;
  }

private:
  std::mutex m_;
  std::vector<PresenceEvent> presence_;
  std::deque<AutomationEvent> events_;
  std::map<std::string, AutomationEvent> last_;
  nlohmann::json status_ = nlohmann::json::object();
  std::vector<HaCopy> ha_copies_;
  std::map<std::string, nlohmann::json> briefing_;
  std::map<std::string, nlohmann::json> pattern_;
  std::optional<std::array<double, 3>> home_;
};
