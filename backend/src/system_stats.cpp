// Implementation of SystemStats (see system_stats.h): CPU, RAM, disk, temperature, uptime.

#include "system_stats.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// Whole file as string ("" on error).
std::string read_file(const std::string &path) {
  std::ifstream f(path);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Look for a sensible CPU temperature: prefer known CPU drivers in hwmon,
// fall back to the first thermal zone.
bool read_cpu_temp(float &out) {
  std::error_code ec;
  const char *preferred[] = {"coretemp", "k10temp", "zenpower", "cpu_thermal",
                             "acpitz"};
  if (fs::exists("/sys/class/hwmon", ec)) {
    for (const char *want : preferred) {
      for (const auto &dir : fs::directory_iterator("/sys/class/hwmon", ec)) {
        std::string name = read_file(dir.path() / "name");
        while (!name.empty() && (name.back() == '\n')) name.pop_back();
        if (name != want) continue;
        std::string t = read_file(dir.path() / "temp1_input");
        if (!t.empty()) {
          out = std::stof(t) / 1000.0f;
          return true;
        }
      }
    }
  }
  std::string t = read_file("/sys/class/thermal/thermal_zone0/temp");
  if (!t.empty()) {
    try {
      out = std::stof(t) / 1000.0f;
      return true;
    } catch (...) {
    }
  }
  return false;
}

} // namespace

SystemStats::SystemStats(std::string disk_path)
    : disk_path_(std::move(disk_path)) {}

void SystemStats::fill(dashboard_backend::Server *out) {
  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) == 0) out->set_hostname(host);

  // Uptime
  {
    std::istringstream in(read_file("/proc/uptime"));
    double up = 0;
    if (in >> up) out->set_uptime_seconds(static_cast<uint64_t>(up));
  }

  // Load average
  {
    std::istringstream in(read_file("/proc/loadavg"));
    float l1 = 0;
    if (in >> l1) out->set_load1(l1);
  }

  // CPU usage (delta since last call)
  {
    std::istringstream in(read_file("/proc/stat"));
    std::string cpu;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
             softirq = 0, steal = 0;
    if (in >> cpu >> user >> nice >> system >> idle >> iowait >> irq >>
        softirq >> steal) {
      uint64_t idle_all = idle + iowait;
      uint64_t total =
          user + nice + system + idle + iowait + irq + softirq + steal;
      if (prev_total_ != 0 && total > prev_total_) {
        double dt = double(total - prev_total_);
        double di = double(idle_all - prev_idle_);
        out->set_cpu_percent(float(100.0 * (1.0 - di / dt)));
      }
      prev_total_ = total;
      prev_idle_ = idle_all;
    }
  }

  // Memory
  {
    std::istringstream in(read_file("/proc/meminfo"));
    std::string key, unit;
    uint64_t value = 0, total = 0, available = 0;
    while (in >> key >> value >> unit) {
      if (key == "MemTotal:") total = value * 1024;
      else if (key == "MemAvailable:") available = value * 1024;
    }
    out->set_mem_total(total);
    out->set_mem_used(total > available ? total - available : 0);
  }

  // Disk
  {
    struct statvfs s{};
    if (statvfs(disk_path_.c_str(), &s) == 0) {
      uint64_t total = uint64_t(s.f_blocks) * s.f_frsize;
      uint64_t free_ = uint64_t(s.f_bfree) * s.f_frsize;
      out->set_disk_total(total);
      out->set_disk_used(total - free_);
    }
  }

  float temp = 0;
  if (read_cpu_temp(temp)) out->set_temperature(temp);
}
