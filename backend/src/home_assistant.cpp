#include "home_assistant.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

using nlohmann::json;
namespace dash = dashboard_backend;

namespace {

uint64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Parses HA timestamps like "2026-11-01T09:12:33.123456+00:00" into Unix ms.
std::optional<uint64_t> parse_iso8601(const std::string &s) {
  int Y, M, D, h = 0, m = 0;
  double sec = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &m, &sec) <
      3)
    return std::nullopt;

  std::tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = 0;
  int64_t t = static_cast<int64_t>(timegm(&tm)) * 1000 +
              static_cast<int64_t>(std::llround(sec * 1000.0));

  // Timezone offset after the time part: Z, +HH:MM or -HH:MM
  auto tpos = s.find('T');
  if (tpos != std::string::npos) {
    auto zpos = s.find_first_of("Z+-", tpos);
    if (zpos != std::string::npos && s[zpos] != 'Z') {
      int oh = 0, om = 0;
      if (std::sscanf(s.c_str() + zpos + 1, "%d:%d", &oh, &om) >= 1) {
        int64_t off = (oh * 60 + om) * 60 * 1000LL;
        t += (s[zpos] == '+') ? -off : off;
      }
    }
  }
  return t < 0 ? std::nullopt : std::optional<uint64_t>(t);
}

bool is_unavailable(const std::string &state) {
  return state == "unavailable" || state == "unknown" || state.empty();
}

std::optional<double> state_number(const json &st) {
  std::string s = st.value("state", "");
  if (is_unavailable(s)) return std::nullopt;
  try {
    return std::stod(s);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> attr_number(const json &attrs, const char *key) {
  if (!attrs.contains(key)) return std::nullopt;
  const auto &v = attrs[key];
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
    }
  }
  return std::nullopt;
}

std::string attr_string(const json &attrs, const char *key) {
  if (attrs.contains(key) && attrs[key].is_string())
    return attrs[key].get<std::string>();
  return {};
}

} // namespace

HomeAssistant::HomeAssistant(const Config &cfg, CoverCache &covers)
    : cfg_(cfg), covers_(covers) {
  auth_headers_ = {"Authorization: Bearer " + cfg_.ha_token,
                   "Accept: application/json"};
}

std::optional<json> HomeAssistant::get_state(const std::string &entity,
                                             std::string &error) {
  auto res = http_.get(cfg_.ha_url + "/api/states/" + entity, auth_headers_);
  if (!res.ok()) {
    error = entity + ": " +
            (res.status ? "HTTP " + std::to_string(res.status) : res.error);
    return std::nullopt;
  }
  try {
    return json::parse(res.body);
  } catch (const std::exception &e) {
    error = entity + ": invalid JSON";
    return std::nullopt;
  }
}

std::optional<json> HomeAssistant::call_service(const std::string &domain,
                                                const std::string &service,
                                                const json &data,
                                                bool return_response,
                                                std::string &error) {
  std::string url = cfg_.ha_url + "/api/services/" + domain + "/" + service;
  if (return_response) url += "?return_response";
  auto res = http_.post(url, data.dump(), auth_headers_);
  if (!res.ok()) {
    error = domain + "." + service + ": " +
            (res.status ? "HTTP " + std::to_string(res.status) : res.error);
    if (res.status && !res.body.empty()) error += " " + res.body.substr(0, 200);
    return std::nullopt;
  }
  try {
    return json::parse(res.body);
  } catch (...) {
    return json::object();
  }
}

void HomeAssistant::fill(dash::Dashboard &out, std::vector<std::string> &errors) {
  if (!cfg_.ha_enabled()) return;
  fill_presence(out, errors);
  fill_light_sensor(out, errors);
  fill_weather(out, errors);
  fill_phone(out, errors);
  fill_todo(out, errors);
  fill_lamps(out, errors);
  fill_glance(out, errors);
}

