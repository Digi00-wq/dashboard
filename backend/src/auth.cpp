#include "auth.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <cctype>
#include <regex>
#include <sstream>
#include <sys/stat.h>

using nlohmann::json;
namespace fs = std::filesystem;

namespace auth {

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string to_hex(const unsigned char *data, size_t len) {
  std::string out(len * 2, '\0');
  sodium_bin2hex(out.data(), out.size() + 1, data, len);
  return out;
}

// Argon2id cost: ~64 MiB memory, a few iterations ("interactive" profile).
constexpr unsigned long long kOps = crypto_pwhash_OPSLIMIT_INTERACTIVE;
constexpr size_t kMem = crypto_pwhash_MEMLIMIT_INTERACTIVE;

std::string g_dummy_hash;

} // namespace

// ---------------------------------------------------------------------------
// Permissions
// ---------------------------------------------------------------------------
const std::vector<std::pair<std::string, uint32_t>> &perm_names() {
  static const std::vector<std::pair<std::string, uint32_t>> names = {
      {"weather", WEATHER}, {"lights", LIGHTS}, {"room", ROOM},
      {"tasks", TASKS},     {"media", MEDIA},   {"health", HEALTH},
      {"server", SERVER},   {"announce", ANNOUNCE}, {"collect", COLLECT},
  };
  return names;
}

uint32_t perms_from_names(const std::vector<std::string> &names) {
  uint32_t mask = 0;
  for (const auto &n : names)
    for (const auto &[name, bit] : perm_names())
      if (n == name) mask |= bit;
  return mask;
}

std::vector<std::string> perm_to_names(uint32_t mask) {
  std::vector<std::string> out;
  for (const auto &[name, bit] : perm_names())
    if (mask & bit) out.push_back(name);
  return out;
}

// ---------------------------------------------------------------------------
// Passwords & tokens
// ---------------------------------------------------------------------------
bool init() {
  if (sodium_init() < 0) return false;
  // Hash of a random secret: used to burn equal time for unknown users.
  g_dummy_hash = hash_password(random_token());
  return !g_dummy_hash.empty();
}

std::string hash_password(const std::string &password) {
  char out[crypto_pwhash_STRBYTES];
  if (crypto_pwhash_str(out, password.data(), password.size(), kOps, kMem) != 0)
    return {}; // out of memory
  return out;
}

bool verify_password(const std::string &hash, const std::string &password) {
  if (hash.empty() || hash.size() >= crypto_pwhash_STRBYTES) return false;
  return crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) == 0;
}

void verify_dummy(const std::string &password) {
  (void)verify_password(g_dummy_hash, password);
}

bool valid_username(const std::string &name) {
  static const std::regex re("^[a-z0-9][a-z0-9_.-]{0,31}$");
  return std::regex_match(name, re);
}

std::optional<std::string> clean_display_name(const std::string &in) {
  auto b = in.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::nullopt;
  auto e = in.find_last_not_of(" \t\r\n");
  std::string s = in.substr(b, e - b + 1);
  size_t chars = 0;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == '&' || c == '"') return std::nullopt;
    if ((c & 0xC0) != 0x80) ++chars; // count UTF-8 code points, not bytes
  }
  if (chars == 0 || chars > 40 || s.size() > 160) return std::nullopt;
  return s;
}

std::string User::spoken_name() const {
  if (!display_name.empty()) return display_name;
  std::string s = name;
  if (!s.empty()) s[0] = char(std::toupper((unsigned char)s[0]));
  return s;
}

Owner resolve_owner(const std::filesystem::path &users_file, const std::string &preferred) {
  UserStore users(users_file);
  std::string err;
  Owner o;
  if (!users.load(err)) {
    o.user = preferred;
    return o;
  }
  const User *u = preferred.empty() ? nullptr : users.find(preferred);
  if (!u && preferred.empty())
    for (const auto &x : users.all())
      if (x.admin && !x.disabled) {
        u = &x;
        break;
      }
  if (u) {
    o.user = u->name;
    o.spoken = u->spoken_name();
  } else {
    o.user = preferred;
    User tmp;
    tmp.name = preferred;
    o.spoken = tmp.spoken_name();
  }
  return o;
}

