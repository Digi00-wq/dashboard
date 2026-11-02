#include "voice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace dash = dashboard_backend;

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::tm local(int64_t ms) {
  std::time_t t = ms / 1000;
  std::tm tm{};
  localtime_r(&t, &tm);
  return tm;
}

// "6 Uhr 15", "7 Uhr"
std::string spoken_time(int64_t ms) {
  std::tm tm = local(ms);
  std::string s = std::to_string(tm.tm_hour) + " Uhr";
  if (tm.tm_min) s += " " + std::to_string(tm.tm_min);
  return s;
}

std::string iso_date(int64_t ms) {
  std::tm tm = local(ms);
  char buf[11];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

// "7 Stunden 32 Minuten"
std::string spoken_duration(int minutes) {
  int h = minutes / 60, m = minutes % 60;
  std::string s;
  if (h) s = std::to_string(h) + (h == 1 ? " Stunde" : " Stunden");
  if (m) s += (s.empty() ? "" : " ") + std::to_string(m) + (m == 1 ? " Minute" : " Minuten");
  return s.empty() ? "0 Minuten" : s;
}

std::string join_and(const std::vector<std::string> &items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += (i + 1 == items.size()) ? " und " : ", ";
    out += items[i];
  }
  return out;
}

// lower-case ASCII, strip punctuation and the wake word, collapse spaces
std::string normalize(std::string s) {
  std::string out;
  bool space = false;
  for (unsigned char c : s) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c == '.' || c == ',' || c == '!' || c == '?') continue;
    if (c == ' ' || c == '\t') {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += char(c);
  }
  if (out.rfind("alexa ", 0) == 0) out.erase(0, 6);
  return out;
}

const std::map<std::string, std::string> kWeather = {
    {"clear-night", "klar"},        {"cloudy", "bewölkt"},
    {"exceptional", "Unwetter"},    {"fog", "neblig"},
    {"hail", "Hagel"},              {"lightning", "Gewitter"},
    {"lightning-rainy", "Gewitter mit Regen"},
    {"partlycloudy", "teilweise bewölkt"}, {"pouring", "Starkregen"},
    {"rainy", "Regen"},             {"snowy", "Schnee"},
    {"snowy-rainy", "Schneeregen"}, {"sunny", "sonnig"},
    {"windy", "windig"},            {"windy-variant", "windig"},
};

enum class Cmd { LightsOn, LightsOff, LightsDim, LightsBright, Server, Briefing, GoodNight, Tasks, Sleep };

const std::map<std::string, Cmd> kPhrases = {
    {"licht an", Cmd::LightsOn},          {"licht ein", Cmd::LightsOn},
    {"lampen an", Cmd::LightsOn},         {"mach das licht an", Cmd::LightsOn},
    {"licht aus", Cmd::LightsOff},        {"lampen aus", Cmd::LightsOff},
    {"mach das licht aus", Cmd::LightsOff},
    {"licht dimmen", Cmd::LightsDim},     {"licht gedimmt", Cmd::LightsDim},
    {"licht dunkler", Cmd::LightsDim},
    {"licht hell", Cmd::LightsBright},    {"licht heller", Cmd::LightsBright},
    {"volles licht", Cmd::LightsBright},
    {"serverstatus", Cmd::Server},        {"server status", Cmd::Server},
    {"wie geht es dem server", Cmd::Server}, {"wie gehts dem server", Cmd::Server},
    {"guten morgen", Cmd::Briefing},      {"briefing", Cmd::Briefing},
    {"morgenbriefing", Cmd::Briefing},
    {"gute nacht", Cmd::GoodNight},
    {"was steht an", Cmd::Tasks},         {"was steht heute an", Cmd::Tasks},
    {"meine aufgaben", Cmd::Tasks},       {"aufgaben", Cmd::Tasks},
    {"wie habe ich geschlafen", Cmd::Sleep}, {"wie hab ich geschlafen", Cmd::Sleep},
};

} // namespace

Voice::Voice(const Config &cfg, HomeAssistant &ha) : cfg_(cfg), ha_(ha) {}

void Voice::say(const std::string &text) {
  std::string err;
  std::cout << "[Voice] say: " << text << "\n";
  if (!ha_.speak(text, err)) std::cerr << "[Voice] speak failed: " << err << "\n";
}

void Voice::tick(const dash::Dashboard &snap) {
  if (!cfg_.voice_enabled()) return;
  int64_t now = now_ms();

  std::string err;
  if (auto u = ha_.last_called(err)) {
    if (last_ts_ < 0) {
      last_ts_ = u->timestamp; // startup: don't replay old commands
    } else if (u->timestamp > last_ts_) {
      last_ts_ = u->timestamp;
      std::string phrase = normalize(u->text);
      // Ignore stale events (e.g. after a long HA outage).
      if (now - u->timestamp < 2 * 60 * 1000 && handle(phrase, snap))
        std::cout << "[Voice] handled: \"" << phrase << "\"\n";
    }
  }
  alarms(snap, now);
}