void HomeAssistant::fill_presence(dash::Dashboard &out,
                                  std::vector<std::string> &errors) {
  if (cfg_.presence_entity.empty()) return;
  std::string err;
  auto st = get_state(cfg_.presence_entity, err);
  if (!st) return errors.push_back(err);

  std::string s = st->value("state", "");
  if (is_unavailable(s)) return;
  bool here = s == "home" || s == "on" || s == "detected" || s == "true" ||
              s == "occupied";
  auto *p = out.mutable_presence();
  p->set_is_now(here);
  if (here) {
    p->set_last_seen(now_ms());
  } else if (auto t = parse_iso8601(st->value("last_changed", ""))) {
    p->set_last_seen(*t);
  }
}

void HomeAssistant::fill_light_sensor(dash::Dashboard &out,
                                      std::vector<std::string> &errors) {
  if (cfg_.lux_entity.empty()) return;
  std::string err;
  auto st = get_state(cfg_.lux_entity, err);
  if (!st) return errors.push_back(err);

  // Works for numeric lux sensors and for binary "is light" sensors.
  std::string s = st->value("state", "");
  if (is_unavailable(s)) return;
  auto *l = out.mutable_light();
  if (auto lux = state_number(*st)) {
    l->set_lux_value(static_cast<int32_t>(std::lround(*lux)));
    l->set_is_light(*lux >= cfg_.lux_threshold);
  } else {
    l->set_is_light(s == "on");
  }
}

void HomeAssistant::fill_weather(dash::Dashboard &out,
                                 std::vector<std::string> &errors) {
  if (cfg_.weather_entity.empty()) return;
  std::string err;
  auto st = get_state(cfg_.weather_entity, err);
  if (!st) return errors.push_back(err);

  auto *w = out.mutable_weather();
  const json attrs = st->value("attributes", json::object());
  std::string s = st->value("state", "");
  if (!is_unavailable(s)) w->set_condition(s);
  if (auto v = attr_number(attrs, "temperature")) w->set_temperature(*v);
  if (auto v = attr_number(attrs, "humidity")) w->set_humidity(*v);
  if (auto v = attr_number(attrs, "wind_speed")) w->set_wind_speed(*v);
  w->set_temperature_unit(attr_string(attrs, "temperature_unit"));
  w->set_wind_speed_unit(attr_string(attrs, "wind_speed_unit"));

  // Forecast: refresh at most every 10 minutes.
  auto now = std::chrono::steady_clock::now();
  if (forecast_cache_.empty() ||
      now - forecast_fetched_ > std::chrono::minutes(10)) {
    json data = {{"entity_id", cfg_.weather_entity}, {"type", "daily"}};
    auto resp = call_service("weather", "get_forecasts", data, true, err);
    if (!resp) {
      errors.push_back(err);
    } else {
      forecast_cache_.clear();
      try {
        const auto &list =
            resp->at("service_response").at(cfg_.weather_entity).at("forecast");
        for (const auto &f : list) {
          if (forecast_cache_.size() >= 7) break;
          dash::Forecast fc;
          if (auto t = parse_iso8601(f.value("datetime", ""))) fc.set_time(*t);
          fc.set_condition(f.value("condition", ""));
          if (auto v = attr_number(f, "temperature")) fc.set_temperature(*v);
          if (auto v = attr_number(f, "templow")) fc.set_temp_low(*v);
          if (auto v = attr_number(f, "precipitation")) fc.set_precipitation(*v);
          if (auto v = attr_number(f, "precipitation_probability"))
            fc.set_precipitation_probability(static_cast<int32_t>(*v));
          forecast_cache_.push_back(std::move(fc));
        }
        forecast_fetched_ = now;
      } catch (const std::exception &e) {
        errors.push_back("weather forecast: unexpected response");
      }
    }
  }
  for (const auto &fc : forecast_cache_) *w->add_forecast() = fc;
}

void HomeAssistant::fill_phone(dash::Dashboard &out,
                               std::vector<std::string> &errors) {
  struct Item {
    const std::string &entity;
    void (dash::Phone::*setter)(int32_t);
  } items[] = {
      {cfg_.steps_entity, &dash::Phone::set_steps},
      {cfg_.heart_rate_entity, &dash::Phone::set_heart_rate},
      {cfg_.battery_entity, &dash::Phone::set_battery},
  };
  for (const auto &it : items) {
    if (it.entity.empty()) continue;
    std::string err;
    auto st = get_state(it.entity, err);
    if (!st) {
      errors.push_back(err);
      continue;
    }
    if (auto v = state_number(*st))
      (out.mutable_phone()->*it.setter)(static_cast<int32_t>(std::lround(*v)));
  }
}

