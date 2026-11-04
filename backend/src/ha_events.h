#pragma once

#include "config.h"

#include <functional>
#include <string>
#include <vector>

// Subscribes to Home Assistant's WebSocket API and calls `on_change` whenever
// one of the dashboard's entities changes state, so updates arrive within
// milliseconds instead of waiting for the next poll.
// https://developers.home-assistant.io/docs/api/websocket
//
// Optionally also listens for `mobile_app_notification_action` (a button in
// a push notification was tapped) and passes the action id to `on_action`.
//
// run() blocks forever (reconnects with backoff) — give it its own thread.
class HaEventStream {
public:
  HaEventStream(const Config &cfg, std::function<void()> on_change,
                std::function<void(const std::string &)> on_action = {});
  // Action id from one WebSocket message of the action subscription, if any.
  static std::string action_of(const std::string &message);
  [[noreturn]] void run();

private:
  enum class Recv { Message, Timeout, Closed };

  bool session(); // one connection; returns true if it got as far as auth_ok
  Recv recv(std::string &out, int timeout_ms);
  bool send(const std::string &text);

  const Config &cfg_;
  std::function<void()> on_change_;
  std::function<void(const std::string &)> on_action_;
  std::vector<std::string> entities_;
  std::string ws_url_;
  void *curl_ = nullptr;
  std::string partial_;
};
