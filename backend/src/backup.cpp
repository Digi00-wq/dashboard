// Implementation of daily backups (see backup.h).
// Each day: BACKUP_DIR/YYYY-MM-DD/{collect.db.gz, users.json, prefs.json}.
// Restore the database with: gunzip -k collect.db.gz

#include "backup.h"

#include <sqlite3.h>
#include <sys/stat.h>
#include <zlib.h>

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::mutex g_mutex; // backup thread vs. nothing else in-process; keeps runs serial

// BACKUP_DIR or DATA_DIR/backups.
fs::path backup_root(const Config &cfg) {
  return cfg.backup_dir.empty() ? fs::path(cfg.data_dir) / "backups" : fs::path(cfg.backup_dir);
}

// Local date YYYY-MM-DD (= backup folder name).
std::string today_str() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

// Only folders named exactly YYYY-MM-DD are ours to rotate.
bool is_date_name(const std::string &s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

constexpr auto kDirPerms = fs::perms::owner_all;
constexpr auto kFilePerms = fs::perms::owner_read | fs::perms::owner_write;

// Online backup: consistent copy while the server keeps writing (WAL mode
// readers don't block the writer).
bool copy_db(const fs::path &src, const fs::path &dst, std::string &error) {
  sqlite3 *in = nullptr, *out = nullptr;
  auto close_all = [&] {
    if (in) sqlite3_close(in);
    if (out) sqlite3_close(out);
  };
  if (sqlite3_open_v2(src.c_str(), &in, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    error = "cannot open " + src.string() + ": " + (in ? sqlite3_errmsg(in) : "?");
    close_all();
    return false;
  }
  sqlite3_busy_timeout(in, 5000);
  mode_t old = umask(0077);
  int rc = sqlite3_open_v2(dst.c_str(), &out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  umask(old);
  if (rc != SQLITE_OK) {
    error = "cannot create " + dst.string() + ": " + (out ? sqlite3_errmsg(out) : "?");
    close_all();
    return false;
  }
  sqlite3_backup *b = sqlite3_backup_init(out, "main", in, "main");
  if (!b) {
    error = std::string("backup init: ") + sqlite3_errmsg(out);
    close_all();
    return false;
  }
  for (int tries = 0;;) {
    rc = sqlite3_backup_step(b, -1);
    if (rc == SQLITE_DONE) break;
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && ++tries < 50) {
      std::this_thread::sleep_for(200ms);
      continue;
    }
    break;
  }
  sqlite3_backup_finish(b);
  if (rc != SQLITE_DONE) {
    error = std::string("backup step: ") + sqlite3_errstr(rc);
    close_all();
    return false;
  }
  // One self-contained file (no -wal next to it), and make sure it's sound.
  sqlite3_exec(out, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
  sqlite3_stmt *q = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(out, "PRAGMA quick_check", -1, &q, nullptr) == SQLITE_OK &&
      sqlite3_step(q) == SQLITE_ROW) {
    auto *t = sqlite3_column_text(q, 0);
    ok = t && std::string(reinterpret_cast<const char *>(t)) == "ok";
  }
  sqlite3_finalize(q);
  close_all();
  if (!ok) error = "quick_check of the copy failed";
  return ok;
}

// gzip-compresses src into dst (mode 0600), written as dst.part and renamed
// when complete. The database compresses to about a quarter of its size.
bool gzip_file(const fs::path &src, const fs::path &dst, std::string &error) {
  std::FILE *in = std::fopen(src.c_str(), "rb");
  if (!in) {
    error = "cannot read " + src.string();
    return false;
  }
  const fs::path part = dst.string() + ".part";
  mode_t old = umask(0077);
  gzFile out = gzopen(part.c_str(), "wb6");
  umask(old);
  if (!out) {
    std::fclose(in);
    error = "cannot create " + part.string();
    return false;
  }
  gzbuffer(out, 256 * 1024);
  std::vector<char> buf(256 * 1024);
  bool ok = true;
  size_t n;
  while (ok && (n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
    ok = gzwrite(out, buf.data(), unsigned(n)) == int(n);
  ok = ok && !std::ferror(in);
  std::fclose(in);
  ok = (gzclose(out) == Z_OK) && ok; // flushes; fails e.g. on a full disk
  std::error_code ec;
  if (ok) fs::rename(part, dst, ec);
  if (!ok || ec) {
    fs::remove(part, ec);
    error = "compressing " + src.filename().string() + " failed (disk full?)";
    return false;
  }
  return true;
}

// Older backups made before compression existed: compress their collect.db
// in place (the backup is kept, just smaller).
void compress_old(const fs::path &root) {
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(root, ec)) {
    const std::string name = e.path().filename().string();
    if (!is_date_name(name) || !e.is_directory(ec) || e.is_symlink(ec)) continue;
    const fs::path db = e.path() / "collect.db", gz = e.path() / "collect.db.gz";
    if (!fs::exists(db, ec) || fs::exists(gz, ec)) continue;
    std::string err;
    if (gzip_file(db, gz, err)) {
      fs::remove(db, ec);
      std::cout << "[Backup] compressed old backup " << name << "\n";
    } else {
      std::cerr << "[Backup] could not compress " << name << ": " << err << "\n";
      return; // probably out of space; try again next time
    }
  }
}

// Copies a file with mode 0600.
bool copy_file(const fs::path &src, const fs::path &dst, std::string &error) {
  std::error_code ec;
  if (!fs::exists(src, ec)) return true; // nothing to back up (yet)
  if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
    error = "copy " + src.filename().string() + ": " + ec.message();
    return false;
  }
  fs::permissions(dst, kFilePerms, ec);
  return true;
}

// Deletes the oldest YYYY-MM-DD folders beyond `keep`.
void rotate(const fs::path &root, int keep) {
  std::error_code ec;
  std::vector<std::string> days;
  for (const auto &e : fs::directory_iterator(root, ec)) {
    std::string name = e.path().filename().string();
    if (is_date_name(name) && e.is_directory(ec) && !e.is_symlink(ec)) days.push_back(name);
  }
  std::sort(days.begin(), days.end()); // ISO dates sort chronologically
  for (size_t i = 0; i + size_t(std::max(keep, 1)) < days.size(); ++i) {
    fs::remove_all(root / days[i], ec);
    std::cout << "[Backup] removed old backup " << days[i]
              << (ec ? " (failed: " + ec.message() + ")" : "") << "\n";
  }
}

} // namespace

// One backup into BACKUP_DIR/<today>/ and pruning of old folders.
bool backup_now(const Config &cfg, std::string &error) {
  std::lock_guard lock(g_mutex);
  const fs::path root = backup_root(cfg), data = cfg.data_dir;
  const std::string day = today_str();
  const fs::path tmp = root / (".tmp-" + day), dest = root / day;
  std::error_code ec;

  fs::create_directories(root, ec);
  if (ec) {
    error = "cannot create " + root.string() + ": " + ec.message();
    return false;
  }
  fs::permissions(root, kDirPerms, ec); // may fail on some mounts; not fatal
  fs::remove_all(tmp, ec);
  fs::create_directory(tmp, ec);
  if (ec) {
    error = "cannot create " + tmp.string() + ": " + ec.message();
    return false;
  }
  fs::permissions(tmp, kDirPerms, ec);

  // Build in a hidden folder, then swap in: a half-written backup never
  // looks like a valid day.
  bool ok = true;
  if (fs::exists(data / "collect.db", ec)) {
    ok = copy_db(data / "collect.db", tmp / "collect.db", error) &&
         gzip_file(tmp / "collect.db", tmp / "collect.db.gz", error);
    fs::remove(tmp / "collect.db", ec); // only the compressed copy is kept
  }
  ok = ok && copy_file(data / "users.json", tmp / "users.json", error) &&
       copy_file(data / "prefs.json", tmp / "prefs.json", error);
  if (ok) {
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) {
      error = "cannot rename to " + dest.string() + ": " + ec.message();
      ok = false;
    }
  }
  if (!ok) {
    fs::remove_all(tmp, ec);
    return false;
  }
  std::cout << "[Backup] wrote " << dest.string() << "\n";
  rotate(root, cfg.backup_keep);
  compress_old(root);
  return true;
}

