#pragma once

#include "config.h"
#include "cover_cache.h"
#include "dashboard.pb.h"
#include "http_client.h"

#include <array>
#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Talks to the Home Assistant REST API (https://developers.home-assistant.io/docs/api/rest/).
// Not thread safe: use from the worker thread only.
class HomeAssistant {
public:
  HomeAssistant(const Config &cfg, CoverCache &covers);

  // Fills every configured section of `out`. Problems are appended to errors.
  void fill(dashboard_backend::Dashboard &out, std::vector<std::string> &errors);

  bool light_command(const dashboard_backend::LightCommand &cmd,
                     std::string &error);
  bool all_lights(bool on, std::string &error);

  // --- voice ---
  struct Utterance {
    std::string text;    // what was said to Alexa (without the wake word)
    int64_t timestamp;   // Unix ms
  };
  std::optional<Utterance> last_called(std::string &error);
  // Speaks `text` on the Echo with the configured Polly voice.
  bool speak(const std::string &text, std::string &error);
  // All configured lights: on (optionally brightness %, fade seconds) / off.
  bool set_lights(bool on, std::optional<int> brightness_pct, int transition_s,
                  std::string &error);
  bool pause_media(std::string &error);
  // Only these lights / players (must be configured ones).
  bool set_lights(const std::vector<std::string> &entities, bool on,
                  std::optional<int> brightness_pct, int transition_s, std::string &error);
  bool pause_media(const std::vector<std::string> &entities, std::string &error);

  // --- automations ---
  // Push to the phone via notify.<HA_NOTIFY_TARGET> (HA companion app).
  bool notify(const std::string &title, const std::string &message,
              const nlohmann::json &data, std::string &error);
  // sun.sun below the horizon? nullopt if unknown (cached 1 min).
  std::optional<bool> sun_below_horizon();
  // Raw state string of any entity (nullopt + error if HA can't be read).
  std::optional<std::string> entity_state(const std::string &entity, std::string &error);
  // GET /api/ answers -> Home Assistant is reachable and the token works.
  bool ping(std::string &error);
  // Creates/updates an entity state via REST (POST /api/states/<entity>).
  bool set_state(const std::string &entity, const std::string &state,
                 const nlohmann::json &attributes, std::string &error);
  // Home zone (lat, lon, radius m), cached.
  std::optional<std::array<double, 3>> home_zone();
  bool media_command(const dashboard_backend::MediaCommand &cmd,
                     std::string &error);

  // Every entity the dashboard reads (for the live WebSocket subscription).
  static std::vector<std::string> watched_entities(const Config &cfg);
  static std::vector<std::string> sleep_entities(const Config &cfg);
  bool complete_task(const std::string &entity, const std::string &uid,
                     std::string &error);

private:
  std::optional<nlohmann::json> get_state(const std::string &entity,
                                          std::string &error);
  std::optional<nlohmann::json> call_service(const std::string &domain,
                                             const std::string &service,
                                             const nlohmann::json &data,
                                             bool return_response,
                                             std::string &error);

  void fill_presence(dashboard_backend::Dashboard &, std::vector<std::string> &);
  void fill_light_sensor(dashboard_backend::Dashboard &, std::vector<std::string> &);
  void fill_weather(dashboard_backend::Dashboard &, std::vector<std::string> &);
  void fill_phone(dashboard_backend::Dashboard &, std::vector<std::string> &);
  void fill_todo(dashboard_backend::Dashboard &, std::vector<std::string> &);
  void fill_lamps(dashboard_backend::Dashboard &, std::vector<std::string> &);
  void update_cover(const std::string &entity, const std::string &picture,
                    dashboard_backend::Media *m);
  void fill_glance(dashboard_backend::Dashboard &, std::vector<std::string> &);
  // State of an entity, or nullopt (error recorded) / unavailable.
  std::optional<nlohmann::json> state_or_error(const std::string &entity,
                                               std::vector<std::string> &errors);

  const Config &cfg_;
  CoverCache &covers_;
  HttpClient http_;
  std::vector<std::string> auth_headers_;

  std::map<std::string, std::string> todo_names_;
  std::optional<std::array<double, 3>> home_zone_;
  std::chrono::steady_clock::time_point home_zone_fetched_{}; // entity -> friendly name
  std::optional<bool> sun_below_;
  std::chrono::steady_clock::time_point sun_fetched_{};

  // Forecasts change slowly; don't ask HA for them on every poll.
  std::vector<dashboard_backend::Forecast> forecast_cache_;
  std::chrono::steady_clock::time_point forecast_fetched_{};
};
