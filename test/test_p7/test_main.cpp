#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "automation/AutomationCore.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

#define EQ(a, b) TEST_ASSERT_EQUAL(int(a), int(b))

// ---- devices the validators and the fake environment know about

static DeviceConfig mk(const char* id, DeviceType t, uint8_t model = 0) {
  DeviceConfig c;
  strcpy(c.id, id);
  c.type = t;
  c.model = model;
  return c;
}
static DeviceConfig gDevs[] = {
    mk("r1", DeviceType::Relay), mk("r2", DeviceType::Relay), mk("p1", DeviceType::Pwm), mk("m1", DeviceType::Motor),
    mk("s1", DeviceType::Servo), mk("t1", DeviceType::Sensor, 0), mk("in1", DeviceType::Input), mk("ser", DeviceType::Serial),
};
static bool find(void*, const char* id, DeviceConfig& out) {
  for (const DeviceConfig& d : gDevs)
    if (!strcmp(d.id, id)) { out = d; return true; }
  return false;
}

// ---- a scriptable environment for the engine

struct Sent { char dev[kIdCap]; Command cmd; };

struct Fake : AutoEnv {
  struct Val { char dev[kIdCap]; Prop p; double v; };
  Val vals[16];
  size_t nv = 0;
  Sent sent[128];
  size_t ns = 0;
  CmdErr cmdResult = CmdErr::Ok;
  bool haveClock = false;
  ClockInfo clk;
  bool wifi = false;
  char events[16][40];
  size_t ne = 0;
  bool followCommands = true;  // a commanded relay / level shows up in the readings

  void set(const char* dev, Prop p, double v) {
    for (size_t i = 0; i < nv; i++)
      if (!strcmp(vals[i].dev, dev) && vals[i].p == p) { vals[i].v = v; return; }
    strcpy(vals[nv].dev, dev); vals[nv].p = p; vals[nv].v = v; nv++;
  }
  bool read(const char* dev, Prop p, double& v) override {
    for (size_t i = 0; i < nv; i++)
      if (!strcmp(vals[i].dev, dev) && vals[i].p == p) { v = vals[i].v; return true; }
    return false;
  }
  CmdErr command(const char* dev, const Command& c) override {
    if (cmdResult != CmdErr::Ok) return cmdResult;
    strcpy(sent[ns].dev, dev); sent[ns].cmd = c; ns++;
    if (followCommands) {
      if (c.action == CmdAction::Set) set(dev, c.isBool ? Prop::State : Prop::Level, c.value);
      else if (c.action == CmdAction::Stop) set(dev, Prop::Level, 0);
      else if (c.action == CmdAction::Toggle) { double v = 0; read(dev, Prop::State, v); set(dev, Prop::State, v > 0.5 ? 0 : 1); }
    }
    return CmdErr::Ok;
  }
  bool deviceType(const char* dev, DeviceType& t) override {
    DeviceConfig d;
    if (!find(nullptr, dev, d)) return false;
    t = d.type;
    return true;
  }
  bool clock(ClockInfo& ci) override { ci = clk; return haveClock; }
  bool wifiUp() override { return wifi; }
  void event(const char* rule, EvKind k, const char* text) override {
    if (ne < 16) snprintf(events[ne++], 40, "%s:%s:%s", rule, evKindName(k), text);
  }
  size_t count(const char* dev) const {
    size_t n = 0;
    for (size_t i = 0; i < ns; i++) if (!strcmp(sent[i].dev, dev)) n++;
    return n;
  }
};

static Rule base(const char* id, TrigType t) {
  Rule r;
  strcpy(r.id, id);
  strcpy(r.name, id);
  r.trig.type = t;
  r.nActs = 1;
  r.acts[0].type = ActType::On;
  strcpy(r.acts[0].device, "r1");
  return r;
}
static Rule changeRule(const char* id, const char* dev, Prop p) {
  Rule r = base(id, TrigType::Change);
  strcpy(r.trig.device, dev);
  r.trig.prop = p;
  return r;
}
static void addCond(Rule& r, const char* dev, Prop p, Op op, float v, uint32_t forS = 0) {
  Cond& c = r.conds[r.nConds++];
  c.kind = CondKind::Compare;
  strcpy(c.device, dev);
  c.prop = p; c.op = op; c.value = v; c.forS = forS;
}
static AutoErr check(const Rule& r) { return validateRule(r, find, nullptr); }

// Runs the engine from `from` to `to` ms in 100 ms steps.
static void run(AutomationCore& core, Fake& env, uint32_t from, uint32_t to) {
  for (uint32_t t = from; t <= to; t += 100) core.tick(env, t);
}

// ---- time

void test_local_clock() {
  ClockInfo c;
  TEST_ASSERT_FALSE(localClock(0, 0, c));
  TEST_ASSERT_FALSE(localClock(1000000, 0, c));  // before NTP answers
  TEST_ASSERT_TRUE(localClock(1704067200UL, 0, c));  // Mon 2024-01-01 00:00:00 UTC
  EQ(1, c.weekday); EQ(0, c.minuteOfDay);
  TEST_ASSERT_TRUE(localClock(1704067200UL, -300, c));  // 19:00 the day before, a Sunday
  EQ(0, c.weekday); EQ(19 * 60, c.minuteOfDay);
  TEST_ASSERT_TRUE(localClock(1704067200UL + 3600 * 5 + 60 * 7, 330, c));  // +05:30
  EQ(1, c.weekday); EQ(10 * 60 + 37, c.minuteOfDay);
  ClockInfo d;
  localClock(1704067200UL + 59, 0, c);
  localClock(1704067200UL + 60, 0, d);
  EQ(1, d.minuteKey - c.minuteKey);  // the key changes exactly at the minute
  TEST_ASSERT_TRUE(localClock(1704067200UL + 86400 * 6, 0, c));
  EQ(0, c.weekday);  // six days after Monday
}

void test_compare_ops() {
  TEST_ASSERT_TRUE(compare(Op::Eq, 5, 5)); TEST_ASSERT_FALSE(compare(Op::Eq, 5, 5.1));
  TEST_ASSERT_TRUE(compare(Op::Ne, 5, 6)); TEST_ASSERT_FALSE(compare(Op::Ne, 5, 5));
  TEST_ASSERT_TRUE(compare(Op::Gt, 6, 5)); TEST_ASSERT_FALSE(compare(Op::Gt, 5, 5));
  TEST_ASSERT_TRUE(compare(Op::Lt, 4, 5)); TEST_ASSERT_FALSE(compare(Op::Lt, 5, 5));
  TEST_ASSERT_TRUE(compare(Op::Ge, 5, 5)); TEST_ASSERT_TRUE(compare(Op::Ge, 6, 5)); TEST_ASSERT_FALSE(compare(Op::Ge, 4, 5));
  TEST_ASSERT_TRUE(compare(Op::Le, 5, 5)); TEST_ASSERT_TRUE(compare(Op::Le, 4, 5)); TEST_ASSERT_FALSE(compare(Op::Le, 6, 5));
  TEST_ASSERT_FALSE(compare(Op::kCount, 1, 1));
}

