// Implementation of HaEventStream (see ha_events.h): HA WebSocket API client
// (auth, subscribe to state_changed + notification actions, reconnect).

#include "ha_events.h"
#include "home_assistant.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <iostream>
#include <thread>

using nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
constexpr auto kDebounce = 150ms;      // batch bursts (e.g. brightness changes)
constexpr auto kPingEvery = 30s;       // keep-alive towards HA
constexpr auto kDeadAfter = 75s;       // nothing received for this long -> reconnect
constexpr int kTriggerId = 1;
constexpr int kActionId = 2; // mobile_app_notification_action events
} // namespace

HaEventStream::HaEventStream(const Config &cfg, std::function<void()> on_change,
                             std::function<void(const std::string &)> on_action)
    : cfg_(cfg), on_change_(std::move(on_change)), on_action_(std::move(on_action)) {
  entities_ = HomeAssistant::watched_entities(cfg);

  // http://host:port -> ws://host:port/api/websocket (https -> wss)
  ws_url_ = cfg.ha_url;
  if (ws_url_.rfind("https://", 0) == 0) ws_url_ = "wss://" + ws_url_.substr(8);
  else if (ws_url_.rfind("http://", 0) == 0) ws_url_ = "ws://" + ws_url_.substr(7);
  ws_url_ += "/api/websocket";
}

// Reconnect loop (never returns).
void HaEventStream::run() {
  auto backoff = 1s;
  for (;;) {
    bool authed = session();
    if (authed) backoff = 1s; // was a healthy connection -> retry quickly
    std::cerr << "[HA-WS] disconnected, reconnecting in " << backoff.count() << "s\n";
    std::this_thread::sleep_for(backoff);
    backoff = std::min(backoff * 2, std::chrono::seconds(60));
  }
}

// One connection: auth, subscribe, read events until it breaks.
bool HaEventStream::session() {
  CURL *c = curl_easy_init();
  if (!c) return false;
  curl_ = c;
  partial_.clear();
  bool authed = false;

  auto finish = [&] {
    curl_easy_cleanup(c);
    curl_ = nullptr;
    return authed;
  };

  curl_easy_setopt(c, CURLOPT_URL, ws_url_.c_str());
  curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L); // 2 = do the WS upgrade
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  if (CURLcode rc = curl_easy_perform(c); rc != CURLE_OK) {
    std::cerr << "[HA-WS] connect failed: " << curl_easy_strerror(rc) << "\n";
    return finish();
  }

  std::string msg;
  auto expect = [&](const char *type) -> std::optional<json> {
    if (recv(msg, 10000) != Recv::Message) return std::nullopt;
    try {
      auto j = json::parse(msg);
      if (type && j.value("type", "") != type) {
        std::cerr << "[HA-WS] expected " << type << ", got " << msg.substr(0, 200) << "\n";
        return std::nullopt;
      }
      return j;
    } catch (...) {
      return std::nullopt;
    }
  };

  // 1. Authenticate
  if (!expect("auth_required")) return finish();
  send(json{{"type", "auth"}, {"access_token", cfg_.ha_token}}.dump());
  if (!expect("auth_ok")) {
    std::cerr << "[HA-WS] authentication failed (check HA_TOKEN)\n";
    return finish();
  }
  authed = true;

  // 2. Subscribe to state changes of exactly our entities (filtered by HA).
  json sub = {{"id", kTriggerId},
              {"type", "subscribe_trigger"},
              {"trigger", {{"platform", "state"}, {"entity_id", entities_}}}};
  send(sub.dump());
  auto res = expect("result");
  if (!res || !res->value("success", false)) {
    std::cerr << "[HA-WS] subscribe failed: " << msg.substr(0, 300) << "\n";
    return finish();
  }
  std::cout << "[HA-WS] live updates for " << entities_.size() << " entities\n";
  // Buttons in our push notifications (answer handled in the loop below).
  if (on_action_)
    send(json{{"id", kActionId},
              {"type", "subscribe_events"},
              {"event_type", "mobile_app_notification_action"}}
             .dump());
  on_change_(); // resync anything we missed while disconnected

  // 3. Event loop
  int next_id = kActionId + 1;
  auto last_rx = Clock::now();
  auto next_ping = Clock::now() + kPingEvery;
  std::optional<Clock::time_point> fire_at; // pending debounced notification

  for (;;) {
    auto now = Clock::now();
    auto wake = std::min(next_ping, fire_at.value_or(next_ping));
    int timeout = int(std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(wake - now).count()));

    Recv r = recv(msg, timeout);
    if (r == Recv::Closed) return finish();
    now = Clock::now();

    if (r == Recv::Message) {
      last_rx = now;
      try {
        auto j = json::parse(msg);
        std::string type = j.value("type", "");
        int id = j.value("id", 0);
        if (type == "event" && id == kTriggerId && !fire_at)
          fire_at = now + kDebounce;
        else if (type == "event" && id == kActionId && on_action_) {
          std::string action = action_of(msg);
          if (!action.empty()) on_action_(action);
        } else if (type == "result" && id == kActionId && !j.value("success", false))
          std::cerr << "[HA-WS] subscribe to notification actions failed: "
                    << msg.substr(0, 300) << "\n";
      } catch (...) {
      }
    }

    if (fire_at && now >= *fire_at) {
      fire_at.reset();
      on_change_();
    }
    if (now >= next_ping) {
      if (now - last_rx > kDeadAfter) {
        std::cerr << "[HA-WS] no response from Home Assistant\n";
        return finish();
      }
      send(json{{"id", next_id++}, {"type", "ping"}}.dump());
      next_ping = now + kPingEvery;
    }
  }
}

