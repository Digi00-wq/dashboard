#include "web.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

using nlohmann::json;
namespace fs = std::filesystem;
namespace dash = dashboard_backend;
using Res = WebServer::Res;
using WS = WebServer::WS;

namespace {

constexpr size_t kMaxBody = 16 * 1024;
constexpr int kMaxConcurrentHashing = 4;
constexpr const char *kCookie = "dash_session";

// Strict CSP: only our own scripts/styles/images; no framing, no plugins.
// style-src needs 'unsafe-inline' for Solid's style="" attributes (no user
// HTML is ever rendered, so this does not open an XSS vector).
constexpr const char *kCsp =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
    "img-src 'self' data:; font-src 'self'; connect-src 'self'; manifest-src 'self'; "
    "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'";

std::string_view status_text(int code) {
  switch (code) {
  case 200: return "200 OK";
  case 201: return "201 Created";
  case 204: return "204 No Content";
  case 400: return "400 Bad Request";
  case 401: return "401 Unauthorized";
  case 403: return "403 Forbidden";
  case 404: return "404 Not Found";
  case 409: return "409 Conflict";
  case 413: return "413 Payload Too Large";
  case 415: return "415 Unsupported Media Type";
  case 429: return "429 Too Many Requests";
  case 503: return "503 Service Unavailable";
  default: return "500 Internal Server Error";
  }
}

void security_headers(Res *res) {
  res->writeHeader("X-Content-Type-Options", "nosniff")
      ->writeHeader("X-Frame-Options", "DENY")
      ->writeHeader("Referrer-Policy", "no-referrer")
      ->writeHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=()")
      ->writeHeader("Cross-Origin-Opener-Policy", "same-origin")
      ->writeHeader("Cross-Origin-Resource-Policy", "same-origin")
      ->writeHeader("Content-Security-Policy", kCsp);
}

void send_json(Res *res, int code, const json &body, const std::string &set_cookie = "") {
  res->writeStatus(status_text(code));
  security_headers(res);
  res->writeHeader("Cache-Control", "no-store");
  if (!set_cookie.empty()) res->writeHeader("Set-Cookie", set_cookie);
  if (code == 204) {
    res->end();
    return;
  }
  res->writeHeader("Content-Type", "application/json")->end(body.dump());
}

void send_error(Res *res, int code, const std::string &error, json extra = json::object()) {
  extra["error"] = error;
  send_json(res, code, extra);
}

std::string session_cookie(const std::string &token, int days, bool secure) {
  std::string c = std::string(kCookie) + "=" + token +
                  "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(days * 86400);
  if (secure) c += "; Secure";
  return c;
}

std::string clear_cookie(bool secure) {
  std::string c = std::string(kCookie) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
  if (secure) c += "; Secure";
  return c;
}

std::string cookie_value(std::string_view header, std::string_view name) {
  size_t pos = 0;
  while (pos < header.size()) {
    size_t end = header.find(';', pos);
    std::string_view part = header.substr(pos, end == std::string_view::npos ? end : end - pos);
    while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
    if (part.size() > name.size() && part.substr(0, name.size()) == name &&
        part[name.size()] == '=')
      return std::string(part.substr(name.size() + 1));
    if (end == std::string_view::npos) break;
    pos = end + 1;
  }
  return {};
}

json user_json(const auth::User &u) {
  return {{"name", u.name},
          {"admin", u.admin},
          {"permissions", auth::perm_to_names(u.admin ? auth::ALL_FEATURES : u.perms)},
          {"disabled", u.disabled},
          {"created", u.created}};
}

std::optional<std::string> string_field(const json &j, const char *key) {
  if (!j.contains(key) || !j[key].is_string()) return std::nullopt;
  return j[key].get<std::string>();
}

std::string lower_trim(std::string s) {
  auto b = s.find_first_not_of(" \t\r\n");
  auto e = s.find_last_not_of(" \t\r\n");
  s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::optional<uint32_t> perms_field(const json &j) {
  if (!j.contains("permissions")) return std::nullopt;
  if (!j["permissions"].is_array()) return std::nullopt;
  std::vector<std::string> names;
  for (const auto &p : j["permissions"])
    if (p.is_string()) names.push_back(p.get<std::string>());
  return auth::perms_from_names(names);
}

// Removes everything a mask may not see.
dash::Dashboard filter(const dash::Dashboard &full, uint32_t mask) {
  dash::Dashboard d = full;
  if (!(mask & auth::WEATHER)) d.clear_weather();
  if (!(mask & auth::LIGHTS)) d.clear_lamps();
  if (!(mask & auth::ROOM)) {
    d.clear_presence();
    d.clear_light();
  }
  if (!(mask & auth::TASKS)) d.clear_todoist();
  if (!(mask & auth::MEDIA)) d.mutable_glance()->clear_media();
  if (!(mask & auth::HEALTH)) {
    d.clear_phone();
    d.clear_sleep();
    d.mutable_glance()->clear_next_alarm();
  }
  if (!(mask & auth::SERVER)) {
    d.clear_server();
    d.mutable_glance()->clear_network();
  }
  if (!(mask & auth::ADMIN_VIEW)) d.clear_errors();
  return d;
}

uint32_t required_perm(const dash::Command &cmd) {
  switch (cmd.cmd_case()) {
  case dash::Command::kLight:
  case dash::Command::kAllLights: return auth::LIGHTS;
  case dash::Command::kCompleteTask: return auth::TASKS;
  case dash::Command::kMedia: return auth::MEDIA;
  default: return 0; // refresh
  }
}

std::string topic(uint32_t mask) { return "d/" + std::to_string(mask); }

// ---------------------------------------------------------------------------
// Static files
// ---------------------------------------------------------------------------
std::string_view mime_for(const fs::path &p) {
  static const std::map<std::string, std::string_view> types = {
      {".html", "text/html; charset=utf-8"},
      {".js", "text/javascript; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},
      {".svg", "image/svg+xml"},
      {".png", "image/png"},
      {".jpg", "image/jpeg"},
      {".ico", "image/x-icon"},
      {".json", "application/json"},
      {".webmanifest", "application/manifest+json"},
      {".woff2", "font/woff2"},
      {".woff", "font/woff"},
      {".txt", "text/plain; charset=utf-8"},
  };
  auto it = types.find(p.extension().string());
  return it == types.end() ? "application/octet-stream" : it->second;
}

void serve_static(const fs::path &root, Res *res, std::string_view url) {
  std::string rel(url.substr(0, url.find('?')));
  while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
  if (rel.empty()) rel = "index.html";

  // No traversal, no hidden files, no odd bytes.
  if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos ||
      rel.find('\0') != std::string::npos || rel.find("/.") != std::string::npos ||
      rel.front() == '.') {
    res->writeStatus("400 Bad Request");
    security_headers(res);
    res->end("bad path");
    return;
  }

  fs::path file = root / rel;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    if (fs::path(rel).has_extension()) {
      res->writeStatus("404 Not Found");
      security_headers(res);
      res->end("not found");
      return;
    }
    file = root / "index.html"; // SPA fallback
  }

  std::ifstream f(file, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  bool immutable = rel.rfind("assets/", 0) == 0;
  res->writeStatus("200 OK");
  security_headers(res);
  res->writeHeader("Content-Type", mime_for(file))
      ->writeHeader("Cache-Control", immutable ? "public, max-age=31536000, immutable" : "no-cache")
      ->end(ss.str());
}

} // namespace

// ---------------------------------------------------------------------------

WebServer::WebServer(const Config &cfg, CommandQueue &commands, CoverCache &covers)
    : cfg_(cfg), commands_(commands), covers_(covers), loop_(uWS::Loop::get()),
      users_(fs::path(cfg.data_dir) / "users.json"),
      sessions_(fs::path(cfg.data_dir) / "sessions.json",
                std::chrono::hours(24 * cfg.session_days)) {
  std::string err;
  if (!users_.load(err)) std::cerr << "[Auth] " << err << "\n";
  sessions_.load();
  if (users_.empty())
    std::cout << "[Auth] No users yet. Create your admin account with:\n"
                 "         dashboard_backend --create-admin <name>\n"
                 "       (Docker: docker compose exec -it dashboard dashboard_backend "
                 "--create-admin <name>)\n";
  else
    std::cout << "[Auth] " << users_.all().size() << " user(s) loaded\n";
  websocket();
  routes();
}

void WebServer::publish(std::string bytes) {
  loop_->defer([this, bytes = std::move(bytes)]() mutable { on_snapshot(std::move(bytes)); });
}

void WebServer::run() {
  app_.listen(cfg_.port, [&](auto *token) {
        if (token) {
          std::cout << "[Server] listening on http://0.0.0.0:" << cfg_.port << "\n";
        } else {
          std::cerr << "[Server] could not bind port " << cfg_.port << "\n";
          std::exit(1);
        }
      }).run();
}

// ---------------------------------------------------------------------------
// Request helpers
// ---------------------------------------------------------------------------
WebServer::Req WebServer::capture(Res *res, uWS::HttpRequest *req) const {
  Req r;
  r.token = cookie_value(req->getHeader("cookie"), kCookie);
  r.origin = std::string(req->getHeader("origin"));
  r.host = std::string(req->getHeader("host"));
  r.content_type = std::string(req->getHeader("content-type"));
  if (cfg_.trust_proxy) {
    std::string xff(req->getHeader("x-forwarded-for"));
    r.ip = xff.substr(0, xff.find(','));
  }
  if (r.ip.empty()) r.ip = std::string(res->getRemoteAddressAsText());
  return r;
}

// Browsers always send Origin on cross-site POST and WebSocket requests;
// reject any that don't come from this very host (CSRF / CSWSH protection).
bool WebServer::origin_ok(const Req &r) const {
  if (r.origin.empty()) return true; // non-browser client (curl etc.)
  return r.origin == "http://" + r.host || r.origin == "https://" + r.host;
}

const auth::User *WebServer::current_user(const Req &r) {
  if (r.token.empty()) return nullptr;
  users_.reload_if_changed();
  auto name = sessions_.lookup(r.token);
  if (!name) return nullptr;
  const auth::User *u = users_.find(*name);
  if (!u || u->disabled) return nullptr;
  return u;
}

void WebServer::with_json_body(Res *res, uWS::HttpRequest *req, BodyHandler handler) {
  auto aborted = std::make_shared<bool>(false);
  auto body = std::make_shared<std::string>();
  auto too_big = std::make_shared<bool>(false);
  auto r = std::make_shared<Req>(capture(res, req));
  res->onAborted([aborted] { *aborted = true; });
  res->onData([=, this](std::string_view chunk, bool last) {
    if (body->size() + chunk.size() > kMaxBody) *too_big = true;
    else body->append(chunk);
    if (!last) return;
    if (*too_big) return send_error(res, 413, "body_too_large");
    if (!origin_ok(*r)) return send_error(res, 403, "bad_origin");
    // JSON-only API: HTML forms can't send this cross-site without CORS.
    if (r->content_type.rfind("application/json", 0) != 0)
      return send_error(res, 415, "json_required");
    json j = json::parse(*body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return send_error(res, 400, "invalid_json");
    handler(res, *r, j, aborted);
  });
}

void WebServer::offload(Res *res, std::shared_ptr<bool> aborted,
                        std::function<std::string()> work,
                        std::function<void(std::string)> done) {
  if (hashing_ >= kMaxConcurrentHashing) return send_error(res, 503, "busy");
  ++hashing_;
  std::thread([this, res, aborted, work = std::move(work), done = std::move(done)]() mutable {
    std::string result = work();
    loop_->defer([this, res, aborted, result = std::move(result), done = std::move(done)]() {
      --hashing_;
      if (*aborted) return;
      res->cork([&] { done(result); });
    });
  }).detach();
}

// ---------------------------------------------------------------------------
// Live data
// ---------------------------------------------------------------------------
void WebServer::on_snapshot(std::string bytes) {
  if (!latest_.ParseFromString(bytes)) return;
  has_latest_ = true;
  std::unordered_set<uint32_t> masks;
  for (auto *ws : sockets_) masks.insert(ws->getUserData()->mask);
  for (uint32_t mask : masks) {
    std::string out;
    if (!filter(latest_, mask).SerializeToString(&out)) continue;
    app_.publish(topic(mask), out, uWS::OpCode::BINARY);
  }
}

void WebServer::send_filtered(WS *ws) {
  if (!has_latest_) return;
  std::string out;
  if (!filter(latest_, ws->getUserData()->mask).SerializeToString(&out)) return;
  ws->send(out, uWS::OpCode::BINARY);
}

// Closing makes the client reconnect, which re-checks session + permissions.
void WebServer::close_sockets(const std::string &user, const std::string &session_hash) {
  std::vector<WS *> victims;
  for (auto *ws : sockets_) {
    auto *d = ws->getUserData();
    if (d->user == user && (session_hash.empty() || d->session_hash == session_hash))
      victims.push_back(ws);
  }
  for (auto *ws : victims) ws->end(4001, "session changed");
}

void WebServer::websocket() {
  app_.ws<SocketData>(
      "/ws",
      {.compression = uWS::DISABLED,
       .maxPayloadLength = 16 * 1024,
       .idleTimeout = 120,
       .sendPingsAutomatically = true,
       .upgrade =
           [this](Res *res, uWS::HttpRequest *req, struct us_socket_context_t *context) {
             Req r = capture(res, req);
             if (!origin_ok(r)) {
               res->writeStatus("403 Forbidden")->end();
               return;
             }
             const auth::User *u = current_user(r);
             if (!u) {
               res->writeStatus("401 Unauthorized")->end();
               return;
             }
             res->template upgrade<SocketData>(
                 {u->name, auth::sha256_hex(r.token), u->mask()},
                 req->getHeader("sec-websocket-key"), req->getHeader("sec-websocket-protocol"),
                 req->getHeader("sec-websocket-extensions"), context);
           },
       .open =
           [this](WS *ws) {
             sockets_.insert(ws);
             ws->subscribe(topic(ws->getUserData()->mask));
             send_filtered(ws);
           },
       .message =
           [this](WS *ws, std::string_view msg, uWS::OpCode op) {
             if (op != uWS::OpCode::BINARY) return;
             dash::Command cmd;
             if (!cmd.ParseFromArray(msg.data(), int(msg.size()))) return;
             uint32_t need = required_perm(cmd);
             if (need && !(ws->getUserData()->mask & need)) {
               std::cerr << "[WS] " << ws->getUserData()->user
                         << " tried a command without permission\n";
               return;
             }
             commands_.push(std::move(cmd));
           },
       .close = [this](WS *ws, int, std::string_view) { sockets_.erase(ws); }});
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------
void WebServer::routes() {
  app_.get("/healthz", [](Res *res, auto *) { res->end("ok"); });

  // --- session ---
  app_.get("/api/me", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (const auth::User *u = current_user(r)) return send_json(res, 200, user_json(*u));
    users_.reload_if_changed();
    send_error(res, 401, "unauthenticated", {{"setup", users_.empty()}});
  });

  app_.post("/api/login", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
      login(res, r, j, ab);
    });
  });

  app_.post("/api/logout", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &, std::shared_ptr<bool>) {
      if (auto name = sessions_.lookup(r.token)) {
        close_sockets(*name, auth::sha256_hex(r.token));
        sessions_.revoke(r.token);
      }
      send_json(res, 204, {}, clear_cookie(cfg_.cookie_secure));
    });
  });

  app_.post("/api/me/password", [this](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
      change_own_password(res, r, j, ab);
    });
  });

  // --- admin: users ---
  auto admin_only = [this](Res *res, const Req &r) -> const auth::User * {
    const auth::User *u = current_user(r);
    if (!u) send_error(res, 401, "unauthenticated");
    else if (!u->admin) send_error(res, 403, "forbidden");
    else return u;
    return nullptr;
  };

  app_.get("/api/users", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (!admin_only(res, r)) return;
    json list = json::array();
    for (const auto &u : users_.all()) list.push_back(user_json(u));
    json perms = json::array();
    for (const auto &[name, bit] : auth::perm_names()) perms.push_back(name);
    send_json(res, 200, {{"users", list}, {"permissions", perms}});
  });

  app_.post("/api/users", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    with_json_body(res, req, [this, admin_only](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
      if (admin_only(res, r)) create_user(res, r, j, ab);
    });
  });

  app_.put("/api/users/:name", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    std::string name(req->getParameter(0));
    with_json_body(res, req,
                   [this, admin_only, name](Res *res, Req &r, json &j, std::shared_ptr<bool> ab) {
                     if (admin_only(res, r)) update_user(res, r, name, j, ab);
                   });
  });

  app_.del("/api/users/:name", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    std::string name(req->getParameter(0));
    with_json_body(res, req, [this, admin_only, name](Res *res, Req &r, json &, std::shared_ptr<bool>) {
      const auth::User *me = admin_only(res, r);
      if (!me) return;
      if (name == me->name) return send_error(res, 409, "cannot_delete_self");
      if (!users_.remove(name)) return send_error(res, 404, "not_found");
      std::string err;
      if (!users_.save(err)) return send_error(res, 500, "save_failed");
      sessions_.revoke_user(name);
      close_sockets(name);
      std::cout << "[Auth] user deleted: " << name << "\n";
      send_json(res, 204, {});
    });
  });

  // --- data ---
  app_.get("/api/dashboard", [this, admin_only](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    if (!admin_only(res, r)) return;
    std::string out;
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    (void)google::protobuf::util::MessageToJsonString(latest_, &out, opts);
    res->writeStatus("200 OK");
    security_headers(res);
    res->writeHeader("Cache-Control", "no-store")
        ->writeHeader("Content-Type", "application/json")
        ->end(out);
  });

  app_.get("/api/cover/*", [this](Res *res, uWS::HttpRequest *req) {
    Req r = capture(res, req);
    const auth::User *u = current_user(r);
    if (!u) return send_error(res, 401, "unauthenticated");
    if (!(u->mask() & auth::MEDIA)) return send_error(res, 403, "forbidden");
    std::string entity(req->getUrl().substr(std::string_view("/api/cover/").size()));
    auto cover = covers_.get(entity);
    if (!cover) return send_error(res, 404, "not_found");
    res->writeStatus("200 OK");
    security_headers(res);
    res->writeHeader("Content-Type", cover->mime)
        ->writeHeader("Cache-Control", "private, max-age=604800, immutable")
        ->end(cover->bytes);
  });

  // Unknown API routes / methods: JSON 404 instead of the SPA.
  app_.get("/api/*", [](Res *res, auto *) { send_error(res, 404, "not_found"); });
  app_.any("/api/*", [](Res *res, auto *) { send_error(res, 404, "not_found"); });

  if (!cfg_.static_dir.empty()) {
    fs::path root = fs::absolute(cfg_.static_dir);
    app_.get("/*", [root](Res *res, uWS::HttpRequest *req) { serve_static(root, res, req->getUrl()); });
  } else {
    app_.get("/*", [](Res *res, auto *) {
      res->writeStatus("200 OK");
      security_headers(res);
      res->writeHeader("Content-Type", "text/plain")
          ->end("Dashboard backend running. Set STATIC_DIR to serve the frontend.");
    });
  }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
