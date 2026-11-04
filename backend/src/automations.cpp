#include "automations.h"

Automations::Automations(const Config &cfg, HomeAssistant &ha, PrefsStore &prefs,
                         AutomationBus &bus)
    : cfg_(cfg), ha_(ha), prefs_(prefs), bus_(bus) {}

void Automations::tick(const dashboard_backend::Dashboard &,
                       const std::map<std::string, dashboard_backend::Activity> &) {
  // Coming in the next commit (notifications, morning push, presence, sleep pattern).
}
