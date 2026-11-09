#pragma once

// Notifications for the HomeCollect app. Every push the automations send
// lands here (per user, with an increasing id) and is handed to the app's
// live stream at once (GET /api/notifications/stream). When iOS suspended
// the app, it catches up with GET /api/notifications?after=<id> the next
// time it runs. Kept in DATA_DIR/notifications.json (0600), so a restart
// does not lose or repeat anything. Thread safe (worker adds, web reads).

#include "auth.h" // write_private_file

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

class NotificationInbox {
public:
  static constexpr size_t kKeepPerUser = 50;

  void open(std::filesystem::path file) {
    std::lock_guard lock(m_);
    file_ = std::move(file);
    std::ifstream f(file_);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    next_id_ = j.value("nextId", int64_t{1});
    if (j.contains("users") && j["users"].is_object())
      for (auto &[user, list] : j["users"].items())
        if (list.is_array())
          for (auto &n : list) users_[user].push_back(n);
  }

  // Worker thread: hands each new notification to the web server's streams.
  void on_new(std::function<void(const std::string &user, const nlohmann::json &n)> cb) {
    std::lock_guard lock(m_);
    listener_ = std::move(cb);
  }

  // Stores a notification and returns it with its id. `data` uses the HA
  // companion-app keys we already send (tag, url, actions, push).
  nlohmann::json add(const std::string &user, const std::string &title, const std::string &message,
                     const nlohmann::json &data) {
    nlohmann::json n = {{"title", title}, {"message", message}, {"t", now_ms()}};
    if (data.is_object()) {
      if (data.contains("tag")) n["tag"] = data["tag"];
      if (data.contains("url")) n["url"] = data["url"];
      if (data.contains("actions")) n["actions"] = data["actions"];
      auto push = data.find("push");
      if (push != data.end() && push->is_object() &&
          push->value("interruption-level", "") == "time-sensitive")
        n["timeSensitive"] = true;
    }
    std::function<void(const std::string &, const nlohmann::json &)> cb;
    {
      std::lock_guard lock(m_);
      n["id"] = next_id_++;
      auto &list = users_[user];
      list.push_back(n);
      while (list.size() > kKeepPerUser) list.pop_front();
      save_locked();
      cb = listener_;
    }
    if (cb) cb(user, n);
    return n;
  }

  // Everything newer than `after`, oldest first; `latest` = newest id overall.
  // Marks what it returns as fetched by the app (delivery.fetched = time).
  nlohmann::json since(const std::string &user, int64_t after) {
    std::lock_guard lock(m_);
    nlohmann::json list = nlohmann::json::array();
    int64_t latest = 0;
    bool changed = false;
    if (auto it = users_.find(user); it != users_.end())
      for (auto &n : it->second) {
        int64_t id = n.value("id", int64_t{0});
        latest = std::max(latest, id);
        if (id <= after) continue;
        nlohmann::json out = n;
        out.erase("delivery"); // server-side bookkeeping only
        list.push_back(std::move(out));
        if (!n["delivery"].contains("fetched")) {
          n["delivery"]["fetched"] = now_ms();
          changed = true;
        }
      }
    if (changed) save_locked();
    return {{"notifications", list}, {"latest", latest}};
  }

  // Delivery bookkeeping for the overview page: key = "stream" (number of
  // live app connections it was sent to), "ha" ("ok" / "failed: ..." / "off").
  void mark(const std::string &user, int64_t id, const std::string &key, nlohmann::json value) {
    std::lock_guard lock(m_);
    auto it = users_.find(user);
    if (it == users_.end()) return;
    for (auto &n : it->second)
      if (n.value("id", int64_t{0}) == id) {
        n["delivery"][key] = std::move(value);
        save_locked();
        return;
      }
  }

  // Newest `count` notifications of a user, newest first, with delivery info.
  nlohmann::json recent(const std::string &user, size_t count) {
    std::lock_guard lock(m_);
    nlohmann::json out = nlohmann::json::array();
    if (auto it = users_.find(user); it != users_.end())
      for (auto n = it->second.rbegin(); n != it->second.rend() && out.size() < count; ++n)
        out.push_back(*n);
    return out;
  }

private:
  static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  void save_locked() {
    if (file_.empty()) return;
    nlohmann::json users = nlohmann::json::object();
    for (const auto &[user, list] : users_) users[user] = nlohmann::json(list);
    std::string err;
    if (!auth::write_private_file(file_, nlohmann::json{{"nextId", next_id_}, {"users", users}}.dump(),
                                  err))
      std::cerr << "[Notify] cannot save inbox: " << err << "\n";
  }

  std::mutex m_;
  std::filesystem::path file_;
  int64_t next_id_ = 1;
  std::map<std::string, std::deque<nlohmann::json>> users_;
  std::function<void(const std::string &, const nlohmann::json &)> listener_;
};
