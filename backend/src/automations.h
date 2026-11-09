#pragma once

// Worker-side automations: notifications via the Home Assistant companion app
// (lights left on, server problems), morning briefing push, presence actions
// from the iPhone (arrive / leave), and the learned sleep pattern experiment
// (adaptive wake light, bedtime reminder). Worker thread only.
//
// "Already sent" state lives in DATA_DIR/automations_state.json so a restart
// doesn't send anything twice. Every action is logged as "[Auto] ...".

#include "automation_bus.h"
#include "config.h"
#include "dashboard.pb.h"
#include "home_assistant.h"
#include "prefs.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Typical bedtime / wake time learned from HealthKit sleep samples.
struct SleepPattern {
  struct Kind {
    std::optional<int> bedtime_min; // minutes after local midnight (0..1439)
    std::optional<int> wake_min;
    int nights = 0;
  };
  Kind weekday; // nights before Mon-Fri mornings
  Kind weekend; // nights before Sat/Sun mornings
  int64_t updated_at = 0;
  nlohmann::json to_json() const;
};

// Computes the pattern from collect.db (opened read-only). `now` = Unix ms.
// Nights shorter than 3 h are ignored; per night the source with the most
// sleep wins (like Collector::summary). Returns nullopt if the DB can't be read.
std::optional<SleepPattern> compute_sleep_pattern(const std::string &db_file,
                                                  const std::string &user, int64_t now,
                                                  std::string &error);

class Automations {
public:
  Automations(const Config &cfg, HomeAssistant &ha, PrefsStore &prefs, AutomationBus &bus);

  // Call after every snapshot. `activity` = latest HomeCollect summary per user.
  void tick(const dashboard_backend::Dashboard &snap,
            const std::map<std::string, dashboard_backend::Activity> &activity);

  // Actionable notification button pressed on the phone (HA event
  // mobile_app_notification_action) -> command for the worker, if it is ours.
  static std::optional<dashboard_backend::Command> command_for_action(const std::string &action);

  // `dashboard_backend --sleep-pattern [user]`: computed pattern + what the
  // automations would do today. Reads only, never acts.
  std::string dry_run(const dashboard_backend::Dashboard &snap, const std::string &user);

  // All automations for the overview page: key (as in the history), pref
  // (Prefs JSON key of its switch, or null), name/when/does in de + en.
  // Keep in sync when adding an automation.
  static nlohmann::json catalog();

  // Owner: OWNER_USER, else COLLECT_DASHBOARD_USER, else first admin.
  std::string owner();

private:
  // "[Auto] ..." to stdout and into the automation history (overview page).
  // Use like a stream: alog("homework") << "reminder: " << n << " due";
  class Log {
  public:
    Log(AutomationBus &bus, const char *key) : bus_(bus), key_(key) {}
    Log(const Log &) = delete;
    template <class T> Log &operator<<(const T &v) {
      os_ << v;
      return *this;
    }
    ~Log();

  private:
    AutomationBus &bus_;
    const char *key_;
    std::ostringstream os_;
  };
  Log alog(const char *key) { return Log(bus_, key); }
  // Publishes presence/work state and Home Assistant's own automations for
  // the overview page (bus status).
  void publish_status(int64_t now);
  int64_t ha_automations_at_ = 0;

  int64_t now() const;
  bool notify(const Prefs &p, const std::string &title, const std::string &message,
              const nlohmann::json &data);
  bool ha_reachable(int64_t now);

  void lights_left_on(const dashboard_backend::Dashboard &snap, const Prefs &p, int64_t now);
  void server_issues(const dashboard_backend::Dashboard &snap, const Prefs &p, int64_t now,
                     bool reachable);
  void morning(const dashboard_backend::Dashboard &snap, const dashboard_backend::Activity *act,
               const Prefs &p, int64_t now, bool reachable);
  void presence(const dashboard_backend::Dashboard &snap, const Prefs &p, int64_t now);
  void work_presence(const dashboard_backend::Dashboard &snap, const Prefs &p,
                     const PresenceEvent &e, int64_t now);

  // Presence extras (see automations.cpp section 3b)
  bool owner_home() const; // last accepted home state (true if unknown)
  void welcome(const dashboard_backend::Dashboard &snap, const Prefs &p, int64_t now);
  void room_alarm(const dashboard_backend::Dashboard &snap, const Prefs &p, int64_t now);
  void umbrella(const dashboard_backend::Dashboard &snap, const Prefs &p, int64_t now);
  void homework(const Prefs &p, int64_t now);
  void shopping_list(const dashboard_backend::Dashboard &snap, const Prefs &p);
  struct Homework {
    std::string school_date;                                  // YYYY-MM-DD (tomorrow)
    std::vector<std::string> due;                             // events on that day
    std::vector<std::pair<std::string, std::string>> tests;   // later tests: date, summary
  };
  // Homework calendar entries if tomorrow is a school day (nullopt: not a
  // school day, no calendar, or HA error).
  std::optional<Homework> homework_for_tomorrow(int64_t now);
  // Accepted arrive/leave -> HA: sensor.homecollect_<user>_presence (home/away/work)
  // + event "homecollect_presence" {user, event, place, source}.
  void publish_presence(const std::string &user, const std::string &event,
                        const std::string &place, const std::string &source, int64_t t);
  void sleep_pattern(const std::string &user, int64_t now);
  void wake_light(const dashboard_backend::Dashboard &snap, const dashboard_backend::Activity *act,
                  const Prefs &p, int64_t now);
  void bedtime(const dashboard_backend::Dashboard &snap, const Prefs &p, int64_t now);

  nlohmann::json briefing(const dashboard_backend::Dashboard &snap,
                          const dashboard_backend::Activity *act, const Prefs &p,
                          int64_t now) const;
  std::vector<std::string> current_issues(const dashboard_backend::Dashboard &snap,
                                          const Prefs &p) const;
  bool someone_home(const dashboard_backend::Dashboard &snap,
                    const dashboard_backend::Activity *act, int64_t now);

  void load_state();
  void save_state();

  const Config &cfg_;
  HomeAssistant &ha_;
  PrefsStore &prefs_;
  AutomationBus &bus_;

  std::string state_file_;
  nlohmann::json state_; // persisted

  std::string owner_;
  std::string owner_name_; // how to address the owner (display name)
  int64_t owner_checked_ = 0;
  bool warned_no_target_ = false;
  bool warned_unreachable_ = false;
  int64_t reach_checked_ = 0;
  bool reachable_ = false;

  // server problems: when the condition started (not persisted, needs to
  // be observed continuously anyway)
  std::map<std::string, int64_t> issue_since_;
  std::map<std::string, int64_t> issue_clear_since_;

  std::optional<SleepPattern> pattern_;
  std::string pattern_user_;
  int64_t pattern_at_ = 0;
  int64_t briefing_at_ = 0;
  int64_t focus_checked_ = 0;
  std::optional<int64_t> tracked_alarm_;
  int64_t retry_after_ = 0; // after a failed push
};
