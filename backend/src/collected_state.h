#pragma once

#include "dashboard.pb.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

// Latest HomeCollect summary per user, handed from the web thread (uploads)
// to the worker (dashboard snapshot + publishing to Home Assistant).
class CollectedState {
public:
  void update(const std::string &user, dashboard_backend::Activity a) {
    std::lock_guard lock(m_);
    by_user_[user] = std::move(a);
    dirty_.insert(user);
  }

  // Copies all summaries; `changed` gets the users updated since last call.
  std::map<std::string, dashboard_backend::Activity> take(std::set<std::string> &changed) {
    std::lock_guard lock(m_);
    changed.swap(dirty_);
    dirty_.clear();
    return by_user_;
  }

private:
  std::mutex m_;
  std::map<std::string, dashboard_backend::Activity> by_user_;
  std::set<std::string> dirty_;
};