std::optional<std::string> password_problem(const std::string &pw) {
  if (pw.size() < 8) return "password must be at least 8 characters";
  if (pw.size() > 128) return "password must be at most 128 characters";
  return std::nullopt;
}

std::string random_token() {
  unsigned char buf[32];
  randombytes_buf(buf, sizeof(buf));
  return to_hex(buf, sizeof(buf));
}

std::string sha256_hex(const std::string &s) {
  unsigned char out[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(out, reinterpret_cast<const unsigned char *>(s.data()), s.size());
  return to_hex(out, sizeof(out));
}

bool constant_time_equal(const std::string &a, const std::string &b) {
  return a.size() == b.size() && sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool write_private_file(const fs::path &file, const std::string &content,
                        std::string &error) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += ".tmp";
  {
    mode_t old = umask(0077); // file is created 0600
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    umask(old);
    if (!f) {
      error = "cannot write " + tmp.string();
      return false;
    }
    f << content;
    f.flush();
    if (!f) {
      error = "write failed: " + tmp.string();
      return false;
    }
  }
  fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, ec);
  fs::rename(tmp, file, ec); // atomic replace
  if (ec) {
    error = "rename failed: " + ec.message();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// UserStore
// ---------------------------------------------------------------------------
UserStore::UserStore(fs::path file) : file_(std::move(file)) {}

bool UserStore::load(std::string &error) {
  users_.clear();
  std::error_code ec;
  if (!fs::exists(file_, ec)) return true; // no users yet
  std::ifstream f(file_);
  try {
    json j = json::parse(f);
    for (const auto &u : j.at("users")) {
      User user;
      user.name = u.at("name").get<std::string>();
      if (auto dn = clean_display_name(u.value("displayName", ""))) user.display_name = *dn;
      user.hash = u.at("hash").get<std::string>();
      user.admin = u.value("admin", false);
      user.perms = perms_from_names(u.value("permissions", std::vector<std::string>{}));
      user.disabled = u.value("disabled", false);
      user.created = u.value("created", int64_t{0});
      if (valid_username(user.name)) users_.push_back(std::move(user));
    }
  } catch (const std::exception &e) {
    error = "cannot parse " + file_.string() + ": " + e.what();
    return false;
  }
  mtime_ = fs::last_write_time(file_, ec);
  return true;
}

bool UserStore::save(std::string &error) {
  json arr = json::array();
  for (const auto &u : users_)
    arr.push_back({{"name", u.name},
                   {"displayName", u.display_name},
                   {"hash", u.hash},
                   {"admin", u.admin},
                   {"permissions", perm_to_names(u.perms)},
                   {"disabled", u.disabled},
                   {"created", u.created}});
  if (!write_private_file(file_, json{{"users", arr}}.dump(2), error)) return false;
  std::error_code ec;
  mtime_ = fs::last_write_time(file_, ec);
  return true;
}

void UserStore::reload_if_changed() {
  std::error_code ec;
  auto t = fs::last_write_time(file_, ec);
  if (ec || t == mtime_) return;
  std::string err;
  if (!load(err)) fprintf(stderr, "[Auth] %s\n", err.c_str());
}

const User *UserStore::find(const std::string &name) const {
  for (const auto &u : users_)
    if (u.name == name) return &u;
  return nullptr;
}

User *UserStore::find_mut(const std::string &name) {
  for (auto &u : users_)
    if (u.name == name) return &u;
  return nullptr;
}

int UserStore::admin_count() const {
  return int(std::count_if(users_.begin(), users_.end(),
                           [](const User &u) { return u.admin && !u.disabled; }));
}

void UserStore::upsert(User u) {
  if (u.created == 0) u.created = now_ms();
  if (auto *existing = find_mut(u.name)) *existing = std::move(u);
  else users_.push_back(std::move(u));
}

bool UserStore::remove(const std::string &name) {
  auto it = std::remove_if(users_.begin(), users_.end(),
                           [&](const User &u) { return u.name == name; });
  if (it == users_.end()) return false;
  users_.erase(it, users_.end());
  return true;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
Sessions::Sessions(fs::path file, std::chrono::hours lifetime)
    : file_(std::move(file)), lifetime_(lifetime) {}

void Sessions::load() {
  by_hash_.clear();
  std::ifstream f(file_);
  if (!f) return;
  try {
    json j = json::parse(f);
    for (const auto &s : j.at("sessions"))
      by_hash_[s.at("hash").get<std::string>()] = {s.at("user").get<std::string>(),
                                                    s.at("expires").get<int64_t>()};
  } catch (...) {
    by_hash_.clear();
  }
  prune();
}

void Sessions::save() {
  json arr = json::array();
  for (const auto &[hash, e] : by_hash_)
    arr.push_back({{"hash", hash}, {"user", e.user}, {"expires", e.expires}});
  std::string err;
  if (!write_private_file(file_, json{{"sessions", arr}}.dump(), err))
    fprintf(stderr, "[Auth] %s\n", err.c_str());
}

void Sessions::prune() {
  int64_t now = now_ms();
  std::erase_if(by_hash_, [&](const auto &kv) { return kv.second.expires < now; });
}

std::string Sessions::create(const std::string &user) {
  prune();
  std::string token = random_token();
  by_hash_[sha256_hex(token)] = {
      user, now_ms() + std::chrono::duration_cast<std::chrono::milliseconds>(lifetime_).count()};
  save();
  return token;
}

std::optional<std::string> Sessions::lookup(const std::string &token) {
  if (token.size() != 64) return std::nullopt;
  auto it = by_hash_.find(sha256_hex(token));
  if (it == by_hash_.end()) return std::nullopt;
  if (it->second.expires < now_ms()) {
    by_hash_.erase(it);
    save();
    return std::nullopt;
  }
  return it->second.user;
}

void Sessions::revoke(const std::string &token) {
  if (by_hash_.erase(sha256_hex(token))) save();
}

void Sessions::revoke_user(const std::string &user) {
  if (std::erase_if(by_hash_, [&](const auto &kv) { return kv.second.user == user; }))
    save();
}

// ---------------------------------------------------------------------------
// LoginLimiter: 5 free attempts per 15 min, then 1, 2, 4 ... max 60 min lockout
// ---------------------------------------------------------------------------
int LoginLimiter::locked_for(const std::string &key) {
  prune();
  auto it = state_.find(key);
  if (it == state_.end()) return 0;
  auto now = std::chrono::steady_clock::now();
  if (it->second.locked_until <= now) return 0;
  return int(std::chrono::duration_cast<std::chrono::seconds>(it->second.locked_until - now)
                 .count()) + 1;
}

void LoginLimiter::failure(const std::string &key) {
  auto now = std::chrono::steady_clock::now();
  auto &s = state_[key];
  if (now - s.last > std::chrono::minutes(15)) s.failures = 0;
  s.failures++;
  s.last = now;
  if (s.failures >= 5) {
    int exp = std::min(s.failures - 5, 6); // 1,2,4,...,64 -> capped below
    auto lock = std::min(std::chrono::minutes(1 << exp), std::chrono::minutes(60));
    s.locked_until = now + lock;
  }
}

void LoginLimiter::success(const std::string &key) { state_.erase(key); }

void LoginLimiter::prune() {
  auto now = std::chrono::steady_clock::now();
  std::erase_if(state_, [&](const auto &kv) {
    return kv.second.locked_until < now && now - kv.second.last > std::chrono::hours(1);
  });
}

} // namespace auth
