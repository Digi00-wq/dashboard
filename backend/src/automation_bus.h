#pragma once

// Hand-over between the web thread (HTTP routes) and the worker thread
// (automations). Thread safe.

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct PresenceEvent {
  std::string user;
  bool arrive = false; // false = left home
  int64_t t = 0;       // Unix ms (from the phone)
};

class AutomationBus {
public:
  // web -> worker: iPhone entered / left the home region (POST /api/presence)
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
  std::map<std::string, nlohmann::json> briefing_;
  std::map<std::string, nlohmann::json> pattern_;
  std::optional<std::array<double, 3>> home_;
};
