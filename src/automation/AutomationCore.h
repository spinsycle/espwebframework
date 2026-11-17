#pragma once
// The automation engine itself: rules in, device commands out. Pure C++ (no Arduino): everything it needs
// from the outside (device values, commands, the clock, Wi-Fi) comes through AutoEnv, so the host tests can
// drive it with a fake. Non-blocking: tick() does a small bounded amount of work and returns.
#include <math.h>
#include <new>

#include "AutomationRules.h"

namespace espwf {

struct ClockInfo {
  uint16_t minuteOfDay = 0;  // local time
  uint8_t weekday = 0;       // 0 = Sunday
  uint32_t minuteKey = 0;    // local minutes since 1970: changes once a minute
};

// Local time from UTC seconds and the configured offset. False while the clock has never been set
// (before NTP answers the system time is near 1970).
inline bool localClock(uint32_t epochUtc, int16_t offsetMin, ClockInfo& ci) {
  if (epochUtc < 1600000000UL) return false;
  int64_t local = int64_t(epochUtc) + int64_t(offsetMin) * 60;
  if (local < 0) return false;
  uint32_t days = uint32_t(local / 86400);
  ci.weekday = uint8_t((days + 4) % 7);  // 1970-01-01 was a Thursday
  ci.minuteOfDay = uint16_t((local % 86400) / 60);
  ci.minuteKey = uint32_t(local / 60);
  return true;
}

enum class EvKind : uint8_t { Fired, Failed, Blocked, Unconfirmed, Storm, Custom };
inline const char* evKindName(EvKind k) {
  static const char* const n[] = {"fired", "failed", "blocked", "unconfirmed", "storm", "event"};
  return n[uint8_t(k)];
}

enum class Last : uint8_t { None, Ok, Rejected, Blocked, NoDevice, Cooldown, Storm, ConditionsFalse };
inline const char* lastName(Last l) {
  static const char* const n[] = {"none", "ok", "rejected", "blocked", "no_device", "cooldown", "storm", "conditions_false"};
  return n[uint8_t(l)];
}

class AutoEnv {
 public:
  virtual ~AutoEnv() {}
  virtual bool read(const char* dev, Prop p, double& v) = 0;  // false: no such device, or no reading right now
  virtual CmdErr command(const char* dev, const Command& c) = 0;
  virtual bool deviceType(const char* dev, DeviceType& t) = 0;
  virtual bool clock(ClockInfo& ci) = 0;                      // false: no clock yet
  virtual bool wifiUp() = 0;
  virtual void event(const char*, EvKind, const char*) {}
};

struct RuleState {
  bool lastCond = false;
  bool havePrev = false;
  bool everFired = false;
  bool startupDone = false;
  bool holding[kMaxConds] = {false};
  int8_t lastWifi = -1;
  float prev = 0;
  uint32_t lastFire = 0;
  uint32_t nextAt = 0;
  uint32_t lastMinuteKey = 0;
  uint32_t since[kMaxConds] = {0};
  uint16_t fired = 0, failed = 0, unconfirmed = 0;
  Last last = Last::None;
};

struct EventRec {
  uint32_t ms = 0;
  char rule[kIdCap] = {0};
  EvKind kind = EvKind::Fired;
  char text[kActTextCap] = {0};
};

class AutomationCore {
 public:
  static constexpr uint32_t kTickMs = 100;       // rules are looked at ten times a second
  static constexpr uint32_t kMinGapMs = 200;     // a rule never fires twice within this
  static constexpr uint32_t kStormMax = 30;      // firings the whole engine allows per second
  static constexpr uint32_t kVerifyMs = 1500;    // how long a device has to show the commanded value
  static constexpr uint32_t kStartupMs = 3000;   // "startup" rules fire this long after boot
  static constexpr size_t kEvents = 8;
  static constexpr size_t kVerifies = 8;

  AutoSettings settings;

  ~AutomationCore() { delete[] rules_; delete[] st_; }

  size_t count() const { return n_; }
  const Rule& rule(size_t i) const { return rules_[i]; }
  const RuleState& state(size_t i) const { return st_[i]; }
  int find(const char* id) const {
    for (size_t i = 0; i < n_; i++)
      if (!strcmp(rules_[i].id, id)) return int(i);
    return -1;
  }