void test_names_round_trip() {
  for (uint8_t i = 0; i < uint8_t(TrigType::kCount); i++) { TrigType t; TEST_ASSERT_TRUE(parseTrig(trigName(TrigType(i)), t)); EQ(i, t); }
  for (uint8_t i = 0; i < uint8_t(Op::kCount); i++) { Op o; TEST_ASSERT_TRUE(parseOp(opName(Op(i)), o)); EQ(i, o); }
  for (uint8_t i = 0; i < uint8_t(ActType::kCount); i++) { ActType a; TEST_ASSERT_TRUE(parseAct(actName(ActType(i)), a)); EQ(i, a); }
  TrigType t; Op o; ActType a;
  TEST_ASSERT_FALSE(parseTrig("sequence_done", t)); TEST_ASSERT_FALSE(parseTrig(nullptr, t));
  TEST_ASSERT_FALSE(parseOp("=>", o)); TEST_ASSERT_FALSE(parseAct("explode", a));
}

// ---- building commands

void test_build_command() {
  Command c;
  Action a;
  a.type = ActType::On;
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Relay, c));
  EQ(CmdAction::Set, c.action); TEST_ASSERT_TRUE(c.isBool); EQ(1, c.value);
  a.type = ActType::Off;
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Relay, c)); EQ(0, c.value);
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Pwm, c));        // on/off is for relays
  a.type = ActType::Toggle;
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Relay, c)); EQ(CmdAction::Toggle, c.action);
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Motor, c));
  a.type = ActType::Set; a.value = 50;
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Pwm, c));
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Motor, c));
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Servo, c));
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Relay, c));
  a.value = 101;
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Pwm, c));        // out of the pwm range
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Servo, c));       // but a fine angle
  a.value = -50;
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Motor, c));
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Pwm, c));
  a.type = ActType::Stop;
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Motor, c));
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Servo, c));
  a.type = ActType::Send; strcpy(a.text, "go");
  TEST_ASSERT_TRUE(buildCommand(a, DeviceType::Serial, c)); TEST_ASSERT_EQUAL_STRING("go", c.text);
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Relay, c));
  a.text[0] = 0;
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Serial, c));     // empty line
  a.type = ActType::Event;
  TEST_ASSERT_FALSE(buildCommand(a, DeviceType::Relay, c));      // events have no command
}

// ---- validating rules

void test_validate_basics() {
  Rule r = base("a", TrigType::Manual);
  EQ(AutoErr::Ok, check(r));
  strcpy(r.id, "Bad Id"); EQ(AutoErr::Id, check(r));
  r = base("a", TrigType::Manual); r.id[0] = 0; EQ(AutoErr::Id, check(r));
  r = base("a", TrigType::Manual); memset(r.name, 'x', kRuleNameCap); EQ(AutoErr::Name, check(r));
  r = base("a", TrigType::Manual); r.name[0] = 0; EQ(AutoErr::Ok, check(r));
  r = base("a", TrigType::Manual); r.nActs = 0; EQ(AutoErr::Actions, check(r));
  r = base("a", TrigType::Manual); r.nActs = kMaxActions + 1; EQ(AutoErr::Actions, check(r));
  r = base("a", TrigType::Manual); r.nActs = kMaxActions; for (size_t i = 0; i < kMaxActions; i++) r.acts[i] = r.acts[0]; EQ(AutoErr::Ok, check(r));
  r = base("a", TrigType::Manual); r.nConds = kMaxConds + 1; EQ(AutoErr::Conds, check(r));
  r = base("a", TrigType::Manual); r.cooldownS = 86400; EQ(AutoErr::Ok, check(r));
  r.cooldownS = 86401; EQ(AutoErr::Cooldown, check(r));
  r = base("a", TrigType::Manual); r.trig.type = TrigType::kCount; EQ(AutoErr::Trigger, check(r));
}

void test_validate_triggers() {
  Rule c = changeRule("a", "t1", Prop::Temp);
  EQ(AutoErr::Ok, check(c));
  strcpy(c.trig.device, "nope"); EQ(AutoErr::Device, check(c));
  c = changeRule("a", "t1", Prop::Level); EQ(AutoErr::Prop, check(c));      // a sensor has no level
  c = changeRule("a", "ser", Prop::Text); EQ(AutoErr::Prop, check(c));      // text cannot be compared
  c = changeRule("a", "t1", Prop::None); EQ(AutoErr::Prop, check(c));
  Rule w = base("a", TrigType::When);
  EQ(AutoErr::Conds, check(w));                                             // nothing to watch
  addCond(w, "t1", Prop::Temp, Op::Gt, 30);
  EQ(AutoErr::Ok, check(w));
  Rule i = base("a", TrigType::Interval);
  EQ(AutoErr::Time, check(i));                                              // every_s 0
  i.trig.everyS = 1; EQ(AutoErr::Ok, check(i));
  i.trig.everyS = 86400; EQ(AutoErr::Ok, check(i));
  i.trig.everyS = 86401; EQ(AutoErr::Time, check(i));
  Rule s = base("a", TrigType::Schedule);
  s.trig.minute = 1439; EQ(AutoErr::Ok, check(s));
  s.trig.minute = 1440; EQ(AutoErr::Time, check(s));
  s.trig.minute = 0; s.trig.days = 0; EQ(AutoErr::Days, check(s));
  s.trig.days = 0x80; EQ(AutoErr::Days, check(s));
  s.trig.days = 0x7F; EQ(AutoErr::Ok, check(s));
  EQ(AutoErr::Ok, check(base("a", TrigType::Startup)));
  EQ(AutoErr::Ok, check(base("a", TrigType::Wifi)));
}

