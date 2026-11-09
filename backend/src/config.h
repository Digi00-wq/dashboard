#pragma once

#include <string>
#include <vector>

// All runtime configuration comes from environment variables so the same
// binary works locally, under systemd (EnvironmentFile=) and in Docker (.env).
struct Config {
  int port = 8080;
  std::string bind_address = "0.0.0.0"; // 127.0.0.1 = only via local reverse proxy/tunnel
  std::string static_dir; // built frontend (frontend/dist); empty = don't serve
  int poll_interval_s = 60; // fallback; live changes come via HA WebSocket
  int server_poll_interval_s = 5;
  std::string disk_path = "/";

  // Accounts & sessions
  std::string data_dir = "data"; // users.json, sessions.json
  bool cookie_secure = false;    // set COOKIE_SECURE=1 behind HTTPS
  bool trust_proxy = false;      // use X-Forwarded-For (behind a reverse proxy)
  int session_days = 30;

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

  // Voice (Alexa via Alexa Media Player): commands + spoken answers
  std::string alexa_entity;  // media_player.echo_dot  (reads last_called_summary)
  std::string alexa_notify;  // notify service, e.g. alexa_media_echo_dot
  std::string voice_name = "Hans"; // Amazon Polly voice for answers
  std::string voice_user;    // name used in greetings
  int briefing_delay_min = 2; // morning briefing N minutes after the alarm (<0 = off)
  int wake_light_min = 0;     // fade lights up N minutes before the alarm (0 = off)
  // HomeCollect (iOS app)
  std::string collect_dashboard_user; // whose data the health card shows (empty = newest upload)
  bool collect_to_ha = true;          // publish summaries as sensor.homecollect_<user>_*

  // Automations / notifications
  std::string notify_target;  // HA notify service of your phone, e.g. mobile_app_iphone_x
  std::string owner_user;     // whose prefs/data drive the home automations (empty = first admin)
  std::string focus_entity;   // binary_sensor.iphone_x_focus (Sleep focus ends -> morning push)
  // Arrive/leave extras (automations.cpp "presence extras")
  std::string homework_calendar; // calendar.notion: homework + tests as events on the due day
  std::vector<int> school_days;  // weekdays (0 = Sun .. 6 = Sat) from SCHOOL_DAYS=mon,tue
  std::string shopping_entity = "todo.shopping_list"; // pushed when leaving work

  // Backups
  std::string backup_dir;     // empty = DATA_DIR/backups
  int backup_keep = 7;        // days
  int backup_hour = 3;        // local hour of the daily backup

  // "Claude" tab of the iOS app (admins only): host-side bridge that runs
  // Claude Code on the server (scripts/claude-bridge.py). Empty URL = off.
  std::string claude_bridge_url;   // e.g. http://127.0.0.1:8091
  std::string claude_bridge_token; // shared secret, same value as the bridge's
  bool claude_enabled() const { return !claude_bridge_url.empty() && !claude_bridge_token.empty(); }

  bool voice_enabled() const { return !alexa_entity.empty() && !alexa_notify.empty(); }

  bool ha_enabled() const { return !ha_url.empty() && !ha_token.empty(); }

  static Config from_env();
};