HaEventStream::Recv HaEventStream::recv(std::string &out, int timeout_ms) {
  auto *c = static_cast<CURL *>(curl_);
  auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[16384];

  for (;;) {
    size_t n = 0;
    const struct curl_ws_frame *meta = nullptr;
    CURLcode rc = curl_ws_recv(c, buf, sizeof(buf), &n, &meta);

    if (rc == CURLE_AGAIN) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) return Recv::Timeout;
      curl_socket_t sock = CURL_SOCKET_BAD;
      curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock);
      pollfd pfd{sock, POLLIN, 0};
      int pr = ::poll(&pfd, 1, int(left));
      if (pr == 0) return Recv::Timeout;
      if (pr < 0) return Recv::Closed;
      continue;
    }
    if (rc != CURLE_OK || !meta) return Recv::Closed;
    if (meta->flags & CURLWS_CLOSE) return Recv::Closed;
    if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue; // libcurl answers pings

    partial_.append(buf, n);
    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
      out = std::move(partial_);
      partial_.clear();
      return Recv::Message;
    }
  }
}

// Sends a text frame.
bool HaEventStream::send(const std::string &text) {
  auto *c = static_cast<CURL *>(curl_);
  size_t off = 0;
  while (off < text.size()) {
    size_t sent = 0;
    CURLcode rc = curl_ws_send(c, text.data() + off, text.size() - off, &sent, 0, CURLWS_TEXT);
    if (rc == CURLE_AGAIN) {
      std::this_thread::sleep_for(5ms);
      continue;
    }
    if (rc != CURLE_OK) return false;
    off += sent;
  }
  return true;
}

// Notification action id from an event message ("" if none).
std::string HaEventStream::action_of(const std::string &message) {
  try {
    auto j = json::parse(message);
    const auto &ev = j.at("event");
    if (ev.value("event_type", "") != "mobile_app_notification_action") return {};
    const auto &data = ev.at("data");
    if (data.contains("action") && data["action"].is_string())
      return data["action"].get<std::string>();
  } catch (...) {
  }
  return {};
}