  // ---- changing the rule list (the caller has validated the rule)

  AutoErr add(const Rule& r) {
    if (find(r.id) >= 0) return AutoErr::Duplicate;
    if (n_ >= kMaxRules) return AutoErr::TooMany;
    if (!grow(n_ + 1)) return AutoErr::TooMany;
    rules_[n_] = r;
    st_[n_] = RuleState();
    st_[n_].startupDone = started_;  // only rules present at boot get the startup event
    n_++;
    return AutoErr::Ok;
  }

  AutoErr replace(const Rule& r) {
    int i = find(r.id);
    if (i < 0) return AutoErr::NotFound;
    rules_[i] = r;
    bool done = st_[i].startupDone;
    st_[i] = RuleState();
    st_[i].startupDone = done;
    dropVerifies(i);
    return AutoErr::Ok;
  }

  bool remove(const char* id) {
    int i = find(id);
    if (i < 0) return false;
    dropVerifies(i);
    for (size_t k = size_t(i) + 1; k < n_; k++) { rules_[k - 1] = rules_[k]; st_[k - 1] = st_[k]; }
    n_--;
    for (Verify& v : ver_)  // rule indexes above the removed one moved down
      if (v.rule > i) v.rule--;
    return true;
  }

  bool setEnabled(const char* id, bool on) {
    int i = find(id);
    if (i < 0) return false;
    rules_[i].enabled = on;
    st_[i].lastCond = false;
    for (size_t k = 0; k < kMaxConds; k++) st_[i].holding[k] = false;
    return true;
  }

  // Ask for a rule to run at the next tick (conditions and cooldown still apply).
  bool requestRun(const char* id) {
    if (find(id) < 0 || strlen(id) >= kIdCap) return false;
    strcpy(runId_, id);
    return true;
  }

  // ---- the event log, newest first

  size_t eventCount() const { return evN_; }
  const EventRec& eventAt(size_t i) const { return ev_[(evHead_ + kEvents - 1 - i) % kEvents]; }
  uint32_t firingsThisSecond() const { return winCount_; }

  // ---- running

  void tick(AutoEnv& env, uint32_t now) {
    if (started_ && uint32_t(now - lastTick_) < kTickMs) return;
    started_ = true;
    lastTick_ = now;
    if (!settings.enabled) { runId_[0] = 0; return; }  // a "run" asked for while the engine is off is dropped, not saved up
    ClockInfo ci;
    bool haveClock = env.clock(ci);
    checkVerifies(env, now);

    if (runId_[0]) {
      int i = find(runId_);
      runId_[0] = 0;
      if (i >= 0 && rules_[i].enabled) {
        if (conds(size_t(i), env, now, ci, haveClock)) fire(size_t(i), env, now);
        else st_[i].last = Last::ConditionsFalse;
      }
    }

    for (size_t i = 0; i < n_; i++) {
      const Rule& r = rules_[i];
      RuleState& s = st_[i];
      if (!r.enabled) continue;
      switch (r.trig.type) {
        case TrigType::Change: {
          double v;
          if (!env.read(r.trig.device, r.trig.prop, v)) break;
          if (!s.havePrev) { s.havePrev = true; s.prev = float(v); break; }
          if (float(v) != s.prev) {
            s.prev = float(v);
            if (conds(i, env, now, ci, haveClock)) fire(i, env, now);
            else s.last = Last::ConditionsFalse;
          }
          break;
        }
        case TrigType::When: {  // fires once each time its conditions go from false to true
          bool c = conds(i, env, now, ci, haveClock);
          if (c && !s.lastCond) fire(i, env, now);
          s.lastCond = c;
          break;
        }
        case TrigType::Interval: {
          uint32_t every = r.trig.everyS * 1000UL;
          if (!every) break;  // validation forbids 0; never spin on it
          if (!s.nextAt) { s.nextAt = now + every; break; }
          if (int32_t(now - s.nextAt) >= 0) {
            s.nextAt += every;
            if (int32_t(now - s.nextAt) >= 0) s.nextAt = now + every;  // fell far behind: do not burst
            if (conds(i, env, now, ci, haveClock)) fire(i, env, now);
            else s.last = Last::ConditionsFalse;
          }
          break;
        }
        case TrigType::Schedule:
          if (haveClock && ci.minuteOfDay == r.trig.minute && ((r.trig.days >> ci.weekday) & 1) && ci.minuteKey != s.lastMinuteKey) {
            s.lastMinuteKey = ci.minuteKey;
            if (conds(i, env, now, ci, haveClock)) fire(i, env, now);
            else s.last = Last::ConditionsFalse;
          }
          break;
        case TrigType::Startup:
          if (!s.startupDone && now >= kStartupMs) {
            s.startupDone = true;
            if (conds(i, env, now, ci, haveClock)) fire(i, env, now);
            else s.last = Last::ConditionsFalse;
          }
          break;
        case TrigType::Wifi: {
          int8_t cur = env.wifiUp() ? 1 : 0;
          if (s.lastWifi < 0) { s.lastWifi = cur; break; }
          if (cur != s.lastWifi) {
            s.lastWifi = cur;
            if ((cur == 1) == r.trig.wifiUp) {
              if (conds(i, env, now, ci, haveClock)) fire(i, env, now);
              else s.last = Last::ConditionsFalse;
            }
          }
          break;
        }
        default: break;  // manual: only requestRun
      }
    }
  }

