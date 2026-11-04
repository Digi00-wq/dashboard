#pragma once

// Worker-side automations: notifications (lights left on, server problems),
// morning briefing push, presence actions (arrive / leave), learned sleep
// pattern (wake light, bedtime reminder). Worker thread only.

#include "automation_bus.h"
#include "config.h"
#include "dashboard.pb.h"
#include "home_assistant.h"
#include "prefs.h"

#include <map>
#include <string>

class Automations {
public:
  Automations(const Config &cfg, HomeAssistant &ha, PrefsStore &prefs, AutomationBus &bus);

  // Call after every snapshot. `activity` = latest HomeCollect summary per user.
  void tick(const dashboard_backend::Dashboard &snap,
            const std::map<std::string, dashboard_backend::Activity> &activity);

private:
  const Config &cfg_;
  HomeAssistant &ha_;
  PrefsStore &prefs_;
  AutomationBus &bus_;
};