void test_validate_conditions() {
  Rule r = base("a", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30);
  EQ(AutoErr::Ok, check(r));
  r.conds[0].forS = 60; EQ(AutoErr::Ok, check(r));                          // a duration is fine on a "when" rule
  r.conds[0].forS = 86401; EQ(AutoErr::Duration, check(r));
  Rule x = base("a", TrigType::Interval); x.trig.everyS = 5;
  addCond(x, "t1", Prop::Temp, Op::Gt, 30, 10);
  EQ(AutoErr::Duration, check(x));                                          // not on any other trigger
  r = base("a", TrigType::When);
  addCond(r, "nope", Prop::Temp, Op::Gt, 30); EQ(AutoErr::Device, check(r));
  r = base("a", TrigType::When); addCond(r, "t1", Prop::Humidity, Op::Gt, 30); EQ(AutoErr::Prop, check(r));
  r = base("a", TrigType::When); addCond(r, "ser", Prop::Text, Op::Eq, 0); EQ(AutoErr::Prop, check(r));
  r = base("a", TrigType::When); addCond(r, "t1", Prop::Temp, Op::kCount, 1); EQ(AutoErr::Op, check(r));
  r = base("a", TrigType::When); addCond(r, "t1", Prop::Temp, Op::Gt, 2e6f); EQ(AutoErr::Value, check(r));
  r = base("a", TrigType::When); addCond(r, "t1", Prop::Temp, Op::Gt, NAN); EQ(AutoErr::Value, check(r));
  r = base("a", TrigType::When); addCond(r, "r1", Prop::State, Op::Eq, 1); EQ(AutoErr::Ok, check(r));
  Rule t = base("a", TrigType::Manual);
  Cond& c = t.conds[t.nConds++];
  c.kind = CondKind::Time; c.from = 8 * 60; c.to = 20 * 60; c.days = 0x7F;
  EQ(AutoErr::Ok, check(t));
  c.from = c.to; EQ(AutoErr::Time, check(t));                               // an empty window
  c.from = 1440; EQ(AutoErr::Time, check(t));
  c.from = 22 * 60; c.to = 6 * 60; EQ(AutoErr::Ok, check(t));               // wraps midnight
  c.days = 0; EQ(AutoErr::Days, check(t));
}

void test_validate_actions() {
  Rule r = base("a", TrigType::Manual);
  r.acts[0].type = ActType::Set; strcpy(r.acts[0].device, "p1"); r.acts[0].value = 50;
  EQ(AutoErr::Ok, check(r));
  r.acts[0].value = 101; EQ(AutoErr::Action, check(r));
  strcpy(r.acts[0].device, "r1"); r.acts[0].value = 1; EQ(AutoErr::Action, check(r));
  strcpy(r.acts[0].device, "nope"); EQ(AutoErr::Device, check(r));
  r.acts[0].type = ActType::On; strcpy(r.acts[0].device, "p1"); EQ(AutoErr::Action, check(r));
  r.acts[0].type = ActType::Event; r.acts[0].device[0] = 0; strcpy(r.acts[0].text, "hot");
  EQ(AutoErr::Ok, check(r));                                                // an event needs no device
  r.acts[0].text[0] = 0; EQ(AutoErr::Value, check(r));
  strcpy(r.acts[0].text, "a\tb"); EQ(AutoErr::Value, check(r));             // printable only
  r.acts[0].type = ActType::Send; strcpy(r.acts[0].device, "ser"); strcpy(r.acts[0].text, "go");
  EQ(AutoErr::Ok, check(r));
  r.acts[0].text[0] = 0; EQ(AutoErr::Value, check(r));
  r.acts[0].type = ActType::kCount; EQ(AutoErr::Action, check(r));
  r = base("a", TrigType::Manual); r.acts[1].type = ActType::Stop; strcpy(r.acts[1].device, "m1"); r.nActs = 2;
  EQ(AutoErr::Ok, check(r));
  strcpy(r.acts[1].device, "s1"); EQ(AutoErr::Action, check(r));            // the second action is checked too
}

void test_validate_settings() {
  AutoSettings s;
  EQ(AutoErr::Ok, validateSettings(s, find, nullptr));
  s.utcOffsetMin = 840; EQ(AutoErr::Ok, validateSettings(s, find, nullptr));
  s.utcOffsetMin = 841; EQ(AutoErr::Offset, validateSettings(s, find, nullptr));
  s.utcOffsetMin = -720; EQ(AutoErr::Ok, validateSettings(s, find, nullptr));
  s.utcOffsetMin = -721; EQ(AutoErr::Offset, validateSettings(s, find, nullptr));
  s.utcOffsetMin = 0;
  s.nLocks = 1; strcpy(s.locks[0].a, "r1"); strcpy(s.locks[0].b, "r2");
  EQ(AutoErr::Ok, validateSettings(s, find, nullptr));
  strcpy(s.locks[0].b, "r1"); EQ(AutoErr::Interlock, validateSettings(s, find, nullptr));   // itself
  strcpy(s.locks[0].b, "p1"); EQ(AutoErr::Interlock, validateSettings(s, find, nullptr));   // not a relay
  strcpy(s.locks[0].b, "gone"); EQ(AutoErr::Interlock, validateSettings(s, find, nullptr));
  s.nLocks = kMaxInterlocks + 1; EQ(AutoErr::Interlock, validateSettings(s, find, nullptr));
}

// ---- the engine: bookkeeping

void test_core_add_remove() {
  AutomationCore core;
  EQ(0, core.count());
  EQ(AutoErr::Ok, core.add(base("a", TrigType::Manual)));
  EQ(AutoErr::Duplicate, core.add(base("a", TrigType::Manual)));
  EQ(AutoErr::Ok, core.add(base("b", TrigType::Manual)));
  EQ(2, core.count());
  EQ(1, core.find("b")); EQ(-1, core.find("zz"));
  Rule b = base("b", TrigType::Interval); b.trig.everyS = 9;
  EQ(AutoErr::Ok, core.replace(b));
  EQ(9, core.rule(1).trig.everyS);
  EQ(AutoErr::NotFound, core.replace(base("zz", TrigType::Manual)));
  TEST_ASSERT_TRUE(core.remove("a"));
  EQ(1, core.count()); EQ(0, core.find("b"));
  TEST_ASSERT_FALSE(core.remove("a"));
  for (size_t i = 0; i < kMaxRules; i++) { char id[8]; snprintf(id, sizeof id, "r%u", unsigned(i)); core.add(base(id, TrigType::Manual)); }
  EQ(kMaxRules, core.count());
  EQ(AutoErr::TooMany, core.add(base("extra", TrigType::Manual)));
}

void test_core_enable_disable() {
  AutomationCore core; Fake env;
  core.add(base("a", TrigType::Interval));                                 // every_s 0 is never accepted by the validator: it must not spin
  Rule r = base("i", TrigType::Interval); r.trig.everyS = 1;
  core.add(r);
  TEST_ASSERT_TRUE(core.setEnabled("i", false));
  TEST_ASSERT_FALSE(core.rule(1).enabled);
  run(core, env, 0, 5000);
  EQ(0, env.ns);                                                            // disabled: never runs
  TEST_ASSERT_TRUE(core.setEnabled("i", true));
  TEST_ASSERT_FALSE(core.setEnabled("zz", true));
}

// ---- triggers

void test_change_trigger() {
  AutomationCore core; Fake env;
  Rule r = changeRule("c", "t1", Prop::Temp);
  core.add(r);
  env.set("t1", Prop::Temp, 20);
  run(core, env, 0, 500);
  EQ(0, env.ns);                                                            // the first reading is not a change
  env.set("t1", Prop::Temp, 21);
  run(core, env, 600, 1000);
  EQ(1, env.ns);
  run(core, env, 1100, 3000);
  EQ(1, env.ns);                                                            // no change, no firing
  env.set("t1", Prop::Temp, 22);
  run(core, env, 3100, 3200);
  EQ(2, env.ns);
  EQ(2, core.state(0).fired);
}