void WebServer::login(Res *res, Req &r, json &j, std::shared_ptr<bool> aborted) {
  auto username = string_field(j, "username");
  auto password = string_field(j, "password");
  if (!username || !password || password->size() > 256) return send_error(res, 400, "invalid_input");
  std::string name = lower_trim(*username);

  // Lock out by IP and by account (whichever is stricter).
  std::string ip_key = "ip:" + r.ip, user_key = "user:" + name;
  int wait = std::max(limiter_.locked_for(ip_key), limiter_.locked_for(user_key));
  if (wait > 0) return send_error(res, 429, "too_many_attempts", {{"retry_after", wait}});

  users_.reload_if_changed();
  const auth::User *u = users_.find(name);
  std::string hash = (u && !u->disabled) ? u->hash : "";
  std::string pw = *password;

  offload(
      res, aborted,
      [hash, pw]() -> std::string {
        if (hash.empty()) {
          auth::verify_dummy(pw); // same timing for unknown users
          return "0";
        }
        return auth::verify_password(hash, pw) ? "1" : "0";
      },
      [this, res, name, ip_key, user_key](std::string ok) {
        const auth::User *u = users_.find(name);
        if (ok != "1" || !u || u->disabled) {
          limiter_.failure(ip_key);
          limiter_.failure(user_key);
          std::cerr << "[Auth] failed login for '" << name << "'\n";
          return send_error(res, 401, "invalid_credentials");
        }
        limiter_.success(ip_key);
        limiter_.success(user_key);
        std::string token = sessions_.create(u->name);
        std::cout << "[Auth] login: " << u->name << "\n";
        send_json(res, 200, user_json(*u), session_cookie(token, cfg_.session_days, cfg_.cookie_secure));
      });
}

