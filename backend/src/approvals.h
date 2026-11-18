#pragma once

// Approval requests from the assistant hub (agent VM, /srv/projekte/agent-hub).
// The hub asks before risky actions: POST /api/approvals (web.cpp, Bearer
// HUB_APPROVAL_TOKEN) creates a request here and a notification with
// "Erlauben"/"Ablehnen" buttons (HUB_APPROVE:<id> / HUB_DENY:<id>) to the
// owner's HomeCollect app and HA app. A button tap (POST /api/notifications/
// action, or the HA notification-action event in main.cpp) decides it; the hub
// polls GET /api/approvals/<id>. No answer before `expires` = expired (= no).
// In memory only: after a restart old requests read as unknown (= no).
// Thread safe (web thread + HA event thread).

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>

class Approvals {
public:
  static Approvals &instance() {
    static Approvals a;
    return a;
  }

  static constexpr const char *kApprovePrefix = "HUB_APPROVE:";
  static constexpr const char *kDenyPrefix = "HUB_DENY:";

  // New pending request; returns its id (32 hex chars).
  std::string create(const std::string &title, const std::string &message, int ttl_s) {
    std::lock_guard lock(m_);
    prune_locked();
    std::string id = random_id();
    int64_t now = now_s();
    items_[id] = {title, message, "pending", "", now, now + ttl_s, 0};
    return id;
  }

  // {"id","status": pending|approved|denied|expired,"by","decidedAt"} or nullopt.
  std::optional<nlohmann::json> get(const std::string &id) {
    std::lock_guard lock(m_);
    auto it = items_.find(id);
    if (it == items_.end()) return std::nullopt;
    auto &a = it->second;
    if (a.status == "pending" && now_s() > a.expires) a.status = "expired";
    nlohmann::json j = {{"id", id}, {"status", a.status}, {"title", a.title},
                        {"created", a.created}, {"expires", a.expires}};
    if (!a.by.empty()) j["by"] = a.by, j["decidedAt"] = a.decided;
    return j;
  }

  // Handles a notification action; true if it was an approval button.
  // `by` = who tapped (user name or "ha-app"). Late/duplicate taps are ignored.
  bool handle_action(const std::string &action, const std::string &by) {
    bool approve = action.rfind(kApprovePrefix, 0) == 0;
    bool deny = action.rfind(kDenyPrefix, 0) == 0;
    if (!approve && !deny) return false;
    std::string id = action.substr(approve ? std::string(kApprovePrefix).size()
                                           : std::string(kDenyPrefix).size());
    std::lock_guard lock(m_);
    auto it = items_.find(id);
    if (it == items_.end()) return true;
    auto &a = it->second;
    if (a.status != "pending" || now_s() > a.expires) return true;
    a.status = approve ? "approved" : "denied";
    a.by = by;
    a.decided = now_s();
    return true;
  }

private:
  struct Item {
    std::string title, message, status, by;
    int64_t created, expires, decided;
  };

  static int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static std::string random_id() {
    static std::random_device rd;
    static const char *hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) s += hex[rd() % 16];
    return s;
  }

  void prune_locked() {
    int64_t cutoff = now_s() - 86400;
    for (auto it = items_.begin(); it != items_.end();)
      it = it->second.expires < cutoff ? items_.erase(it) : std::next(it);
    while (items_.size() > 200) items_.erase(items_.begin());
  }

  std::mutex m_;
  std::map<std::string, Item> items_;
};