 private:
  struct Verify {
    int16_t rule = -1;  // -1 = free
    char dev[kIdCap] = {0};
    Prop prop = Prop::None;
    float expect = 0;
    uint32_t deadline = 0;
  };

  Rule* rules_ = nullptr;
  RuleState* st_ = nullptr;
  size_t n_ = 0;
  bool started_ = false;
  uint32_t lastTick_ = 0;
  uint32_t winStart_ = 0, winCount_ = 0;
  char runId_[kIdCap] = {0};
  EventRec ev_[kEvents];
  size_t evHead_ = 0, evN_ = 0;
  Verify ver_[kVerifies];

  bool grow(size_t want) {
    Rule* nr = new (std::nothrow) Rule[want];
    RuleState* ns = new (std::nothrow) RuleState[want];
    if (!nr || !ns) { delete[] nr; delete[] ns; return false; }
    for (size_t i = 0; i < n_; i++) { nr[i] = rules_[i]; ns[i] = st_[i]; }
    delete[] rules_;
    delete[] st_;
    rules_ = nr;
    st_ = ns;
    return true;
  }

  void dropVerifies(int rule) {
    for (Verify& v : ver_)
      if (v.rule == rule) v.rule = -1;
  }

  void log(AutoEnv& env, uint32_t now, const char* rule, EvKind k, const char* text) {
    EventRec& e = ev_[evHead_];
    e.ms = now;
    copyStr(e.rule, rule, sizeof e.rule);
    e.kind = k;
    copyStr(e.text, text ? text : "", sizeof e.text);
    evHead_ = (evHead_ + 1) % kEvents;
    if (evN_ < kEvents) evN_++;
    env.event(rule, k, text);
  }

  // One condition. A "for" duration is measured across ticks (see the "when" trigger); a manual run uses what has been
  // measured so far, and for any other trigger it can never be met, which validateCond already forbids.
  bool cond(size_t ri, size_t k, AutoEnv& env, uint32_t now, const ClockInfo& ci, bool haveClock) {
    const Cond& c = rules_[ri].conds[k];
    RuleState& s = st_[ri];
    if (c.kind == CondKind::Time) {
      if (!haveClock || !((c.days >> ci.weekday) & 1)) return false;
      uint16_t m = ci.minuteOfDay;
      return c.from < c.to ? (m >= c.from && m < c.to) : (m >= c.from || m < c.to);  // from > to wraps midnight
    }
    double v;
    bool ok = env.read(c.device, c.prop, v) && compare(c.op, v, double(c.value));
    if (c.forS == 0) return ok;
    if (!ok) { s.holding[k] = false; return false; }
    if (!s.holding[k]) { s.holding[k] = true; s.since[k] = now; }
    return uint32_t(now - s.since[k]) >= c.forS * 1000UL;
  }