// Thread: backup at startup if none today, then daily at BACKUP_HOUR.
void start_backup_thread(const Config &cfg) {
  std::thread([cfg] {
    auto run = [&] {
      std::string err;
      if (!backup_now(cfg, err)) std::cerr << "[Backup] failed: " << err << "\n";
    };
    // Shortly after startup, if today's backup is missing.
    std::this_thread::sleep_for(60s);
    std::error_code ec;
    if (!fs::exists(backup_root(cfg) / today_str(), ec)) run();

    for (;;) {
      // Next BACKUP_HOUR:00 local time (today if still ahead, else tomorrow).
      std::time_t now = std::time(nullptr);
      std::tm tm{};
      localtime_r(&now, &tm);
      tm.tm_hour = cfg.backup_hour;
      tm.tm_min = tm.tm_sec = 0;
      tm.tm_isdst = -1;
      std::time_t next = std::mktime(&tm);
      if (next <= now) {
        tm.tm_mday += 1;
        tm.tm_hour = cfg.backup_hour;
        tm.tm_isdst = -1;
        next = std::mktime(&tm);
      }
      // Sleep in short steps: robust against clock changes and suspend.
      while (std::time(nullptr) < next)
        std::this_thread::sleep_for(std::min<std::chrono::seconds>(
            60s, std::chrono::seconds(next - std::time(nullptr))));
      run();
    }
  }).detach();
}