bool Voice::handle(const std::string &phrase, const dash::Dashboard &snap) {
  auto it = kPhrases.find(phrase);
  if (it == kPhrases.end()) return false; // a normal Alexa command, not ours
  std::string err;
  switch (it->second) {
  case Cmd::LightsOn:
    if (ha_.set_lights(true, 100, 1, err)) say("Licht ist an.");
    break;
  case Cmd::LightsOff:
    if (ha_.set_lights(false, std::nullopt, 1, err)) say("Licht ist aus.");
    break;
  case Cmd::LightsDim:
    if (ha_.set_lights(true, 25, 2, err)) say("Licht gedimmt.");
    break;
  case Cmd::LightsBright:
    if (ha_.set_lights(true, 100, 1, err)) say("Volles Licht.");
    break;
  case Cmd::Server:
    say(server_status(snap));
    break;
  case Cmd::Briefing:
    say(briefing(snap));
    break;
  case Cmd::Tasks:
    say(tasks_today(snap));
    break;
  case Cmd::Sleep:
    say(sleep_report(snap));
    break;
  case Cmd::GoodNight: {
    ha_.set_lights(false, std::nullopt, 3, err);
    ha_.pause_media(err);
    std::string text = "Gute Nacht" + (cfg_.voice_user.empty() ? "" : ", " + cfg_.voice_user) + ".";
    if (snap.has_glance() && snap.glance().has_next_alarm() &&
        snap.glance().next_alarm() - now_ms() < 24 * 3600 * 1000LL)
      text += " Dein Wecker klingelt um " + spoken_time(snap.glance().next_alarm()) + ".";
    say(text);
    break;
  }
  }
  if (!err.empty()) {
    std::cerr << "[Voice] " << err << "\n";
    say("Das hat leider nicht geklappt.");
  }
  return true;
}

// Morning briefing after the alarm + optional wake-up light before it.
// The alarm sensor jumps to the *next* alarm as soon as one rings, so we
// remember the alarm we are waiting for until we've handled it.
void Voice::alarms(const dash::Dashboard &snap, int64_t now) {
  std::optional<int64_t> next;
  if (snap.has_glance() && snap.glance().has_next_alarm()) next = snap.glance().next_alarm();

  if (tracked_alarm_ && cfg_.briefing_delay_min >= 0 &&
      now >= *tracked_alarm_ + cfg_.briefing_delay_min * 60 * 1000LL) {
    // Only if someone is actually in the room (when a presence sensor exists).
    bool here = !snap.has_presence() || !snap.presence().has_is_now() || snap.presence().is_now();
    if (now - *tracked_alarm_ < 60 * 60 * 1000LL && here) say(briefing(snap));
    else std::cout << "[Voice] briefing skipped (nobody in the room)\n";
    tracked_alarm_.reset();
  }

  if (next && *next > now && (!tracked_alarm_ || *tracked_alarm_ > now)) {
    if (tracked_alarm_ != next) wake_light_done_ = false; // alarm (re)set
    tracked_alarm_ = next;
  }

  if (tracked_alarm_ && cfg_.wake_light_min > 0 && !wake_light_done_ &&
      now >= *tracked_alarm_ - cfg_.wake_light_min * 60 * 1000LL && now < *tracked_alarm_) {
    std::string err;
    int fade = int((*tracked_alarm_ - now) / 1000);
    std::cout << "[Voice] wake-up light, fading in over " << fade << "s\n";
    if (!ha_.set_lights(true, 100, fade, err)) std::cerr << "[Voice] " << err << "\n";
    wake_light_done_ = true;
  }
}

// ---------------------------------------------------------------------------
// Texts (German, spoken by the Polly voice)
// ---------------------------------------------------------------------------
std::string Voice::weather_report(const dash::Dashboard &snap) const {
  if (!snap.has_weather() || !snap.weather().has_temperature()) return "";
  const auto &w = snap.weather();
  auto cond = kWeather.find(w.condition());
  std::string s = "Draussen sind es " + std::to_string(int(std::lround(w.temperature()))) + " Grad";
  if (cond != kWeather.end()) s += ", " + cond->second;
  s += ".";
  if (w.forecast_size() > 0) {
    const auto &f = w.forecast(0);
    if (f.has_temperature())
      s += " Heute bis " + std::to_string(int(std::lround(f.temperature()))) + " Grad.";
    if (f.has_precipitation_probability() && f.precipitation_probability() >= 30)
      s += " Regenwahrscheinlichkeit " + std::to_string(f.precipitation_probability()) + " Prozent.";
  }
  double t = w.temperature();
  std::string outfit = t < 0 ? "Winterjacke, Mütze und Handschuhe" : t < 8 ? "eine warme Jacke"
                       : t < 15 ? "eine leichte Jacke" : t < 22 ? "einen Pullover oder ein T-Shirt"
                                                          : "ein T-Shirt";
  s += " Zieh " + outfit + " an.";
  bool rain = w.forecast_size() > 0 && w.forecast(0).has_precipitation_probability() &&
              w.forecast(0).precipitation_probability() >= 50;
  if (rain || w.condition() == "rainy" || w.condition() == "pouring")
    s += " Und nimm einen Regenschirm mit.";
  return s;
}

