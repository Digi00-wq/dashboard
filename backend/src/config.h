#pragma once

#include <string>
#include <vector>

// All runtime configuration comes from environment variables so the same
// binary works locally, under systemd (EnvironmentFile=) and in Docker (.env).
struct Config {
  int port = 8080;
  std::string static_dir; // built frontend (frontend/dist); empty = don't serve
  int poll_interval_s = 60; // fallback; live changes come via HA WebSocket
  int server_poll_interval_s = 5;
  std::string disk_path = "/";

  // Home Assistant
  std::string ha_url;   // e.g. http://homeassistant.local:8123
  std::string ha_token; // long-lived access token

  std::string presence_entity;  // person.x / binary_sensor.x / device_tracker.x
  std::string lux_entity;       // sensor.x (illuminance, lx)
  int lux_threshold = 50;       // >= this counts as "light in the room"
  std::vector<std::string> light_entities; // light.a,light.b
  std::string weather_entity;   // weather.home
  std::string steps_entity;     // sensor.phone_daily_steps
  std::string heart_rate_entity;// sensor.phone_heart_rate
  std::string battery_entity;   // sensor.phone_battery_level
  std::vector<std::string> todo_entities; // todo.inbox,todo.work (e.g. Todoist projects)

  // "At a glance" HUD
  std::string alarm_entity;     // sensor.echo_next_alarm (timestamp)
  std::string sleep_prefix;     // sensor.iphone_x -> _sleep_duration, _deep_sleep, ...
  std::string wan_entity;       // binary_sensor.router_wan_status
  std::string download_entity;  // sensor.router_download_speed (KiB/s)
  std::string upload_entity;    // sensor.router_upload_speed (KiB/s)
  std::vector<std::string> media_entities; // media_player.a,media_player.b

  bool ha_enabled() const { return !ha_url.empty() && !ha_token.empty(); }

  static Config from_env();
};