void test_change_trigger_with_condition() {
  AutomationCore core; Fake env;
  Rule r = changeRule("c", "t1", Prop::Temp);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30);
  core.add(r);
  env.set("t1", Prop::Temp, 20);
  run(core, env, 0, 300);
  env.set("t1", Prop::Temp, 25);
  run(core, env, 400, 600);
  EQ(0, env.ns);
  EQ(Last::ConditionsFalse, core.state(0).last);
  env.set("t1", Prop::Temp, 31);
  run(core, env, 700, 900);
  EQ(1, env.ns);
  EQ(Last::Ok, core.state(0).last);
}

void test_change_trigger_unreadable_device() {
  AutomationCore core; Fake env;
  core.add(changeRule("c", "t1", Prop::Temp));
  run(core, env, 0, 2000);                                                  // no reading at all
  EQ(0, env.ns);
  env.set("t1", Prop::Temp, 5);
  run(core, env, 2100, 2500);
  EQ(0, env.ns);                                                            // first reading, no change yet
}

void test_when_trigger_edge() {
  AutomationCore core; Fake env;
  Rule r = base("w", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30);
  core.add(r);
  env.set("t1", Prop::Temp, 20);
  run(core, env, 0, 1000);
  EQ(0, env.ns);
  env.set("t1", Prop::Temp, 35);
  run(core, env, 1100, 5000);
  EQ(1, env.ns);                                                            // fires once, not every tick
  env.set("t1", Prop::Temp, 25);
  run(core, env, 5100, 6000);
  env.set("t1", Prop::Temp, 40);
  run(core, env, 6100, 7000);
  EQ(2, env.ns);                                                            // armed again after it went false
}

void test_when_trigger_already_true_at_start() {
  AutomationCore core; Fake env;
  Rule r = base("w", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30);
  core.add(r);
  env.set("t1", Prop::Temp, 40);
  run(core, env, 0, 500);
  EQ(1, env.ns);                                                            // true at boot counts as becoming true
}

void test_when_trigger_duration() {
  AutomationCore core; Fake env;
  Rule r = base("w", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30, 2);                              // above 30 for 2 seconds
  core.add(r);
  env.set("t1", Prop::Temp, 35);
  run(core, env, 0, 1900);
  EQ(0, env.ns);
  run(core, env, 2000, 2300);
  EQ(1, env.ns);
  env.set("t1", Prop::Temp, 20);
  run(core, env, 2400, 3000);
  env.set("t1", Prop::Temp, 35);
  run(core, env, 3100, 4900);                                               // only 1.8 s above 30
  EQ(1, env.ns);
  env.set("t1", Prop::Temp, 20);                                            // drops before the 2 s are up
  run(core, env, 5000, 5100);
  env.set("t1", Prop::Temp, 35);
  run(core, env, 5200, 6900);
  EQ(1, env.ns);                                                            // the timer restarted: 1.7 s
  run(core, env, 7000, 7400);
  EQ(2, env.ns);
}

void test_when_all_and_any() {
  for (int any = 0; any < 2; any++) {
    AutomationCore core; Fake env;
    Rule r = base("w", TrigType::When);
    r.matchAny = any;
    addCond(r, "t1", Prop::Temp, Op::Gt, 30);
    addCond(r, "r2", Prop::State, Op::Eq, 0);                               // "the fan is off"
    core.add(r);
    env.set("t1", Prop::Temp, 20);
    env.set("r2", Prop::State, 1);
    run(core, env, 0, 300);
    EQ(0, env.ns);                                                          // neither holds
    env.set("t1", Prop::Temp, 40);
    run(core, env, 400, 700);
    EQ(any ? 1 : 0, env.ns);                                                // hot but the fan is on: only OR fires
    env.set("r2", Prop::State, 0);
    run(core, env, 800, 1200);
    EQ(1, env.ns);                                                          // both true now (AND fires here)
  }
}

void test_interval_trigger() {
  AutomationCore core; Fake env;
  Rule r = base("i", TrigType::Interval);
  r.trig.everyS = 10;
  core.add(r);
  run(core, env, 0, 9900);
  EQ(0, env.ns);
  run(core, env, 10000, 10300);
  EQ(1, env.ns);
  run(core, env, 10400, 19900);
  EQ(1, env.ns);
  run(core, env, 20000, 20300);
  EQ(2, env.ns);
  core.tick(env, 600000);                                                   // a long stall does not cause a burst
  core.tick(env, 600100);
  core.tick(env, 600200);
  EQ(3, env.ns);
}

void test_schedule_trigger() {
  AutomationCore core; Fake env;
  Rule r = base("s", TrigType::Schedule);
  r.trig.minute = 18 * 60;
  r.trig.days = uint8_t(1 << 1);                                            // Mondays
  core.add(r);
  env.haveClock = false;
  env.clk.minuteOfDay = 18 * 60; env.clk.weekday = 1; env.clk.minuteKey = 1000;
  run(core, env, 0, 1000);
  EQ(0, env.ns);                                                            // no clock: never fires
  env.haveClock = true;
  env.clk.weekday = 2;
  run(core, env, 1100, 2000);
  EQ(0, env.ns);                                                            // a Tuesday
  env.clk.weekday = 1; env.clk.minuteOfDay = 18 * 60 - 1;
  run(core, env, 2100, 3000);
  EQ(0, env.ns);                                                            // a minute early
  env.clk.minuteOfDay = 18 * 60; env.clk.minuteKey = 1001;
  run(core, env, 3100, 9000);
  EQ(1, env.ns);                                                            // once for the whole minute
  env.clk.minuteOfDay = 18 * 60 + 1; env.clk.minuteKey = 1002;
  run(core, env, 9100, 9500);
  env.clk.minuteOfDay = 18 * 60; env.clk.minuteKey = 1003;                  // the next day's 18:00 (same weekday here)
  run(core, env, 9600, 10000);
  EQ(2, env.ns);
}

void test_startup_trigger() {
  AutomationCore core; Fake env;
  core.add(base("u", TrigType::Startup));
  run(core, env, 0, 2900);
  EQ(0, env.ns);
  run(core, env, 3000, 6000);
  EQ(1, env.ns);                                                            // once
  core.add(base("late", TrigType::Startup));                                // added while running: not a boot event
  run(core, env, 6100, 9000);
  EQ(1, env.ns);
}

void test_wifi_trigger() {
  AutomationCore core; Fake env;
  Rule up = base("up", TrigType::Wifi); up.trig.wifiUp = true; strcpy(up.acts[0].device, "r1");
  Rule dn = base("dn", TrigType::Wifi); dn.trig.wifiUp = false; strcpy(dn.acts[0].device, "r2");
  core.add(up); core.add(dn);
  env.wifi = false;
  run(core, env, 0, 500);
  EQ(0, env.ns);                                                            // the starting state is not an event
  env.wifi = true;
  run(core, env, 600, 1000);
  EQ(1, env.count("r1")); EQ(0, env.count("r2"));
  env.wifi = false;
  run(core, env, 1100, 1500);
  EQ(1, env.count("r1")); EQ(1, env.count("r2"));
}

