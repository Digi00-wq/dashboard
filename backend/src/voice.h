#pragma once

#include "config.h"
#include "dashboard.pb.h"
#include "home_assistant.h"

#include <cstdint>
#include <optional>
#include <string>

// Voice assistant on top of Alexa Media Player:
//  * reacts to Alexa commands ("Alexa, Licht an") by reading the Echo's
//    last_called_summary and answering through the Echo (Polly voice),
//  * morning briefing a few minutes after the Echo alarm rang,
//  * optional wake-up light that fades in before the alarm.
// Worker thread only.
class Voice {
public:
  Voice(const Config &cfg, HomeAssistant &ha);
  // Call after every snapshot; cheap when nothing happens.
  void tick(const dashboard_backend::Dashboard &snap);
  // All answers as text, without speaking (dashboard_backend --voice-preview).
  std::string preview(const dashboard_backend::Dashboard &snap) const;

private:
  bool handle(const std::string &phrase, const dashboard_backend::Dashboard &snap);
  void say(const std::string &text);
  void alarms(const dashboard_backend::Dashboard &snap, int64_t now);

  std::string briefing(const dashboard_backend::Dashboard &snap) const;
  std::string server_status(const dashboard_backend::Dashboard &snap) const;
  std::string tasks_today(const dashboard_backend::Dashboard &snap) const;
  std::string sleep_report(const dashboard_backend::Dashboard &snap) const;
  std::string weather_report(const dashboard_backend::Dashboard &snap) const;
  // Owner's display name (users.json, re-read every 10 min), else VOICE_USER.
  std::string listener() const;

  const Config &cfg_;
  HomeAssistant &ha_;
  int64_t last_ts_ = -1;                 // last handled utterance
  std::optional<int64_t> tracked_alarm_; // alarm we are waiting for (Unix ms)
  bool wake_light_done_ = false;
  mutable std::string listener_;
  mutable int64_t listener_checked_ = -1;
};