void WebServer::change_own_password(Res *res, Req &r, json &j, std::shared_ptr<bool> aborted) {
  const auth::User *u = current_user(r);
  if (!u) return send_error(res, 401, "unauthenticated");
  auto current = string_field(j, "current");
  auto next = string_field(j, "new");
  if (!current || !next || current->size() > 256) return send_error(res, 400, "invalid_input");
  if (auto problem = auth::password_problem(*next))
    return send_error(res, 400, "weak_password", {{"detail", *problem}});

  std::string name = u->name, hash = u->hash, cur = *current, nxt = *next;
  offload(
      res, aborted,
      [hash, cur, nxt]() -> std::string {
        if (!auth::verify_password(hash, cur)) return "";
        return auth::hash_password(nxt);
      },
      [this, res, name](std::string new_hash) {
        auth::User *u = users_.find_mut(name);
        if (!u) return send_error(res, 401, "unauthenticated");
        if (new_hash.empty()) return send_error(res, 403, "wrong_password");
        u->hash = new_hash;
        std::string err;
        if (!users_.save(err)) return send_error(res, 500, "save_failed");
        // Log out every other device, keep this one signed in.
        sessions_.revoke_user(name);
        close_sockets(name);
        std::string token = sessions_.create(name);
        std::cout << "[Auth] password changed: " << name << "\n";
        send_json(res, 204, {}, session_cookie(token, cfg_.session_days, cfg_.cookie_secure));
      });
}

