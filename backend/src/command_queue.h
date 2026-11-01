#pragma once

#include "dashboard.pb.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

// Commands from WebSocket clients (loop thread) to the worker thread.
class CommandQueue {
public:
  using Clock = std::chrono::steady_clock;

  void push(dashboard_backend::Command cmd) {
    {
      std::lock_guard lock(m_);
      q_.push_back(std::move(cmd));
    }
    cv_.notify_one();
  }

  // Waits until a command arrives or the deadline passes.
  std::deque<dashboard_backend::Command> wait_until(Clock::time_point deadline) {
    std::unique_lock lock(m_);
    cv_.wait_until(lock, deadline, [&] { return !q_.empty(); });
    std::deque<dashboard_backend::Command> out;
    out.swap(q_);
    return out;
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<dashboard_backend::Command> q_;
};
