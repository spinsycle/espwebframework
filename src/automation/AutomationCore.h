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

enum class EvKind : uint8_t { Fired, Failed, Blocked, Unconfirmed, Storm, Custom, SeqDone, SeqStopped, TimerDone };
inline const char* evKindName(EvKind k) {
  static const char* const n[] = {"fired", "failed", "blocked", "unconfirmed", "storm", "event", "sequence_done", "sequence_stopped", "timer_done"};
  return n[uint8_t(k)];
}

enum class Last : uint8_t { None, Ok, Rejected, Blocked, NoDevice, Cooldown, Storm, ConditionsFalse };
inline const char* lastName(Last l) {
  static const char* const n[] = {"none", "ok", "rejected", "blocked", "no_device", "cooldown", "storm", "conditions_false"};
  return n[uint8_t(l)];
}

class AutoEnv;

// A time-of-day window. Needs a clock; a window with from > to wraps midnight.
inline bool timeCondHolds(const Cond& c, const ClockInfo& ci, bool haveClock) {
  if (!haveClock || !((c.days >> ci.weekday) & 1)) return false;
  uint16_t m = ci.minuteOfDay;
  return c.from < c.to ? (m >= c.from && m < c.to) : (m >= c.from || m < c.to);  // from > to wraps midnight
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
  uint16_t seen = 0;  // timer / sequence triggers: the finish count already acted on
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
  static constexpr size_t kCtlSlots = 4;
  static constexpr int kStepsPerPass = 8;  // a sequence runs at most this many steps in one tick

  AutoSettings settings;

  ~AutomationCore() {
    delete[] rules_;
    delete[] st_;
    for (size_t i = 0; i < nSeq_; i++) delete[] seqs_[i].steps;
    delete[] seqs_;
    delete[] sr_;
  }

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

  // ---- sequences (steps live in exactly sized arrays; at most kMaxStepsTotal steps in all)

  enum class SeqRunState : uint8_t { Idle, Running, Paused };
  struct SeqStore {
    char id[kIdCap] = {0};
    char name[kRuleNameCap] = {0};
    uint8_t repeat = 1;
    uint8_t n = 0;
    Step* steps = nullptr;
  };
  struct SeqRun {
    SeqRunState st = SeqRunState::Idle;
    uint8_t step = 0, loop = 0;
    bool inUntil = false;
    uint32_t resumeAt = 0, deadline = 0, left = 0;
    uint16_t done = 0, stopped = 0;
  };
  struct TimerRun {
    bool running = false;
    uint32_t endAt = 0;
    uint16_t finished = 0;
  };

  size_t seqCount() const { return nSeq_; }
  const SeqStore& seq(size_t i) const { return seqs_[i]; }
  const SeqRun& seqRun(size_t i) const { return sr_[i]; }
  int findSeq(const char* id) const {
    for (size_t i = 0; i < nSeq_; i++)
      if (!strcmp(seqs_[i].id, id)) return int(i);
    return -1;
  }
  size_t stepsUsed() const {
    size_t n = 0;
    for (size_t i = 0; i < nSeq_; i++) n += seqs_[i].n;
    return n;
  }
  void seqDef(size_t i, SeqDef& out) const {
    copyStr(out.id, seqs_[i].id, sizeof out.id);
    copyStr(out.name, seqs_[i].name, sizeof out.name);
    out.repeat = seqs_[i].repeat;
    out.nSteps = seqs_[i].n;
    for (size_t k = 0; k < seqs_[i].n; k++) out.steps[k] = seqs_[i].steps[k];
  }

  AutoErr addSeq(const SeqDef& d) {
    if (findSeq(d.id) >= 0) return AutoErr::Duplicate;
    if (nSeq_ >= kMaxSeqs || stepsUsed() + d.nSteps > kMaxStepsTotal) return AutoErr::TooMany;
    Step* steps = new (std::nothrow) Step[d.nSteps ? d.nSteps : 1];
    SeqStore* ns = new (std::nothrow) SeqStore[nSeq_ + 1];
    SeqRun* nr = new (std::nothrow) SeqRun[nSeq_ + 1];
    if (!steps || !ns || !nr) { delete[] steps; delete[] ns; delete[] nr; return AutoErr::TooMany; }
    for (size_t k = 0; k < d.nSteps; k++) steps[k] = d.steps[k];
    for (size_t i = 0; i < nSeq_; i++) { ns[i] = seqs_[i]; nr[i] = sr_[i]; }  // the steps move by pointer
    copyStr(ns[nSeq_].id, d.id, kIdCap);
    copyStr(ns[nSeq_].name, d.name, kRuleNameCap);
    ns[nSeq_].repeat = d.repeat;
    ns[nSeq_].n = d.nSteps;
    ns[nSeq_].steps = steps;
    delete[] seqs_;
    delete[] sr_;
    seqs_ = ns;
    sr_ = nr;
    nSeq_++;
    return AutoErr::Ok;
  }

  AutoErr replaceSeq(const SeqDef& d) {
    int i = findSeq(d.id);
    if (i < 0) return AutoErr::NotFound;
    if (stepsUsed() - seqs_[i].n + d.nSteps > kMaxStepsTotal) return AutoErr::TooMany;
    Step* steps = new (std::nothrow) Step[d.nSteps ? d.nSteps : 1];
    if (!steps) return AutoErr::TooMany;
    for (size_t k = 0; k < d.nSteps; k++) steps[k] = d.steps[k];
    delete[] seqs_[i].steps;
    copyStr(seqs_[i].name, d.name, kRuleNameCap);
    seqs_[i].repeat = d.repeat;
    seqs_[i].n = d.nSteps;
    seqs_[i].steps = steps;
    sr_[i].st = SeqRunState::Idle;  // an edited sequence starts over the next time it is started
    sr_[i].inUntil = false;
    return AutoErr::Ok;
  }

  bool removeSeq(const char* id) {
    int i = findSeq(id);
    if (i < 0) return false;
    delete[] seqs_[i].steps;
    seqs_[i].steps = nullptr;
    for (size_t k = size_t(i) + 1; k < nSeq_; k++) { seqs_[k - 1] = seqs_[k]; sr_[k - 1] = sr_[k]; }
    nSeq_--;
    return true;
  }

  // ---- timers

  size_t timerCount() const { return nTimer_; }
  const TimerDef& timer(size_t i) const { return timers_[i]; }
  const TimerRun& timerRun(size_t i) const { return tr_[i]; }
  int findTimer(const char* id) const {
    for (size_t i = 0; i < nTimer_; i++)
      if (!strcmp(timers_[i].id, id)) return int(i);
    return -1;
  }
  uint32_t timerRemainingMs(size_t i, uint32_t now) const {
    return tr_[i].running && int32_t(tr_[i].endAt - now) > 0 ? tr_[i].endAt - now : 0;
  }
  AutoErr addTimer(const TimerDef& t) {
    if (findTimer(t.id) >= 0) return AutoErr::Duplicate;
    if (nTimer_ >= kMaxTimers) return AutoErr::TooMany;
    timers_[nTimer_] = t;
    tr_[nTimer_] = TimerRun();
    nTimer_++;
    return AutoErr::Ok;
  }
  AutoErr replaceTimer(const TimerDef& t) {
    int i = findTimer(t.id);
    if (i < 0) return AutoErr::NotFound;
    timers_[i] = t;
    tr_[i] = TimerRun();  // an edited timer is stopped
    return AutoErr::Ok;
  }
  bool removeTimer(const char* id) {
    int i = findTimer(id);
    if (i < 0) return false;
    for (size_t k = size_t(i) + 1; k < nTimer_; k++) { timers_[k - 1] = timers_[k]; tr_[k - 1] = tr_[k]; }
    nTimer_--;
    return true;
  }

  // ---- starting and stopping sequences and timers from outside (done at the next tick)

  enum class Ctl : uint8_t { SeqStart, SeqStop, SeqPause, SeqResume, TimerStart, TimerStop };
  bool requestCtl(Ctl op, const char* id) {
    bool seq = op == Ctl::SeqStart || op == Ctl::SeqStop || op == Ctl::SeqPause || op == Ctl::SeqResume;
    if ((seq ? findSeq(id) : findTimer(id)) < 0 || strlen(id) >= kIdCap) return false;
    if (nCtl_ >= kCtlSlots) return false;
    ctl_[nCtl_].op = op;
    copyStr(ctl_[nCtl_].id, id, kIdCap);
    nCtl_++;
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

    for (size_t k = 0; k < nCtl_; k++) ctlNow(ctl_[k].op, ctl_[k].id, env, now);
    nCtl_ = 0;
    tickTimers(env, now);
    tickSeqs(env, now, ci, haveClock);

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
        case TrigType::Timer:
        case TrigType::Sequence: {  // fires each time the timer finishes / the sequence finishes (or is stopped early)
          bool isTimer = r.trig.type == TrigType::Timer;
          int k = isTimer ? findTimer(r.trig.device) : findSeq(r.trig.device);
          if (k < 0) break;
          uint16_t count = isTimer ? tr_[k].finished : (r.trig.onStopped ? sr_[k].stopped : sr_[k].done);
          if (!s.havePrev) { s.havePrev = true; s.seen = count; break; }
          if (count != s.seen) {
            s.seen = count;
            if (conds(i, env, now, ci, haveClock)) fire(i, env, now);
            else s.last = Last::ConditionsFalse;
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

  SeqStore* seqs_ = nullptr;
  SeqRun* sr_ = nullptr;
  size_t nSeq_ = 0;
  TimerDef timers_[kMaxTimers];
  TimerRun tr_[kMaxTimers];
  size_t nTimer_ = 0;
  struct CtlReq { Ctl op = Ctl::SeqStart; char id[kIdCap] = {0}; };
  CtlReq ctl_[kCtlSlots];
  size_t nCtl_ = 0;

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
    if (c.kind == CondKind::Time) return timeCondHolds(c, ci, haveClock);
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
      if (a.type == ActType::SeqStart || a.type == ActType::SeqStop || a.type == ActType::TimerStart || a.type == ActType::TimerStop) {
        if (!control(a.type, a.device, env, now)) { s.failed++; s.last = Last::NoDevice; log(env, now, r.id, EvKind::Failed, a.device); }
        continue;
      }
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

  // ---- sequences and timers

  bool control(ActType a, const char* id, AutoEnv& env, uint32_t now) {
    switch (a) {
      case ActType::SeqStart: return ctlNow(Ctl::SeqStart, id, env, now);
      case ActType::SeqStop: return ctlNow(Ctl::SeqStop, id, env, now);
      case ActType::TimerStart: return ctlNow(Ctl::TimerStart, id, env, now);
      case ActType::TimerStop: return ctlNow(Ctl::TimerStop, id, env, now);
      default: return false;
    }
  }

  void stopSeq(size_t i, AutoEnv& env, uint32_t now, const char* why) {
    sr_[i].st = SeqRunState::Idle;
    sr_[i].inUntil = false;
    sr_[i].stopped++;
    log(env, now, seqs_[i].id, EvKind::SeqStopped, why);
  }

  bool ctlNow(Ctl op, const char* id, AutoEnv& env, uint32_t now) {
    if (op == Ctl::TimerStart || op == Ctl::TimerStop) {
      int t = findTimer(id);
      if (t < 0) return false;
      tr_[t].running = op == Ctl::TimerStart;
      if (tr_[t].running) tr_[t].endAt = now + timers_[t].durationS * 1000UL;
      return true;
    }
    int i = findSeq(id);
    if (i < 0) return false;
    SeqRun& r = sr_[i];
    switch (op) {
      case Ctl::SeqStart:
        if (r.st != SeqRunState::Idle) return true;  // already going
        {
          uint16_t done = r.done, stopped = r.stopped;  // the counters survive a restart
          r = SeqRun();
          r.done = done;
          r.stopped = stopped;
        }
        r.st = SeqRunState::Running;
        r.resumeAt = now;
        log(env, now, seqs_[i].id, EvKind::Fired, "");
        break;
      case Ctl::SeqStop:
        if (r.st != SeqRunState::Idle) stopSeq(size_t(i), env, now, "stopped");
        break;
      case Ctl::SeqPause:
        if (r.st == SeqRunState::Running) {
          uint32_t until = r.inUntil ? r.deadline : r.resumeAt;
          r.left = int32_t(until - now) > 0 ? until - now : 0;
          r.st = SeqRunState::Paused;
        }
        break;
      case Ctl::SeqResume:
        if (r.st == SeqRunState::Paused) {
          r.st = SeqRunState::Running;
          if (r.inUntil) r.deadline = now + r.left;
          else r.resumeAt = now + r.left;
        }
        break;
      default: break;
    }
    return true;
  }

  void tickTimers(AutoEnv& env, uint32_t now) {
    for (size_t i = 0; i < nTimer_; i++) {
      TimerRun& t = tr_[i];
      if (!t.running || int32_t(now - t.endAt) < 0) continue;
      t.finished++;
      log(env, now, timers_[i].id, EvKind::TimerDone, "");
      uint32_t d = timers_[i].durationS * 1000UL;
      if (!timers_[i].repeat) { t.running = false; continue; }
      t.endAt += d;
      if (int32_t(now - t.endAt) >= 0) t.endAt = now + d;  // far behind: do not burst
    }
  }

  // A condition in a sequence step: no duration, no memory.
  bool condNow(const Cond& c, AutoEnv& env, const ClockInfo& ci, bool haveClock) {
    if (c.kind == CondKind::Time) return timeCondHolds(c, ci, haveClock);
    double v;
    return env.read(c.device, c.prop, v) && compare(c.op, v, double(c.value));
  }

  // The action of a "do" step. Sequence steps are not verified afterwards: wait for the result with an "until" step.
  void stepAction(AutoEnv& env, const char* owner, const Action& a, uint32_t now) {
    if (a.type == ActType::Event) { log(env, now, owner, EvKind::Custom, a.text); return; }
    if (a.type == ActType::TimerStart || a.type == ActType::TimerStop) {
      if (!control(a.type, a.device, env, now)) log(env, now, owner, EvKind::Failed, a.device);
      return;
    }
    DeviceType t;
    if (!env.deviceType(a.device, t)) { log(env, now, owner, EvKind::Failed, a.device); return; }
    if (blockedByLock(env, a)) { log(env, now, owner, EvKind::Blocked, a.device); return; }
    Command c;
    if (!buildCommand(a, t, c) || env.command(a.device, c) != CmdErr::Ok) log(env, now, owner, EvKind::Failed, a.device);
  }

  // Runs what is due in one sequence. True = the next step may run in this same pass.
  bool stepSeq(size_t i, AutoEnv& env, uint32_t now, const ClockInfo& ci, bool haveClock) {
    SeqStore& q = seqs_[i];
    SeqRun& r = sr_[i];
    if (!r.inUntil && int32_t(now - r.resumeAt) < 0) return false;  // still waiting
    if (r.step >= q.n) {                                              // end of a pass through the steps
      r.loop++;
      if (q.repeat == 0 || r.loop < q.repeat) {
        r.step = 0;
        r.resumeAt = now + kTickMs;
        return false;
      }
      r.st = SeqRunState::Idle;
      r.done++;
      log(env, now, q.id, EvKind::SeqDone, "");
      return false;
    }
    const Step& s = q.steps[r.step];
    switch (s.type) {
      case StepType::Do:
        if (s.hasGuard && !condNow(s.cond, env, ci, haveClock)) {
          if (s.stopOnFalse) { stopSeq(i, env, now, "guard"); return false; }
          r.step++;  // a false guard skips the step
          return true;
        }
        stepAction(env, q.id, s.act, now);
        r.step++;
        return true;
      case StepType::Wait:
        r.step++;
        r.resumeAt = now + s.secs * 1000UL;
        return false;
      case StepType::Until:
        if (condNow(s.cond, env, ci, haveClock)) { r.inUntil = false; r.step++; return true; }
        if (!r.inUntil) { r.inUntil = true; r.deadline = now + s.secs * 1000UL; return false; }
        if (int32_t(now - r.deadline) >= 0) {
          r.inUntil = false;
          log(env, now, q.id, EvKind::Failed, "timeout");
          if (s.stopOnTimeout) { stopSeq(i, env, now, "timeout"); return false; }
          r.step++;
          return true;
        }
        return false;
      default:
        r.step++;
        return true;
    }
  }

  void tickSeqs(AutoEnv& env, uint32_t now, const ClockInfo& ci, bool haveClock) {
    for (size_t i = 0; i < nSeq_; i++)
      for (int k = 0; k < kStepsPerPass && sr_[i].st == SeqRunState::Running; k++)
        if (!stepSeq(i, env, now, ci, haveClock)) break;
  }
};

}  // namespace espwf
