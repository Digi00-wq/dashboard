// Reads Config from environment variables (see config.h and .env.example).

#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace {

// Env var or fallback.
std::string env(const char *name, const std::string &fallback = "") {
  const char *v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

// Env var as int or fallback.
int env_int(const char *name, int fallback) {
  const char *v = std::getenv(name);
  if (!v || !*v) return fallback;
  try {
    return std::stoi(v);
  } catch (...) {
    return fallback;
  }
}

// "a, b,c" -> {a, b, c}.
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

// All settings. Add new env vars here + config.h + .env.example.
Config Config::from_env() {
  Config c;
  c.port = env_int("PORT", c.port);
  c.bind_address = env("BIND_ADDRESS", c.bind_address);
  c.static_dir = env("STATIC_DIR", "");
  c.poll_interval_s = std::max(2, env_int("POLL_INTERVAL", c.poll_interval_s));
  c.server_poll_interval_s =
      std::max(1, env_int("SERVER_POLL_INTERVAL", c.server_poll_interval_s));
  c.disk_path = env("DISK_PATH", c.disk_path);
  c.data_dir = env("DATA_DIR", c.data_dir);
  c.cookie_secure = env("COOKIE_SECURE") == "1";
  c.trust_proxy = env("TRUST_PROXY") == "1";
  c.session_days = std::clamp(env_int("SESSION_DAYS", c.session_days), 1, 365);

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
  c.alexa_entity = env("HA_ALEXA_ENTITY");
  c.alexa_notify = env("HA_ALEXA_NOTIFY");
  if (c.alexa_notify.rfind("notify.", 0) == 0) c.alexa_notify.erase(0, 7);
  c.voice_name = env("VOICE_NAME", c.voice_name);
  c.voice_user = env("VOICE_USER");
  c.briefing_delay_min = env_int("BRIEFING_DELAY_MIN", c.briefing_delay_min);
  c.wake_light_min = std::clamp(env_int("WAKE_LIGHT_MIN", c.wake_light_min), 0, 60);
  c.collect_dashboard_user = env("COLLECT_DASHBOARD_USER");
  c.collect_to_ha = env("HA_COLLECT_PUBLISH", "1") != "0";
  c.notify_target = env("HA_NOTIFY_TARGET");
  if (c.notify_target.rfind("notify.", 0) == 0) c.notify_target.erase(0, 7);
  c.owner_user = env("OWNER_USER");
  c.focus_entity = env("HA_FOCUS_ENTITY");
  c.backup_dir = env("BACKUP_DIR");
  c.backup_keep = std::clamp(env_int("BACKUP_KEEP", c.backup_keep), 1, 365);
  c.backup_hour = std::clamp(env_int("BACKUP_HOUR", c.backup_hour), 0, 23);
  c.claude_bridge_url = env("CLAUDE_BRIDGE_URL");
  while (!c.claude_bridge_url.empty() && c.claude_bridge_url.back() == '/')
    c.claude_bridge_url.pop_back();
  c.claude_bridge_token = env("CLAUDE_BRIDGE_TOKEN");
  return c;
}