  bool conds(size_t ri, AutoEnv& env, uint32_t now, const ClockInfo& ci, bool haveClock) {
    const Rule& r = rules_[ri];
    if (r.nConds == 0) return true;
    bool any = false, all = true;
    for (size_t k = 0; k < r.nConds; k++) {  // every condition is evaluated: duration timers must keep running
      bool v = cond(ri, k, env, now, ci, haveClock);
      any = any || v;
      all = all && v;
    }
    return r.matchAny ? any : all;
  }

  bool blockedByLock(AutoEnv& env, const Action& a) {
    bool turnsOn = a.type == ActType::On;
    double v;
    if (a.type == ActType::Toggle) turnsOn = env.read(a.device, Prop::State, v) && v < 0.5;
    if (!turnsOn) return false;
    for (size_t i = 0; i < settings.nLocks; i++) {
      const Interlock& l = settings.locks[i];
      const char* other = !strcmp(a.device, l.a) ? l.b : !strcmp(a.device, l.b) ? l.a : nullptr;
      if (other && env.read(other, Prop::State, v) && v > 0.5) return true;
    }
    return false;
  }

  void addVerify(size_t ri, const char* dev, Prop p, float expect, uint32_t now) {
    for (Verify& v : ver_)
      if (v.rule < 0) {
        v.rule = int16_t(ri);
        copyStr(v.dev, dev, sizeof v.dev);
        v.prop = p;
        v.expect = expect;
        v.deadline = now + kVerifyMs;
        return;
      }
  }

  // "Triggered" is not "done": a command that was accepted is checked against the device a moment later.
  void checkVerifies(AutoEnv& env, uint32_t now) {
    for (Verify& v : ver_) {
      if (v.rule < 0 || int32_t(now - v.deadline) < 0) continue;
      double x;
      bool ok = env.read(v.dev, v.prop, x) && fabs(x - double(v.expect)) < 0.5;
      if (!ok && size_t(v.rule) < n_) {
        st_[v.rule].unconfirmed++;
        log(env, now, rules_[v.rule].id, EvKind::Unconfirmed, v.dev);
      }
      v.rule = -1;
    }
  }

  void fire(size_t ri, AutoEnv& env, uint32_t now) {
    const Rule& r = rules_[ri];
    RuleState& s = st_[ri];
    if (s.everFired) {
      uint32_t since = uint32_t(now - s.lastFire);
      if (since < kMinGapMs || (r.cooldownS && since < r.cooldownS * 1000UL)) { s.last = Last::Cooldown; return; }
    }
    if (uint32_t(now - winStart_) >= 1000) { winStart_ = now; winCount_ = 0; }
    if (winCount_ >= kStormMax) {  // rules triggering each other in a loop: stop feeding it
      s.last = Last::Storm;
      log(env, now, r.id, EvKind::Storm, "");
      return;
    }
    winCount_++;
    s.lastFire = now;
    s.everFired = true;
    s.fired++;
    s.last = Last::Ok;
    log(env, now, r.id, EvKind::Fired, "");

    for (size_t k = 0; k < r.nActs; k++) {
      const Action& a = r.acts[k];
      if (a.type == ActType::Event) { log(env, now, r.id, EvKind::Custom, a.text); continue; }
      DeviceType t;
      if (!env.deviceType(a.device, t)) { s.failed++; s.last = Last::NoDevice; log(env, now, r.id, EvKind::Failed, a.device); continue; }
      if (blockedByLock(env, a)) { s.failed++; s.last = Last::Blocked; log(env, now, r.id, EvKind::Blocked, a.device); continue; }
      Command c;
      if (!buildCommand(a, t, c) || env.command(a.device, c) != CmdErr::Ok) {
        s.failed++;
        s.last = Last::Rejected;
        log(env, now, r.id, EvKind::Failed, a.device);
        continue;
      }
      if (a.type == ActType::On) addVerify(ri, a.device, Prop::State, 1, now);
      else if (a.type == ActType::Off) addVerify(ri, a.device, Prop::State, 0, now);
      else if (a.type == ActType::Set) addVerify(ri, a.device, Prop::Level, a.value, now);
      else if (a.type == ActType::Stop) addVerify(ri, a.device, Prop::Level, 0, now);
    }
  }
};

}  // namespace espwf
