#pragma once

#include "dashboard.pb.h"

#include <cstdint>
#include <string>

// Reads host metrics from /proc and /sys. Keeps the previous CPU sample so
// cpu_percent is the usage since the last call.
class SystemStats {
public:
  explicit SystemStats(std::string disk_path);
  void fill(dashboard_backend::Server *out);

private:
  std::string disk_path_;
  uint64_t prev_idle_ = 0;
  uint64_t prev_total_ = 0;
};