void test_manual_run() {
  AutomationCore core; Fake env;
  Rule r = base("m", TrigType::Manual);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30);
  core.add(r);
  env.set("t1", Prop::Temp, 20);
  run(core, env, 0, 1000);
  EQ(0, env.ns);                                                            // manual rules never run by themselves
  TEST_ASSERT_TRUE(core.requestRun("m"));
  run(core, env, 1100, 1300);
  EQ(0, env.ns);
  EQ(Last::ConditionsFalse, core.state(0).last);                            // the conditions guard a manual run
  env.set("t1", Prop::Temp, 40);
  core.requestRun("m");
  run(core, env, 1400, 1600);
  EQ(1, env.ns);
  TEST_ASSERT_FALSE(core.requestRun("nope"));
  core.setEnabled("m", false);
  core.requestRun("m");
  run(core, env, 1700, 2500);
  EQ(1, env.ns);                                                            // disabled rules do not run
}

void test_time_window_condition() {
  AutomationCore core; Fake env;
  Rule r = base("t", TrigType::Manual);
  Cond& c = r.conds[r.nConds++];
  c.kind = CondKind::Time; c.from = 22 * 60; c.to = 6 * 60; c.days = 0x7F;  // 22:00 to 06:00
  core.add(r);
  env.haveClock = true; env.clk.weekday = 3;
  struct { int minute; bool in; } t[] = {{23 * 60, true}, {0, true}, {5 * 60 + 59, true}, {6 * 60, false}, {12 * 60, false}, {22 * 60 - 1, false}, {22 * 60, true}};
  uint32_t now = 0;
  size_t expect = 0;
  for (auto& x : t) {
    env.clk.minuteOfDay = uint16_t(x.minute);
    core.requestRun("t");
    now += 1000;                                                            // clear of the 200 ms gap between firings
    core.tick(env, now);
    if (x.in) expect++;
    EQ(expect, env.ns);
  }
  env.haveClock = false;
  core.requestRun("t");
  core.tick(env, now + 1000);
  EQ(expect, env.ns);                                                       // no clock: the window is closed
}

void test_time_window_days() {
  AutomationCore core; Fake env;
  Rule r = base("t", TrigType::Manual);
  Cond& c = r.conds[r.nConds++];
  c.kind = CondKind::Time; c.from = 8 * 60; c.to = 20 * 60; c.days = uint8_t((1 << 1) | (1 << 5));  // Monday and Friday
  core.add(r);
  env.haveClock = true; env.clk.minuteOfDay = 12 * 60;
  uint32_t now = 0;
  for (int day = 0; day < 7; day++) {
    env.clk.weekday = uint8_t(day);
    size_t before = env.ns;
    core.requestRun("t");
    now += 1000;
    core.tick(env, now);
    EQ((day == 1 || day == 5) ? 1 : 0, env.ns - before);
  }
}

// ---- actions and safeguards

void test_actions_in_order() {
  AutomationCore core; Fake env;
  Rule r = base("a", TrigType::Manual);
  r.nActs = 4;
  r.acts[0].type = ActType::On; strcpy(r.acts[0].device, "r1");
  r.acts[1].type = ActType::Set; strcpy(r.acts[1].device, "p1"); r.acts[1].value = 60;
  r.acts[2].type = ActType::Stop; strcpy(r.acts[2].device, "m1");
  r.acts[3].type = ActType::Send; strcpy(r.acts[3].device, "ser"); strcpy(r.acts[3].text, "hi");
  core.add(r);
  core.requestRun("a");
  core.tick(env, 100);
  EQ(4, env.ns);
  EQ(CmdAction::Set, env.sent[0].cmd.action); TEST_ASSERT_TRUE(env.sent[0].cmd.isBool); EQ(1, env.sent[0].cmd.value);
  TEST_ASSERT_EQUAL_STRING("p1", env.sent[1].dev); EQ(60, env.sent[1].cmd.value);
  EQ(CmdAction::Stop, env.sent[2].cmd.action);
  TEST_ASSERT_EQUAL_STRING("hi", env.sent[3].cmd.text);
}

void test_event_action() {
  AutomationCore core; Fake env;
  Rule r = base("a", TrigType::Manual);
  r.acts[0].type = ActType::Event; r.acts[0].device[0] = 0; strcpy(r.acts[0].text, "hot");
  core.add(r);
  core.requestRun("a");
  core.tick(env, 100);
  EQ(0, env.ns);                                                            // no device command
  TEST_ASSERT_EQUAL_STRING("a:event:hot", env.events[1]);
  EQ(EvKind::Custom, core.eventAt(0).kind);
  TEST_ASSERT_EQUAL_STRING("hot", core.eventAt(0).text);
}

void test_rejected_and_missing_devices() {
  AutomationCore core; Fake env;
  Rule r = base("a", TrigType::Manual);
  core.add(r);
  env.cmdResult = CmdErr::QueueFull;
  core.requestRun("a");
  core.tick(env, 100);
  EQ(Last::Rejected, core.state(0).last);
  EQ(1, core.state(0).failed);
  EQ(1, core.state(0).fired);                                               // it was triggered, it did not complete
  env.cmdResult = CmdErr::Ok;
  Rule gone = base("b", TrigType::Manual);
  strcpy(gone.acts[0].device, "removed");
  core.add(gone);
  core.requestRun("b");
  core.tick(env, 300);
  EQ(Last::NoDevice, core.state(1).last);
  EQ(1, core.state(1).failed);
}

void test_cooldown() {
  AutomationCore core; Fake env;
  Rule r = changeRule("c", "t1", Prop::Temp);
  r.cooldownS = 5;
  core.add(r);
  env.set("t1", Prop::Temp, 1);
  run(core, env, 0, 200);
  for (int i = 2; i <= 40; i++) { env.set("t1", Prop::Temp, i); core.tick(env, 200 + uint32_t(i) * 100); }  // a change every tick for 4 s
  EQ(1, env.ns);                                                            // one firing, the rest held back
  EQ(Last::Cooldown, core.state(0).last);
  env.set("t1", Prop::Temp, 99);
  core.tick(env, 6000);
  EQ(2, env.ns);
}

void test_min_gap_between_firings() {
  AutomationCore core; Fake env;
  core.add(changeRule("c", "t1", Prop::Temp));
  env.set("t1", Prop::Temp, 0);
  core.tick(env, 0);
  for (int i = 1; i <= 10; i++) { env.set("t1", Prop::Temp, i); core.tick(env, uint32_t(i) * 100); }   // changes every 100 ms for a second
  EQ(5, env.ns);                                                            // never faster than every 200 ms
}

