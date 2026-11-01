#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace {

std::string env(const char *name, const std::string &fallback = "") {
  const char *v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

int env_int(const char *name, int fallback) {
  const char *v = std::getenv(name);
  if (!v || !*v) return fallback;
  try {
    return std::stoi(v);
  } catch (...) {
    return fallback;
  }
}

std::vector<std::string> split_list(const std::string &s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    auto b = item.find_first_not_of(" \t");
    auto e = item.find_last_not_of(" \t");
    if (b != std::string::npos) out.push_back(item.substr(b, e - b + 1));
  }
  return out;
}

} // namespace

Config Config::from_env() {
  Config c;
  c.port = env_int("PORT", c.port);
  c.static_dir = env("STATIC_DIR", "");
  c.poll_interval_s = std::max(2, env_int("POLL_INTERVAL", c.poll_interval_s));
  c.server_poll_interval_s =
      std::max(1, env_int("SERVER_POLL_INTERVAL", c.server_poll_interval_s));
  c.disk_path = env("DISK_PATH", c.disk_path);

  c.ha_url = env("HA_URL");
  while (!c.ha_url.empty() && c.ha_url.back() == '/') c.ha_url.pop_back();
  c.ha_token = env("HA_TOKEN");

  c.presence_entity = env("HA_PRESENCE_ENTITY");
  c.lux_entity = env("HA_LUX_ENTITY");
  c.lux_threshold = env_int("HA_LUX_THRESHOLD", c.lux_threshold);
  c.light_entities = split_list(env("HA_LIGHT_ENTITIES"));
  c.weather_entity = env("HA_WEATHER_ENTITY");
  c.steps_entity = env("HA_STEPS_ENTITY");
  c.heart_rate_entity = env("HA_HEART_RATE_ENTITY");
  c.battery_entity = env("HA_BATTERY_ENTITY");
  c.todo_entities = split_list(env("HA_TODO_ENTITIES"));
  for (auto &e : split_list(env("HA_TODO_ENTITY"))) // older single-list name
    c.todo_entities.push_back(e);
  c.alarm_entity = env("HA_ALARM_ENTITY");
  c.sleep_prefix = env("HA_SLEEP_PREFIX");
  c.wan_entity = env("HA_WAN_ENTITY");
  c.download_entity = env("HA_DOWNLOAD_ENTITY");
  c.upload_entity = env("HA_UPLOAD_ENTITY");
  c.media_entities = split_list(env("HA_MEDIA_ENTITIES"));
  return c;
}
