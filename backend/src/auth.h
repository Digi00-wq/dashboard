#pragma once

// Accounts, password hashing, sessions and login rate limiting.
//
// Security model:
//  * Passwords are hashed with Argon2id (libsodium crypto_pwhash_str), never stored.
//  * Session tokens are 256-bit random values; the server stores only their
//    SHA-256, so a leaked sessions.json can't be replayed.
//  * Everything is persisted as JSON files (0600) in DATA_DIR — no SQL, so no
//    SQL injection surface.
//  * UserStore / Sessions / LoginLimiter are NOT thread safe: use them from
//    the event-loop thread only. Hashing runs on helper threads with copies.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace auth {

// ---------------------------------------------------------------------------
// Permissions (what a user may see / control)
// ---------------------------------------------------------------------------
enum Perm : uint32_t {
  WEATHER = 1u << 0,
  LIGHTS = 1u << 1, // see + switch lights
  ROOM = 1u << 2,   // presence + light sensor
  TASKS = 1u << 3,  // see + complete to-dos
  MEDIA = 1u << 4,  // Spotify: see + control
  HEALTH = 1u << 5, // phone, sleep, alarm
  SERVER = 1u << 6, // server stats, network
  ALL_FEATURES = (1u << 7) - 1,
  ADMIN_VIEW = 1u << 31, // data-source errors etc. (admins only)
};

const std::vector<std::pair<std::string, uint32_t>> &perm_names();
uint32_t perms_from_names(const std::vector<std::string> &names);
std::vector<std::string> perm_to_names(uint32_t mask);
constexpr uint32_t kDefaultPerms = WEATHER | LIGHTS;

// ---------------------------------------------------------------------------
// Passwords
// ---------------------------------------------------------------------------
bool init(); // sodium_init; call once at startup
std::string hash_password(const std::string &password); // empty on failure
bool verify_password(const std::string &hash, const std::string &password);
// Burns the same time as a real verify (prevents username enumeration).
void verify_dummy(const std::string &password);

bool valid_username(const std::string &name); // [a-z0-9][a-z0-9_.-]{0,31}
std::optional<std::string> password_problem(const std::string &password);

std::string random_token();           // 64 hex chars (256 bit)
std::string sha256_hex(const std::string &s);
bool constant_time_equal(const std::string &a, const std::string &b);

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------
struct User {
  std::string name;
  std::string hash;
  bool admin = false;
  uint32_t perms = kDefaultPerms;
  bool disabled = false;
  int64_t created = 0; // Unix ms

  // Effective mask: admins see everything incl. error details.
  uint32_t mask() const { return admin ? (ALL_FEATURES | ADMIN_VIEW) : perms; }
};

class UserStore {
public:
  explicit UserStore(std::filesystem::path file);

  bool load(std::string &error);
  bool save(std::string &error);
  void reload_if_changed(); // picks up changes made by the CLI

  const User *find(const std::string &name) const;
  User *find_mut(const std::string &name);
  const std::vector<User> &all() const { return users_; }
  bool empty() const { return users_.empty(); }
  int admin_count() const;

  void upsert(User u);
  bool remove(const std::string &name);

private:
  std::filesystem::path file_;
  std::filesystem::file_time_type mtime_{};
  std::vector<User> users_;
};

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
class Sessions {
public:
  Sessions(std::filesystem::path file, std::chrono::hours lifetime);

  void load();
  // Returns the raw token for the cookie (only its hash is stored).
  std::string create(const std::string &user);
  // User name for a raw token, if valid and not expired.
  std::optional<std::string> lookup(const std::string &token);
  void revoke(const std::string &token);
  void revoke_user(const std::string &user);

private:
  struct Entry {
    std::string user;
    int64_t expires = 0; // Unix ms
  };
  void prune();
  void save();

  std::filesystem::path file_;
  std::chrono::hours lifetime_;
  std::map<std::string, Entry> by_hash_;
};

// ---------------------------------------------------------------------------
// Brute-force protection
// ---------------------------------------------------------------------------
class LoginLimiter {
public:
  // Seconds the key is still locked (0 = allowed).
  int locked_for(const std::string &key);
  void failure(const std::string &key);
  void success(const std::string &key);

private:
  struct State {
    int failures = 0;
    std::chrono::steady_clock::time_point last{};
    std::chrono::steady_clock::time_point locked_until{};
  };
  void prune();
  std::map<std::string, State> state_;
};

// Atomically writes `content` to `file` with mode 0600.
bool write_private_file(const std::filesystem::path &file,
                        const std::string &content, std::string &error);

} // namespace auth