void test_storm_guard() {
  AutomationCore core; Fake env;
  for (size_t i = 0; i < kMaxRules; i++) {
    char id[8]; snprintf(id, sizeof id, "s%u", unsigned(i));
    core.add(changeRule(id, "t1", Prop::Temp));
  }
  env.set("t1", Prop::Temp, 0);
  core.tick(env, 0);
  for (int i = 1; i <= 9; i++) { env.set("t1", Prop::Temp, i); core.tick(env, uint32_t(i) * 100); }   // 16 rules fire on each change
  TEST_ASSERT_TRUE(env.ns <= AutomationCore::kStormMax);                    // never more than 30 firings a second
  TEST_ASSERT_TRUE(env.ns > 0);
  bool storm = false;
  for (size_t i = 0; i < core.eventCount(); i++) if (core.eventAt(i).kind == EvKind::Storm) storm = true;
  TEST_ASSERT_TRUE(storm);
  bool any = false;
  for (size_t i = 0; i < core.count(); i++) if (core.state(i).last == Last::Storm) any = true;
  TEST_ASSERT_TRUE(any);
}

void test_interlock() {
  AutomationCore core; Fake env;
  core.settings.nLocks = 1;
  strcpy(core.settings.locks[0].a, "r1"); strcpy(core.settings.locks[0].b, "r2");
  Rule r = base("a", TrigType::Manual);                                     // turns r1 on
  core.add(r);
  env.set("r2", Prop::State, 1);                                            // the partner is on
  core.requestRun("a");
  core.tick(env, 100);
  EQ(0, env.ns);
  EQ(Last::Blocked, core.state(0).last);
  env.set("r2", Prop::State, 0);
  core.requestRun("a");
  core.tick(env, 1000);
  EQ(1, env.ns);                                                            // fine once the partner is off
  // the pair works in both directions, and off / unrelated devices are never blocked
  Rule b = base("b", TrigType::Manual); strcpy(b.acts[0].device, "r2");
  core.add(b);
  env.set("r1", Prop::State, 1);
  core.requestRun("b");
  core.tick(env, 2000);
  EQ(1, env.ns);
  Rule off = base("off", TrigType::Manual); off.acts[0].type = ActType::Off; strcpy(off.acts[0].device, "r2");
  core.add(off);
  core.requestRun("off");
  core.tick(env, 3000);
  EQ(2, env.ns);                                                            // turning off is always allowed
}

void test_interlock_toggle() {
  AutomationCore core; Fake env;
  core.settings.nLocks = 1;
  strcpy(core.settings.locks[0].a, "r1"); strcpy(core.settings.locks[0].b, "r2");
  Rule t = base("t", TrigType::Manual); t.acts[0].type = ActType::Toggle; strcpy(t.acts[0].device, "r1");
  core.add(t);
  env.set("r1", Prop::State, 0); env.set("r2", Prop::State, 1);
  core.requestRun("t"); core.tick(env, 100);
  EQ(0, env.ns);                                                            // toggling r1 on while r2 is on: blocked
  env.set("r1", Prop::State, 1);
  core.requestRun("t"); core.tick(env, 1000);
  EQ(1, env.ns);                                                            // toggling r1 off is fine
}

void test_verification() {
  AutomationCore core; Fake env;
  core.add(base("a", TrigType::Manual));
  env.followCommands = false;                                               // the relay never actually turns on
  env.set("r1", Prop::State, 0);
  core.requestRun("a");
  core.tick(env, 100);
  EQ(0, core.state(0).unconfirmed);                                         // too early to know
  core.tick(env, 1500);
  EQ(0, core.state(0).unconfirmed);
  core.tick(env, 1700);
  EQ(1, core.state(0).unconfirmed);
  EQ(EvKind::Unconfirmed, core.eventAt(0).kind);
  core.tick(env, 3000);
  EQ(1, core.state(0).unconfirmed);                                         // reported once
}

void test_verification_ok() {
  AutomationCore core; Fake env;
  Rule r = base("a", TrigType::Manual);
  r.nActs = 2;
  r.acts[1].type = ActType::Set; strcpy(r.acts[1].device, "p1"); r.acts[1].value = 40;
  core.add(r);
  core.requestRun("a");
  core.tick(env, 100);
  core.tick(env, 2000);
  EQ(0, core.state(0).unconfirmed);                                         // both devices followed
  Rule off = base("o", TrigType::Manual); off.acts[0].type = ActType::Off;
  core.add(off);
  env.followCommands = false;
  env.set("r1", Prop::State, 1);                                            // an off command that did not take
  core.requestRun("o");
  core.tick(env, 3000);
  core.tick(env, 5000);
  EQ(1, core.state(1).unconfirmed);
}

void test_verification_survives_remove() {
  AutomationCore core; Fake env;
  core.add(base("a", TrigType::Manual));
  core.add(base("b", TrigType::Manual));
  env.followCommands = false;
  env.set("r1", Prop::State, 0);
  core.requestRun("b");
  core.tick(env, 100);
  core.remove("a");                                                         // b moves from index 1 to 0
  core.tick(env, 2000);
  EQ(1, core.state(0).unconfirmed);                                         // the pending check followed b
  Rule c = base("c", TrigType::Manual);
  core.add(c);
  core.requestRun("c");
  core.tick(env, 3000);
  core.remove("c");                                                         // its check is dropped with it
  core.tick(env, 5000);
  EQ(1, core.state(0).unconfirmed);
}

void test_engine_switch() {
  AutomationCore core; Fake env;
  core.add(base("a", TrigType::Interval));
  core.settings.enabled = false;
  Rule r = base("i", TrigType::Interval); r.trig.everyS = 1;
  core.add(r);
  run(core, env, 0, 5000);
  EQ(0, env.ns);
  core.settings.enabled = true;
  run(core, env, 5100, 9000);
  TEST_ASSERT_TRUE(env.ns > 0);
}

void test_event_log_ring() {
  AutomationCore core; Fake env;
  Rule r = base("a", TrigType::Manual);
  r.acts[0].type = ActType::Event; r.acts[0].device[0] = 0;
  core.add(r);
  uint32_t now = 0;
  for (int i = 0; i < 12; i++) {
    snprintf(r.acts[0].text, kActTextCap, "e%d", i);
    core.replace(r);
    core.requestRun("a");
    now += 1000;
    core.tick(env, now);
  }
  EQ(AutomationCore::kEvents, core.eventCount());                           // keeps the latest eight
  TEST_ASSERT_EQUAL_STRING("e11", core.eventAt(0).text);                    // newest first
  EQ(EvKind::Fired, core.eventAt(1).kind);
  TEST_ASSERT_EQUAL_STRING("e8", core.eventAt(6).text);
}

// ---- extra checks found by the mutation run

void test_compare_ne_margin() {
  TEST_ASSERT_TRUE(compare(Op::Ne, 5, 5.5));      // half a unit apart is "not equal"
  TEST_ASSERT_FALSE(compare(Op::Ne, 5, 5.00001));
}