void HomeAssistant::fill_todo(dash::Dashboard &out,
                              std::vector<std::string> &errors) {
  if (cfg_.todo_entities.empty()) return;

  // List names (friendly_name) rarely change: fetch once per list.
  for (const auto &e : cfg_.todo_entities) {
    if (todo_names_.count(e)) continue;
    std::string err;
    if (auto st = get_state(e, err)) {
      std::string n = attr_string(st->value("attributes", json::object()), "friendly_name");
      todo_names_[e] = n.empty() ? e : n;
    } else {
      errors.push_back(err);
    }
  }

  // One call returns the open items of every list.
  std::string err;
  json data = {{"entity_id", cfg_.todo_entities}, {"status", "needs_action"}};
  auto resp = call_service("todo", "get_items", data, true, err);
  if (!resp) return errors.push_back(err);

  auto *todo = out.mutable_todoist();
  for (const auto &entity : cfg_.todo_entities) {
    auto *list = todo->add_lists();
    list->set_entity_id(entity);
    auto name = todo_names_.find(entity);
    list->set_name(name != todo_names_.end() ? name->second : entity);
    try {
      const auto &items = resp->at("service_response").at(entity).at("items");
      std::vector<dash::Task> tasks;
      for (const auto &i : items) {
        dash::Task t;
        t.set_uid(i.value("uid", ""));
        t.set_summary(i.value("summary", ""));
        if (i.contains("due") && i["due"].is_string()) t.set_due(i["due"]);
        if (i.contains("description") && i["description"].is_string())
          t.set_description(i["description"]);
        tasks.push_back(std::move(t));
      }
      // Tasks with a due date first (earliest first), then the rest.
      std::stable_sort(tasks.begin(), tasks.end(),
                       [](const dash::Task &a, const dash::Task &b) {
                         if (a.due().empty() != b.due().empty())
                           return !a.due().empty();
                         return a.due() < b.due();
                       });
      for (auto &t : tasks) *list->add_task() = std::move(t);
    } catch (const std::exception &) {
      errors.push_back(entity + ": unexpected response");
    }
  }
}

void HomeAssistant::fill_lamps(dash::Dashboard &out,
                               std::vector<std::string> &errors) {
  for (const auto &entity : cfg_.light_entities) {
    std::string err;
    auto st = get_state(entity, err);
    if (!st) {
      errors.push_back(err);
      continue;
    }
    const json attrs = st->value("attributes", json::object());
    std::string s = st->value("state", "");

    auto *lamp = out.add_lamps();
    lamp->set_entity_id(entity);
    std::string name = attr_string(attrs, "friendly_name");
    lamp->set_name(name.empty() ? entity : name);
    lamp->set_available(!is_unavailable(s));
    lamp->set_is_on(s == "on");

    if (attrs.contains("supported_color_modes") &&
        attrs["supported_color_modes"].is_array()) {
      for (const auto &m : attrs["supported_color_modes"]) {
        std::string mode = m.is_string() ? m.get<std::string>() : "";
        if (mode == "hs" || mode == "rgb" || mode == "rgbw" ||
            mode == "rgbww" || mode == "xy")
          lamp->set_supports_color(true);
        if (mode != "onoff") lamp->set_supports_brightness(true);
      }
    }
    if (auto b = attr_number(attrs, "brightness"))
      lamp->set_brightness(static_cast<uint32_t>(*b));
    if (attrs.contains("rgb_color") && attrs["rgb_color"].is_array() &&
        attrs["rgb_color"].size() == 3) {
      const auto &c = attrs["rgb_color"];
      uint32_t rgb = (c[0].get<uint32_t>() & 0xff) << 16 |
                     (c[1].get<uint32_t>() & 0xff) << 8 |
                     (c[2].get<uint32_t>() & 0xff);
      lamp->set_rgb(rgb);
    }
  }
}

