#pragma once

#include "auth.h"
#include "command_queue.h"
#include "config.h"
#include "cover_cache.h"
#include "dashboard.pb.h"

#include <App.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

// HTTP + WebSocket server: static frontend, login/session API, user
// administration, and per-user filtered live dashboard data.
class WebServer {
public:
  WebServer(const Config &cfg, CommandQueue &commands, CoverCache &covers);

  // Thread safe: hands a new full snapshot to the event loop.
  void publish(std::string snapshot_bytes);
  // Blocks forever.
  void run();

  struct SocketData {
    std::string user;
    std::string session_hash;
    uint32_t mask = 0;
  };
  using WS = uWS::WebSocket<false, true, SocketData>;
  using Res = uWS::HttpResponse<false>;

  struct Req {
    std::string token, origin, host, content_type, ip;
  };

private:
  void routes();
  void websocket();

  Req capture(Res *res, uWS::HttpRequest *req) const;
  bool origin_ok(const Req &r) const;
  const auth::User *current_user(const Req &r);

  using BodyHandler = std::function<void(Res *, Req &, nlohmann::json &,
                                         std::shared_ptr<bool> aborted)>;
  void with_json_body(Res *res, uWS::HttpRequest *req, BodyHandler handler);
  // Runs `work` (e.g. Argon2) on a helper thread, then `done` on the loop.
  void offload(Res *res, std::shared_ptr<bool> aborted, std::function<std::string()> work,
               std::function<void(std::string)> done);

  void on_snapshot(std::string bytes);
  void send_filtered(WS *ws);

  // Server-Sent Events: push fallback for networks that break WebSockets.
  struct SseClient {
    std::string user;
    std::string session_hash;
    uint32_t mask = 0;
    bool congested = false; // last write hit backpressure
    std::string pending;    // newest snapshot event held back meanwhile
  };
  void events_route();
  void sse_write(Res *res, SseClient &c, const std::string &chunk, bool droppable);
  static void heartbeat(struct us_timer_t *t);
  void close_sockets(const std::string &user, const std::string &session_hash = "");

  // Route handlers
  void login(Res *, Req &, nlohmann::json &, std::shared_ptr<bool>);
  void change_own_password(Res *, Req &, nlohmann::json &, std::shared_ptr<bool>);
  void create_user(Res *, Req &, nlohmann::json &, std::shared_ptr<bool>);
  void update_user(Res *, Req &, const std::string &name, nlohmann::json &,
                   std::shared_ptr<bool>);

  const Config &cfg_;
  CommandQueue &commands_;
  CoverCache &covers_;
  uWS::App app_;
  uWS::Loop *loop_;

  auth::UserStore users_;
  auth::Sessions sessions_;
  auth::LoginLimiter limiter_;
  int hashing_ = 0; // concurrent Argon2 jobs (loop thread only)

  dashboard_backend::Dashboard latest_;
  bool has_latest_ = false;
  std::unordered_set<WS *> sockets_;
  std::unordered_map<Res *, SseClient> sse_;
  struct us_timer_t *heartbeat_ = nullptr;
};