void test_text_rules() {
  char t[kActTextCap];
  memset(t, 'x', sizeof t);
  TEST_ASSERT_FALSE(textOk(t, sizeof t, false));  // no terminator
  strcpy(t, "ok"); TEST_ASSERT_TRUE(textOk(t, sizeof t, false));
  strcpy(t, "a\x1f"); TEST_ASSERT_FALSE(textOk(t, sizeof t, false));   // 0x1F is a control character
  strcpy(t, "a\x20"); TEST_ASSERT_TRUE(textOk(t, sizeof t, false));    // a space is fine
  strcpy(t, "a\x7e"); TEST_ASSERT_TRUE(textOk(t, sizeof t, false));    // ~
  strcpy(t, "a\x7f"); TEST_ASSERT_FALSE(textOk(t, sizeof t, false));   // DEL
  t[0] = 0; TEST_ASSERT_TRUE(textOk(t, sizeof t, true)); TEST_ASSERT_FALSE(textOk(t, sizeof t, false));
  char u[kActTextCap + 1];
  memset(u, 'x', kActTextCap);
  u[kActTextCap] = 0;                                                  // printable all the way, but no terminator within the limit
  TEST_ASSERT_FALSE(textOk(u, kActTextCap, false));
}

void test_validation_edges() {
  Rule t = base("a", TrigType::Manual);
  Cond& c = t.conds[t.nConds++];
  c.kind = CondKind::Time; c.from = 8 * 60; c.to = 1440; c.days = 0x7F;
  EQ(AutoErr::Time, check(t));                                          // 24:00 is not a minute of the day
  c.to = 1439; c.days = uint8_t(0x80 | 1);
  EQ(AutoErr::Days, check(t));                                          // a bit above Saturday
  Rule r = base("a", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, -2e6f);
  EQ(AutoErr::Value, check(r));                                         // too far below as well as above
  Rule n = base("a", TrigType::Manual);
  n.acts[0].type = ActType::Set; strcpy(n.acts[0].device, "p1"); n.acts[0].value = NAN;
  EQ(AutoErr::Value, check(n));
  Rule k = base("a", TrigType::Manual);
  k.acts[0].type = ActType::kCount; strcpy(k.acts[0].device, "nope");
  EQ(AutoErr::Action, check(k));                                        // judged as a bad action before its device
  AutoSettings s;
  s.nLocks = kMaxInterlocks + 1;
  for (size_t i = 0; i < kMaxInterlocks + 1; i++) { strcpy(s.locks[i].a, "r1"); strcpy(s.locks[i].b, "r2"); }
  EQ(AutoErr::Interlock, validateSettings(s, find, nullptr));           // a fifth, valid pair is still too many
  s.nLocks = kMaxInterlocks;
  EQ(AutoErr::Ok, validateSettings(s, find, nullptr));
}

void test_error_names() {
  TEST_ASSERT_EQUAL_STRING("duplicate_id", autoErrCode(AutoErr::Duplicate));
  TEST_ASSERT_EQUAL_STRING("unknown_device", autoErrCode(AutoErr::Device));
  TEST_ASSERT_EQUAL_STRING("bad_conditions", autoErrCode(AutoErr::Conds));
  TEST_ASSERT_EQUAL_STRING("days", autoErrField(AutoErr::Days));
  TEST_ASSERT_EQUAL_STRING("time", autoErrField(AutoErr::Time));
  TEST_ASSERT_EQUAL_STRING("", autoErrField(AutoErr::Ok));
  TEST_ASSERT_EQUAL_STRING("", autoErrCode(AutoErr::Ok));
}

void test_replace_keeps_startup_done() {
  AutomationCore core; Fake env;
  core.add(base("u", TrigType::Startup));
  run(core, env, 0, 4000);
  EQ(1, env.ns);
  core.replace(base("u", TrigType::Startup));                           // editing it must not make it boot again
  run(core, env, 4100, 8000);
  EQ(1, env.ns);
}

void test_replace_drops_pending_check() {
  AutomationCore core; Fake env;
  core.add(base("a", TrigType::Manual));
  env.followCommands = false;
  env.set("r1", Prop::State, 0);
  core.requestRun("a");
  core.tick(env, 100);
  core.replace(base("a", TrigType::Manual));                            // the old check belonged to the old rule
  core.tick(env, 3000);
  EQ(0, core.state(0).unconfirmed);
}

void test_remove_drops_pending_check() {
  AutomationCore core; Fake env;
  core.add(base("a", TrigType::Manual));
  core.add(base("b", TrigType::Manual));
  env.followCommands = false;
  env.set("r1", Prop::State, 0);
  core.requestRun("a");
  core.tick(env, 100);
  core.remove("a");                                                     // b takes index 0: it must not inherit the check
  core.tick(env, 3000);
  EQ(0, core.state(0).unconfirmed);
}

void test_enable_rearms_when_rule() {
  AutomationCore core; Fake env;
  Rule r = base("w", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30);
  core.add(r);
  env.set("t1", Prop::Temp, 40);
  run(core, env, 0, 500);
  EQ(1, env.ns);
  core.setEnabled("w", false);
  run(core, env, 600, 1500);
  core.setEnabled("w", true);
  run(core, env, 1600, 2500);
  EQ(2, env.ns);                                                        // true when switched on again: that is a new start
}

void test_interval_keeps_its_phase() {
  AutomationCore core; Fake env;
  Rule r = base("i", TrigType::Interval); r.trig.everyS = 10;
  core.add(r);
  core.tick(env, 0);
  core.tick(env, 10050);                                                // a late pass
  EQ(1, env.ns);
  core.tick(env, 20000);                                                // the next one is still due at 20.000 s, not 20.050
  EQ(2, env.ns);
}

void test_time_window_boundaries() {
  AutomationCore core; Fake env;
  Rule r = base("t", TrigType::Manual);
  Cond& c = r.conds[r.nConds++];
  c.kind = CondKind::Time; c.from = 8 * 60; c.to = 20 * 60; c.days = 0x7F;
  core.add(r);
  env.haveClock = true; env.clk.weekday = 2;
  struct { int minute; bool in; } t[] = {{8 * 60 - 1, false}, {8 * 60, true}, {20 * 60 - 1, true}, {20 * 60, false}};
  uint32_t now = 0;
  size_t expect = 0;
  for (auto& x : t) {
    env.clk.minuteOfDay = uint16_t(x.minute);
    core.requestRun("t");
    now += 1000;
    core.tick(env, now);
    if (x.in) expect++;
    EQ(expect, env.ns);
  }
}

void test_duration_needs_continuous_watching() {
  AutomationCore core; Fake env;
  Rule r = base("m", TrigType::Manual);                                 // a duration on a manual rule can never be met
  addCond(r, "t1", Prop::Temp, Op::Gt, 30, 1);
  core.add(r);
  env.set("t1", Prop::Temp, 40);
  core.requestRun("m");
  core.tick(env, 100000);
  EQ(0, env.ns);
  EQ(Last::ConditionsFalse, core.state(0).last);
}