void WebServer::create_user(Res *res, Req &, json &j, std::shared_ptr<bool> aborted) {
  auto username = string_field(j, "username");
  auto password = string_field(j, "password");
  if (!username || !password) return send_error(res, 400, "invalid_input");
  std::string name = lower_trim(*username);
  if (!auth::valid_username(name)) return send_error(res, 400, "invalid_username");
  if (auto problem = auth::password_problem(*password))
    return send_error(res, 400, "weak_password", {{"detail", *problem}});
  if (users_.find(name)) return send_error(res, 409, "user_exists");

  auth::User u;
  u.name = name;
  u.admin = j.value("admin", false) == true;
  u.perms = perms_field(j).value_or(auth::kDefaultPerms);
  std::string pw = *password;
  offload(
      res, aborted, [pw] { return auth::hash_password(pw); },
      [this, res, u](std::string hash) mutable {
        if (hash.empty()) return send_error(res, 500, "hash_failed");
        if (users_.find(u.name)) return send_error(res, 409, "user_exists");
        u.hash = hash;
        users_.upsert(u);
        std::string err;
        if (!users_.save(err)) return send_error(res, 500, "save_failed");
        std::cout << "[Auth] user created: " << u.name << (u.admin ? " (admin)" : "") << "\n";
        send_json(res, 201, user_json(*users_.find(u.name)));
      });
}