bool HomeAssistant::light_command(const dash::LightCommand &cmd,
                                  std::string &error) {
  if (!cfg_.ha_enabled()) {
    error = "Home Assistant not configured";
    return false;
  }
  // Only allow lights that are configured for the dashboard.
  if (std::find(cfg_.light_entities.begin(), cfg_.light_entities.end(),
                cmd.entity_id()) == cfg_.light_entities.end()) {
    error = "unknown light " + cmd.entity_id();
    return false;
  }

  json data = {{"entity_id", cmd.entity_id()}};
  std::string service;
  if (cmd.has_rgb() || cmd.has_brightness()) {
    service = "turn_on";
    if (cmd.has_rgb()) {
      uint32_t c = cmd.rgb();
      data["rgb_color"] = {(c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff};
    }
    if (cmd.has_brightness())
      data["brightness"] = std::min<uint32_t>(cmd.brightness(), 255);
  } else if (cmd.has_on()) {
    service = cmd.on() ? "turn_on" : "turn_off";
  } else {
    service = "toggle";
  }
  return call_service("light", service, data, false, error).has_value();
}

bool HomeAssistant::all_lights(bool on, std::string &error) {
  if (!cfg_.ha_enabled() || cfg_.light_entities.empty()) {
    error = "no lights configured";
    return false;
  }
  json data = {{"entity_id", cfg_.light_entities}};
  return call_service("light", on ? "turn_on" : "turn_off", data, false, error)
      .has_value();
}

bool HomeAssistant::complete_task(const std::string &entity,
                                  const std::string &uid, std::string &error) {
  // Only allow lists that are configured for the dashboard.
  if (!cfg_.ha_enabled() ||
      std::find(cfg_.todo_entities.begin(), cfg_.todo_entities.end(), entity) ==
          cfg_.todo_entities.end()) {
    error = "unknown to-do list " + entity;
    return false;
  }
  json data = {{"entity_id", entity}, {"item", uid}, {"status", "completed"}};
  return call_service("todo", "update_item", data, false, error).has_value();
}

// ---------------------------------------------------------------------------
// At a glance (alarm, network, media) + sleep
// ---------------------------------------------------------------------------

std::vector<std::string> HomeAssistant::sleep_entities(const Config &cfg) {
  if (cfg.sleep_prefix.empty()) return {};
  const auto &p = cfg.sleep_prefix;
  return {p + "_sleep_duration", p + "_deep_sleep", p + "_rem_sleep",
          p + "_core_sleep", p + "_awake"};
}

std::vector<std::string> HomeAssistant::watched_entities(const Config &cfg) {
  std::vector<std::string> out;
  for (const auto *e :
       {&cfg.presence_entity, &cfg.lux_entity, &cfg.weather_entity,
        &cfg.steps_entity, &cfg.heart_rate_entity, &cfg.battery_entity,
        &cfg.alarm_entity, &cfg.wan_entity, &cfg.download_entity,
        &cfg.upload_entity, &cfg.alexa_entity})
    if (!e->empty()) out.push_back(*e);
  for (const auto *list : {&cfg.light_entities, &cfg.todo_entities, &cfg.media_entities})
    out.insert(out.end(), list->begin(), list->end());
  auto sleep = sleep_entities(cfg);
  out.insert(out.end(), sleep.begin(), sleep.end());
  return out;
}

std::optional<json> HomeAssistant::state_or_error(const std::string &entity,
                                                  std::vector<std::string> &errors) {
  if (entity.empty()) return std::nullopt;
  std::string err;
  auto st = get_state(entity, err);
  if (!st) errors.push_back(err);
  return st;
}

void HomeAssistant::fill_glance(dash::Dashboard &out,
                                std::vector<std::string> &errors) {
  auto *g = out.mutable_glance();

  if (auto st = state_or_error(cfg_.alarm_entity, errors))
    if (auto t = parse_iso8601(st->value("state", ""))) g->set_next_alarm(*t);

  auto sleep_ids = sleep_entities(cfg_);
  if (!sleep_ids.empty()) {
    void (dash::Sleep::*setters[])(int32_t) = {
        &dash::Sleep::set_total, &dash::Sleep::set_deep, &dash::Sleep::set_rem,
        &dash::Sleep::set_core, &dash::Sleep::set_awake};
    for (size_t i = 0; i < sleep_ids.size(); ++i) {
      auto st = state_or_error(sleep_ids[i], errors);
      if (!st) continue;
      if (auto v = state_number(*st))
        (out.mutable_sleep()->*setters[i])(static_cast<int32_t>(std::lround(*v)));
    }
  }

  if (!cfg_.wan_entity.empty() || !cfg_.download_entity.empty()) {
    auto *net = g->mutable_network();
    if (auto st = state_or_error(cfg_.wan_entity, errors)) {
      std::string s = st->value("state", "");
      if (!is_unavailable(s)) net->set_online(s == "on");
    }
    if (auto st = state_or_error(cfg_.download_entity, errors))
      if (auto v = state_number(*st)) net->set_download_kib(*v);
    if (auto st = state_or_error(cfg_.upload_entity, errors))
      if (auto v = state_number(*st)) net->set_upload_kib(*v);
  }

  for (const auto &entity : cfg_.media_entities) {
    auto st = state_or_error(entity, errors);
    if (!st) continue;
    const json attrs = st->value("attributes", json::object());
    auto *m = g->add_media();
    m->set_entity_id(entity);
    std::string name = attr_string(attrs, "friendly_name");
    m->set_name(name.empty() ? entity : name);
    m->set_state(st->value("state", ""));
    m->set_title(attr_string(attrs, "media_title"));
    std::string artist = attr_string(attrs, "media_artist");
    if (artist.empty()) artist = attr_string(attrs, "media_album_artist");
    m->set_artist(artist);
    m->set_album(attr_string(attrs, "media_album_name"));
    m->set_source(attr_string(attrs, "source"));
    if (attrs.contains("source_list") && attrs["source_list"].is_array())
      for (const auto &s : attrs["source_list"])
        if (s.is_string()) m->add_sources(s.get<std::string>());
    if (auto v = attr_number(attrs, "volume_level")) m->set_volume(*v);
    if (auto v = attr_number(attrs, "media_duration")) m->set_duration(*v);
    if (auto v = attr_number(attrs, "media_position")) m->set_position(*v);
    if (auto t = parse_iso8601(attr_string(attrs, "media_position_updated_at")))
      m->set_position_updated_at(*t);
    if (attrs.contains("shuffle") && attrs["shuffle"].is_boolean())
      m->set_shuffle(attrs["shuffle"].get<bool>());
    update_cover(entity, attr_string(attrs, "entity_picture"), m);
  }
}

void HomeAssistant::update_cover(const std::string &entity,
                                 const std::string &picture, dash::Media *m) {
  if (picture.empty()) return;
  // entity_picture changes (cache=...) with every new track: fetch once.
  if (covers_.source(entity) != picture) {
    std::string url = picture.rfind("http", 0) == 0 ? picture : cfg_.ha_url + picture;
    auto res = http_.get(url, auth_headers_);
    if (!res.ok() || res.body.empty()) return;
    std::string mime = res.content_type.empty() ? "image/jpeg" : res.content_type;
    covers_.put(entity, {picture, mime, std::move(res.body)});
  }
  // Versioned URL so browsers can cache each cover forever.
  m->set_cover("/api/cover/" + entity + "?v=" +
               std::to_string(std::hash<std::string>{}(picture) & 0xffffffff));
}

bool HomeAssistant::media_command(const dash::MediaCommand &cmd,
                                  std::string &error) {
  if (std::find(cfg_.media_entities.begin(), cfg_.media_entities.end(),
                cmd.entity_id()) == cfg_.media_entities.end()) {
    error = "unknown media player " + cmd.entity_id();
    return false;
  }
  json data = {{"entity_id", cmd.entity_id()}};
  const char *service = "media_play_pause";
  switch (cmd.action()) {
  case dash::MediaCommand::NEXT: service = "media_next_track"; break;
  case dash::MediaCommand::PREVIOUS: service = "media_previous_track"; break;
  case dash::MediaCommand::SET_VOLUME:
    service = "volume_set";
    data["volume_level"] = std::clamp(cmd.volume(), 0.0f, 1.0f);
    break;
  case dash::MediaCommand::SELECT_SOURCE:
    service = "select_source";
    data["source"] = cmd.source();
    break;
  case dash::MediaCommand::SET_SHUFFLE:
    service = "shuffle_set";
    data["shuffle"] = cmd.shuffle();
    break;
  default: break;
  }
  return call_service("media_player", service, data, false, error).has_value();
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------
std::optional<HomeAssistant::Utterance> HomeAssistant::last_called(std::string &error) {
  if (cfg_.alexa_entity.empty()) return std::nullopt;
  auto st = get_state(cfg_.alexa_entity, error);
  if (!st) return std::nullopt;
  const json attrs = st->value("attributes", json::object());
  if (!attrs.value("last_called", false)) return std::nullopt; // spoken to another Echo
  auto ts = attr_number(attrs, "last_called_timestamp");
  std::string text = attr_string(attrs, "last_called_summary");
  if (!ts || text.empty()) return std::nullopt;
  return Utterance{text, static_cast<int64_t>(*ts)};
}

namespace {
std::string ssml_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}
} // namespace

bool HomeAssistant::speak(const std::string &text, std::string &error) {
  if (cfg_.alexa_notify.empty()) {
    error = "HA_ALEXA_NOTIFY not set";
    return false;
  }
  std::string ssml = "<speak>" +
                     (cfg_.voice_name.empty()
                          ? ssml_escape(text)
                          : "<voice name=\"" + ssml_escape(cfg_.voice_name) + "\">" +
                                ssml_escape(text) + "</voice>") +
                     "</speak>";
  json data = {{"message", ssml}, {"data", {{"type", "tts"}}}};
  return call_service("notify", cfg_.alexa_notify, data, false, error).has_value();
}

bool HomeAssistant::set_lights(bool on, std::optional<int> brightness_pct, int transition_s,
                               std::string &error) {
  if (cfg_.light_entities.empty()) {
    error = "no lights configured";
    return false;
  }
  json data = {{"entity_id", cfg_.light_entities}};
  if (on && brightness_pct) data["brightness_pct"] = std::clamp(*brightness_pct, 1, 100);
  if (transition_s > 0) data["transition"] = transition_s;
  return call_service("light", on ? "turn_on" : "turn_off", data, false, error).has_value();
}

bool HomeAssistant::pause_media(std::string &error) {
  if (cfg_.media_entities.empty()) return true;
  return call_service("media_player", "media_pause", {{"entity_id", cfg_.media_entities}},
                      false, error)
      .has_value();
}

bool HomeAssistant::set_state(const std::string &entity, const std::string &state,
                              const json &attributes, std::string &error) {
  json body = {{"state", state}, {"attributes", attributes}};
  auto res = http_.post(cfg_.ha_url + "/api/states/" + entity, body.dump(), auth_headers_);
  if (!res.ok()) {
    error = entity + ": " + (res.status ? "HTTP " + std::to_string(res.status) : res.error);
    return false;
  }
  return true;
}

std::optional<std::array<double, 3>> HomeAssistant::home_zone() {
  auto now = std::chrono::steady_clock::now();
  if (home_zone_fetched_ != std::chrono::steady_clock::time_point{} &&
      now - home_zone_fetched_ < std::chrono::hours(6))
    return home_zone_;
  home_zone_fetched_ = now;
  std::string err;
  auto st = get_state("zone.home", err);
  if (!st || !st->contains("attributes")) return home_zone_;
  const auto &a = (*st)["attributes"];
  if (a.contains("latitude") && a["latitude"].is_number() && a.contains("longitude") &&
      a["longitude"].is_number())
    home_zone_ = std::array<double, 3>{a["latitude"].get<double>(), a["longitude"].get<double>(),
                                       a.value("radius", 100.0)};
  return home_zone_;
}