void test_manual_run_uses_measured_duration() {
  AutomationCore core; Fake env;
  Rule r = base("w", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30, 2);
  core.add(r);
  env.set("t1", Prop::Temp, 40);
  run(core, env, 0, 1000);
  core.requestRun("w");
  core.tick(env, 1100);
  EQ(0, env.ns);                                                        // only 1.1 s of the 2 s so far
  run(core, env, 1200, 2000);
  EQ(1, env.ns);                                                        // the trigger itself at 2 s
  core.requestRun("w");
  core.tick(env, 3000);
  EQ(2, env.ns);                                                        // "run now" counts the time already measured
}

void test_duration_boundary() {
  AutomationCore core; Fake env;
  Rule r = base("w", TrigType::When);
  addCond(r, "t1", Prop::Temp, Op::Gt, 30, 2);
  core.add(r);
  env.set("t1", Prop::Temp, 40);
  run(core, env, 0, 1900);
  EQ(0, env.ns);
  core.tick(env, 2000);
  EQ(1, env.ns);                                                        // exactly two seconds counts
}

void test_any_keeps_every_timer_running() {
  AutomationCore core; Fake env;
  Rule r = base("w", TrigType::When);
  r.matchAny = true;
  addCond(r, "t1", Prop::Temp, Op::Gt, 30);                             // A
  addCond(r, "r2", Prop::State, Op::Eq, 1, 2);                          // B, held for 2 s
  core.add(r);
  env.set("t1", Prop::Temp, 40);
  env.set("r2", Prop::State, 1);
  run(core, env, 0, 900);
  EQ(1, env.ns);                                                        // A is true at once
  env.set("t1", Prop::Temp, 20);                                        // A goes false at 1 s; B has been true since 0
  run(core, env, 1000, 2000);
  EQ(2, env.ns);                                                        // B's two seconds ran while A was true
}

void test_verify_set_and_stop() {
  AutomationCore core; Fake env;
  Rule s = base("s", TrigType::Manual); s.acts[0].type = ActType::Set; strcpy(s.acts[0].device, "p1"); s.acts[0].value = 40;
  Rule m = base("m", TrigType::Manual); m.acts[0].type = ActType::Stop; strcpy(m.acts[0].device, "m1");
  core.add(s); core.add(m);
  env.followCommands = false;
  env.set("p1", Prop::Level, 0);                                        // the pwm stays at 0, the motor keeps turning
  env.set("m1", Prop::Level, 60);
  core.requestRun("s"); core.tick(env, 100);
  core.requestRun("m"); core.tick(env, 400);
  core.tick(env, 3000);
  EQ(1, core.state(0).unconfirmed);
  EQ(1, core.state(1).unconfirmed);
}

void test_storm_window_reopens() {
  AutomationCore core; Fake env;
  for (size_t i = 0; i < kMaxRules; i++) {
    char id[8]; snprintf(id, sizeof id, "s%u", unsigned(i));
    core.add(changeRule(id, "t1", Prop::Temp));
  }
  env.set("t1", Prop::Temp, 0);
  core.tick(env, 0);
  for (int i = 1; i <= 9; i++) { env.set("t1", Prop::Temp, i); core.tick(env, uint32_t(i) * 100); }
  size_t first = env.ns;
  TEST_ASSERT_TRUE(first <= AutomationCore::kStormMax);
  env.set("t1", Prop::Temp, 50);
  core.tick(env, 2500);                                                 // a new second: the budget is back
  TEST_ASSERT_TRUE(env.ns > first);
}

void test_run_while_engine_off_is_dropped() {
  AutomationCore core; Fake env;
  core.add(base("a", TrigType::Manual));
  core.settings.enabled = false;
  core.requestRun("a");
  core.tick(env, 100);
  core.tick(env, 1000);
  core.settings.enabled = true;
  core.tick(env, 2000);
  EQ(0, env.ns);                                                        // the request did not wait for the switch to come back on
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_local_clock);
  RUN_TEST(test_compare_ops);
  RUN_TEST(test_names_round_trip);
  RUN_TEST(test_build_command);
  RUN_TEST(test_validate_basics);
  RUN_TEST(test_validate_triggers);
  RUN_TEST(test_validate_conditions);
  RUN_TEST(test_validate_actions);
  RUN_TEST(test_validate_settings);
  RUN_TEST(test_core_add_remove);
  RUN_TEST(test_core_enable_disable);
  RUN_TEST(test_change_trigger);
  RUN_TEST(test_change_trigger_with_condition);
  RUN_TEST(test_change_trigger_unreadable_device);
  RUN_TEST(test_when_trigger_edge);
  RUN_TEST(test_when_trigger_already_true_at_start);
  RUN_TEST(test_when_trigger_duration);
  RUN_TEST(test_when_all_and_any);
  RUN_TEST(test_interval_trigger);
  RUN_TEST(test_schedule_trigger);
  RUN_TEST(test_startup_trigger);
  RUN_TEST(test_wifi_trigger);
  RUN_TEST(test_manual_run);
  RUN_TEST(test_time_window_condition);
  RUN_TEST(test_time_window_days);
  RUN_TEST(test_actions_in_order);
  RUN_TEST(test_event_action);
  RUN_TEST(test_rejected_and_missing_devices);
  RUN_TEST(test_cooldown);
  RUN_TEST(test_min_gap_between_firings);
  RUN_TEST(test_storm_guard);
  RUN_TEST(test_interlock);
  RUN_TEST(test_interlock_toggle);
  RUN_TEST(test_verification);
  RUN_TEST(test_verification_ok);
  RUN_TEST(test_verification_survives_remove);
  RUN_TEST(test_engine_switch);
  RUN_TEST(test_event_log_ring);
  RUN_TEST(test_compare_ne_margin);
  RUN_TEST(test_text_rules);
  RUN_TEST(test_validation_edges);
  RUN_TEST(test_error_names);
  RUN_TEST(test_replace_keeps_startup_done);
  RUN_TEST(test_replace_drops_pending_check);
  RUN_TEST(test_remove_drops_pending_check);
  RUN_TEST(test_enable_rearms_when_rule);
  RUN_TEST(test_interval_keeps_its_phase);
  RUN_TEST(test_time_window_boundaries);
  RUN_TEST(test_duration_needs_continuous_watching);
  RUN_TEST(test_duration_boundary);
  RUN_TEST(test_manual_run_uses_measured_duration);
  RUN_TEST(test_any_keeps_every_timer_running);
  RUN_TEST(test_verify_set_and_stop);
  RUN_TEST(test_storm_window_reopens);
  RUN_TEST(test_run_while_engine_off_is_dropped);
  return UNITY_END();
}