void WebServer::update_user(Res *res, Req &r, const std::string &name, json &j,
                            std::shared_ptr<bool> aborted) {
  const auth::User *me = current_user(r);
  const auth::User *target = users_.find(name);
  if (!target) return send_error(res, 404, "not_found");

  // Validate everything before changing anything.
  std::optional<bool> admin, disabled;
  if (j.contains("admin")) {
    if (!j["admin"].is_boolean()) return send_error(res, 400, "invalid_input");
    admin = j["admin"].get<bool>();
  }
  if (j.contains("disabled")) {
    if (!j["disabled"].is_boolean()) return send_error(res, 400, "invalid_input");
    disabled = j["disabled"].get<bool>();
  }
  auto perms = perms_field(j);
  auto password = string_field(j, "password");
  if (password)
    if (auto problem = auth::password_problem(*password))
      return send_error(res, 400, "weak_password", {{"detail", *problem}});

  bool self = me && me->name == name;
  if (self && ((admin && !*admin) || (disabled && *disabled)))
    return send_error(res, 409, "cannot_lock_out_self");

  bool want_password = password.has_value();
  auto apply = [this, res, name, admin, disabled, perms, want_password](std::string new_hash) {
    if (want_password && new_hash.empty()) return send_error(res, 500, "hash_failed");
    auth::User *u = users_.find_mut(name);
    if (!u) return send_error(res, 404, "not_found");
    bool revoke = false;
    if (admin) u->admin = *admin;
    if (disabled) {
      revoke |= *disabled && !u->disabled;
      u->disabled = *disabled;
    }
    if (perms) u->perms = *perms;
    if (!new_hash.empty()) {
      u->hash = new_hash;
      revoke = true;
    }
    if (users_.admin_count() == 0) { // never leave the dashboard without an admin
      std::string err;
      users_.load(err); // roll back from disk
      return send_error(res, 409, "last_admin");
    }
    std::string err;
    if (!users_.save(err)) return send_error(res, 500, "save_failed");
    if (revoke) sessions_.revoke_user(name);
    close_sockets(name); // reconnect picks up the new permissions
    std::cout << "[Auth] user updated: " << name << "\n";
    send_json(res, 200, user_json(*users_.find(name)));
  };

  if (password) {
    std::string pw = *password;
    offload(res, aborted, [pw] { return auth::hash_password(pw); }, apply);
  } else {
    apply("");
  }
}