std::string Voice::sleep_report(const dash::Dashboard &snap) const {
  if (!snap.has_sleep() || !snap.sleep().has_total()) return "Ich habe keine Schlafdaten.";
  const auto &s = snap.sleep();
  std::string out = "Du hast " + spoken_duration(s.total()) + " geschlafen";
  if (s.has_deep() && s.deep() > 0) out += ", davon " + spoken_duration(s.deep()) + " Tiefschlaf";
  return out + ".";
}

std::string Voice::tasks_today(const dash::Dashboard &snap) const {
  std::string today = iso_date(now_ms());
  std::vector<std::string> due;
  int overdue = 0;
  for (const auto &list : snap.todoist().lists())
    for (const auto &t : list.task()) {
      if (t.due().size() < 10) continue;
      std::string d = t.due().substr(0, 10);
      if (d == today) due.push_back(t.summary());
      else if (d < today) ++overdue;
    }
  std::string s;
  if (due.empty()) s = "Heute ist keine Aufgabe fällig.";
  else if (due.size() <= 3) s = "Heute fällig: " + join_and(due) + ".";
  else s = "Heute sind " + std::to_string(due.size()) + " Aufgaben fällig, zum Beispiel " +
           join_and({due[0], due[1]}) + ".";
  if (overdue == 1) s += " Eine Aufgabe ist überfällig.";
  else if (overdue > 1) s += " " + std::to_string(overdue) + " Aufgaben sind überfällig.";
  return s;
}

std::string Voice::server_status(const dash::Dashboard &snap) const {
  if (!snap.has_server()) return "Ich habe keine Serverdaten.";
  const auto &sv = snap.server();
  std::ostringstream s;
  int days = int(sv.uptime_seconds() / 86400);
  s << "Der Server läuft seit ";
  if (days > 0) s << days << (days == 1 ? " Tag" : " Tagen");
  else s << spoken_duration(int(sv.uptime_seconds() / 60));
  s << ". Prozessor " << std::lround(sv.cpu_percent()) << " Prozent";
  if (sv.has_temperature()) s << " bei " << std::lround(sv.temperature()) << " Grad";
  s << ". Arbeitsspeicher " << std::lround(sv.mem_total() ? 100.0 * sv.mem_used() / sv.mem_total() : 0)
    << " Prozent belegt, Festplatte "
    << std::lround(sv.disk_total() ? 100.0 * sv.disk_used() / sv.disk_total() : 0)
    << " Prozent.";
  if (snap.has_glance() && snap.glance().has_network() && snap.glance().network().has_online())
    s << (snap.glance().network().online() ? " Internet ist online." : " Achtung, das Internet ist offline!");
  if (snap.errors_size() > 0)
    s << " " << snap.errors_size() << (snap.errors_size() == 1 ? " Datenquelle meldet" : " Datenquellen melden")
      << " einen Fehler.";
  else
    s << " Alles läuft.";
  return s.str();
}

std::string Voice::briefing(const dash::Dashboard &snap) const {
  std::string s = "Guten Morgen" + (cfg_.voice_user.empty() ? "" : " " + cfg_.voice_user) +
                  ". Es ist " + spoken_time(now_ms()) + ".";
  if (auto w = weather_report(snap); !w.empty()) s += " " + w;
  if (snap.has_sleep() && snap.sleep().has_total()) s += " " + sleep_report(snap);
  s += " " + tasks_today(snap);
  s += " Einen schönen Tag!";
  return s;
}

std::string Voice::preview(const dash::Dashboard &snap) const {
  std::string out;
  out += "Phrases (Alexa routine names):\n";
  for (const auto &[phrase, cmd] : kPhrases) out += "  - \"" + phrase + "\"\n";
  out += "\n[guten morgen]\n" + briefing(snap) + "\n";
  out += "\n[serverstatus]\n" + server_status(snap) + "\n";
  out += "\n[was steht an]\n" + tasks_today(snap) + "\n";
  out += "\n[wie habe ich geschlafen]\n" + sleep_report(snap) + "\n";
  return out;
}
