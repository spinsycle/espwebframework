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

// ---- sequences and timers (P7b)

static bool gSeqs = true, gTimers = true;
static bool hasSeq(void*, const char* id) { return gSeqs && (!strcmp(id, "s1") || !strcmp(id, "seq")); }
static bool hasTimer(void*, const char* id) { return gTimers && (!strcmp(id, "t1x") || !strcmp(id, "tm")); }
static AutoRefs refs() {
  AutoRefs r;
  r.hasSeq = hasSeq;
  r.hasTimer = hasTimer;
  return r;
}
static AutoErr checkR(const Rule& r) { AutoRefs f = refs(); return validateRule(r, find, nullptr, &f); }
static AutoErr checkSeq(const SeqDef& q) { AutoRefs f = refs(); return validateSeq(q, find, nullptr, &f); }

static Step doStep(ActType t, const char* dev, float value = 0) {
  Step s;
  s.type = StepType::Do;
  s.act.type = t;
  strcpy(s.act.device, dev);
  s.act.value = value;
  return s;
}
static Step waitStep(uint32_t secs) { Step s; s.type = StepType::Wait; s.secs = secs; return s; }
static Step untilStep(const char* dev, Prop p, Op op, float v, uint32_t timeout, bool stopOnTimeout = false) {
  Step s;
  s.type = StepType::Until;
  s.secs = timeout;
  s.stopOnTimeout = stopOnTimeout;
  s.cond.kind = CondKind::Compare;
  strcpy(s.cond.device, dev); s.cond.prop = p; s.cond.op = op; s.cond.value = v;
  return s;
}
static SeqDef mkSeq(const char* id, std::initializer_list<Step> steps, uint8_t repeat = 1) {
  SeqDef q;
  strcpy(q.id, id);
  strcpy(q.name, id);
  q.repeat = repeat;
  q.nSteps = 0;
  for (const Step& st : steps) q.steps[q.nSteps++] = st;
  return q;
}
static TimerDef mkTimer(const char* id, uint32_t secs, bool repeat = false) {
  TimerDef t;
  strcpy(t.id, id);
  strcpy(t.name, id);
  t.durationS = secs;
  t.repeat = repeat;
  return t;
}
static bool seqState(AutomationCore& c, const char* id, AutomationCore::SeqRunState st) { return c.seqRun(size_t(c.findSeq(id))).st == st; }
using RS = AutomationCore::SeqRunState;

void test_new_names() {
  TrigType t; ActType a; StepType st;
  TEST_ASSERT_TRUE(parseTrig("timer", t)); EQ(TrigType::Timer, t);
  TEST_ASSERT_TRUE(parseTrig("sequence", t)); EQ(TrigType::Sequence, t);
  TEST_ASSERT_TRUE(parseAct("seq_start", a)); EQ(ActType::SeqStart, a);
  TEST_ASSERT_TRUE(parseAct("seq_stop", a)); EQ(ActType::SeqStop, a);
  TEST_ASSERT_TRUE(parseAct("timer_start", a)); EQ(ActType::TimerStart, a);
  TEST_ASSERT_TRUE(parseAct("timer_stop", a)); EQ(ActType::TimerStop, a);
  EQ(9, TrigType::kCount); EQ(11, ActType::kCount); EQ(3, StepType::kCount);
  for (uint8_t i = 0; i < uint8_t(StepType::kCount); i++) { TEST_ASSERT_TRUE(parseStep(stepName(StepType(i)), st)); EQ(i, st); }
  TEST_ASSERT_FALSE(parseStep("jump", st));
  TEST_ASSERT_EQUAL_STRING("unknown_target", autoErrCode(AutoErr::Target));
  TEST_ASSERT_EQUAL_STRING("bad_loop", autoErrCode(AutoErr::Loop));
  TEST_ASSERT_EQUAL_STRING("bad_steps", autoErrCode(AutoErr::Steps));
  TEST_ASSERT_EQUAL_STRING("bad_timer", autoErrCode(AutoErr::Timer));
  TEST_ASSERT_EQUAL_STRING("steps", autoErrField(AutoErr::Steps));
  TEST_ASSERT_EQUAL_STRING("repeat", autoErrField(AutoErr::Loop));
  TEST_ASSERT_EQUAL_STRING("duration_s", autoErrField(AutoErr::Timer));
  TEST_ASSERT_EQUAL_STRING("device", autoErrField(AutoErr::Target));
}

void test_validate_rule_targets() {
  gSeqs = gTimers = true;
  Rule r = base("a", TrigType::Manual);
  r.acts[0].type = ActType::SeqStart; strcpy(r.acts[0].device, "s1");
  EQ(AutoErr::Ok, checkR(r));
  EQ(AutoErr::Target, check(r));                                            // no reference table: not allowed
  strcpy(r.acts[0].device, "nope"); EQ(AutoErr::Target, checkR(r));
  r.acts[0].type = ActType::SeqStop; strcpy(r.acts[0].device, "s1"); EQ(AutoErr::Ok, checkR(r));
  r.acts[0].type = ActType::TimerStart; strcpy(r.acts[0].device, "tm"); EQ(AutoErr::Ok, checkR(r));
  strcpy(r.acts[0].device, "s1"); EQ(AutoErr::Target, checkR(r));           // a sequence id is not a timer
  r.acts[0].type = ActType::TimerStop; strcpy(r.acts[0].device, "tm"); EQ(AutoErr::Ok, checkR(r));
  r.acts[0].device[0] = 0; EQ(AutoErr::Target, checkR(r));
  Rule t = base("a", TrigType::Timer); strcpy(t.trig.device, "tm");
  EQ(AutoErr::Ok, checkR(t));
  strcpy(t.trig.device, "s1"); EQ(AutoErr::Target, checkR(t));
  EQ(AutoErr::Target, check(t));
  Rule q = base("a", TrigType::Sequence); strcpy(q.trig.device, "seq");
  EQ(AutoErr::Ok, checkR(q));
  strcpy(q.trig.device, "tm"); EQ(AutoErr::Target, checkR(q));
  q.trig.onStopped = true; strcpy(q.trig.device, "s1"); EQ(AutoErr::Ok, checkR(q));
  gSeqs = false;
  EQ(AutoErr::Target, checkR(q));                                           // the sequence was deleted meanwhile
  gSeqs = true;
}

void test_validate_steps() {
  SeqDef q = mkSeq("q", {doStep(ActType::On, "r1")});
  EQ(AutoErr::Ok, checkSeq(q));
  strcpy(q.id, "Bad Id"); EQ(AutoErr::Id, checkSeq(q));
  q = mkSeq("q", {}); EQ(AutoErr::Steps, checkSeq(q));
  q = mkSeq("q", {doStep(ActType::On, "r1")});
  q.nSteps = kMaxSteps;
  for (size_t i = 0; i < kMaxSteps; i++) q.steps[i] = doStep(ActType::On, "r1");
  EQ(AutoErr::Ok, checkSeq(q));
  q.nSteps = kMaxSteps + 1; q.steps[kMaxSteps] = doStep(ActType::On, "r1");
  EQ(AutoErr::Steps, checkSeq(q));
  q = mkSeq("q", {doStep(ActType::On, "nope")}); EQ(AutoErr::Device, checkSeq(q));
  q = mkSeq("q", {doStep(ActType::Set, "p1", 101)}); EQ(AutoErr::Action, checkSeq(q));
  q = mkSeq("q", {doStep(ActType::SeqStart, "s1")}); EQ(AutoErr::Action, checkSeq(q));      // no sequences inside sequences
  q = mkSeq("q", {doStep(ActType::SeqStop, "s1")}); EQ(AutoErr::Action, checkSeq(q));
  q = mkSeq("q", {doStep(ActType::TimerStart, "tm")}); EQ(AutoErr::Ok, checkSeq(q));         // timers are fine
  q = mkSeq("q", {doStep(ActType::TimerStart, "nope")}); EQ(AutoErr::Target, checkSeq(q));
  Step ev; ev.type = StepType::Do; ev.act.type = ActType::Event; strcpy(ev.act.text, "go");
  q = mkSeq("q", {ev}); EQ(AutoErr::Ok, checkSeq(q));
  q = mkSeq("q", {waitStep(0)}); EQ(AutoErr::Time, checkSeq(q));
  q = mkSeq("q", {waitStep(1)}); EQ(AutoErr::Ok, checkSeq(q));
  q = mkSeq("q", {waitStep(86400)}); EQ(AutoErr::Ok, checkSeq(q));
  q = mkSeq("q", {waitStep(86401)}); EQ(AutoErr::Time, checkSeq(q));
  q = mkSeq("q", {untilStep("t1", Prop::Temp, Op::Lt, 25, 0)}); EQ(AutoErr::Time, checkSeq(q));      // a timeout is required
  q = mkSeq("q", {untilStep("t1", Prop::Temp, Op::Lt, 25, 60)}); EQ(AutoErr::Ok, checkSeq(q));
  q = mkSeq("q", {untilStep("t1", Prop::Temp, Op::Lt, 25, 86401)}); EQ(AutoErr::Time, checkSeq(q));
  q = mkSeq("q", {untilStep("nope", Prop::Temp, Op::Lt, 25, 60)}); EQ(AutoErr::Device, checkSeq(q));
  q = mkSeq("q", {untilStep("t1", Prop::Humidity, Op::Lt, 25, 60)}); EQ(AutoErr::Prop, checkSeq(q));
  Step g = doStep(ActType::On, "r1");
  g.hasGuard = true; g.cond.kind = CondKind::Compare; strcpy(g.cond.device, "t1"); g.cond.prop = Prop::Temp; g.cond.op = Op::Gt; g.cond.value = 1;
  q = mkSeq("q", {g}); EQ(AutoErr::Ok, checkSeq(q));
  g.cond.forS = 5; q = mkSeq("q", {g}); EQ(AutoErr::Duration, checkSeq(q));                     // no durations in a guard
  g.cond.forS = 0; strcpy(g.cond.device, "nope"); q = mkSeq("q", {g}); EQ(AutoErr::Device, checkSeq(q));
  g.hasGuard = false; q = mkSeq("q", {g}); EQ(AutoErr::Ok, checkSeq(q));                        // an unused bad guard is ignored
}

void test_validate_sequence_loop() {
  SeqDef q = mkSeq("q", {doStep(ActType::On, "r1"), doStep(ActType::Off, "r1")}, 3);
  EQ(AutoErr::Loop, checkSeq(q));                                           // repeats with no waiting would spin
  q.repeat = 0; EQ(AutoErr::Loop, checkSeq(q));
  q.repeat = 1; EQ(AutoErr::Ok, checkSeq(q));                               // once is fine
  q = mkSeq("q", {doStep(ActType::On, "r1"), waitStep(1)}, 0); EQ(AutoErr::Ok, checkSeq(q));
  q = mkSeq("q", {untilStep("t1", Prop::Temp, Op::Lt, 25, 5)}, 255); EQ(AutoErr::Ok, checkSeq(q));
}

void test_validate_timer() {
  TimerDef t = mkTimer("tm", 60);
  EQ(AutoErr::Ok, validateTimer(t));
  t.durationS = 0; EQ(AutoErr::Timer, validateTimer(t));
  t.durationS = 86400; EQ(AutoErr::Ok, validateTimer(t));
  t.durationS = 86401; EQ(AutoErr::Timer, validateTimer(t));
  t = mkTimer("Bad Id", 5); EQ(AutoErr::Id, validateTimer(t));
  t = mkTimer("tm", 5); memset(t.name, 'x', kRuleNameCap); EQ(AutoErr::Name, validateTimer(t));
}

void test_seq_store() {
  AutomationCore core;
  SeqDef a = mkSeq("a", {doStep(ActType::On, "r1"), waitStep(1)});
  EQ(AutoErr::Ok, core.addSeq(a));
  EQ(AutoErr::Duplicate, core.addSeq(a));
  EQ(1, core.seqCount()); EQ(2, core.stepsUsed());
  EQ(0, core.findSeq("a")); EQ(-1, core.findSeq("zz"));
  SeqDef out;
  core.seqDef(0, out);
  TEST_ASSERT_EQUAL_STRING("a", out.id); EQ(2, out.nSteps); EQ(StepType::Wait, out.steps[1].type); EQ(1, out.steps[1].secs);
  SeqDef b = mkSeq("b", {waitStep(5)});
  EQ(AutoErr::Ok, core.addSeq(b));
  SeqDef a2 = mkSeq("a", {doStep(ActType::Off, "r1")});
  EQ(AutoErr::Ok, core.replaceSeq(a2));
  EQ(2, core.seqCount()); EQ(2, core.stepsUsed());                           // a now 1 step, b 1 step
  EQ(AutoErr::NotFound, core.replaceSeq(mkSeq("zz", {waitStep(1)})));
  TEST_ASSERT_TRUE(core.removeSeq("a"));
  EQ(1, core.seqCount()); EQ(0, core.findSeq("b"));
  TEST_ASSERT_FALSE(core.removeSeq("a"));
  core.seqDef(0, out);
  EQ(5, out.steps[0].secs);                                                  // b survived the shift intact
}

void test_seq_limits() {
  AutomationCore core;
  SeqDef big;
  strcpy(big.id, "big"); big.nSteps = kMaxSteps;
  for (size_t i = 0; i < kMaxSteps; i++) big.steps[i] = waitStep(1);
  EQ(AutoErr::Ok, core.addSeq(big));
  strcpy(big.id, "big2");
  EQ(AutoErr::Ok, core.addSeq(big));                                        // 32 steps: the pool is full
  EQ(kMaxStepsTotal, core.stepsUsed());
  EQ(AutoErr::TooMany, core.addSeq(mkSeq("one", {waitStep(1)})));           // not even one more step
  SeqDef smaller = mkSeq("big", {waitStep(1), waitStep(1)});
  EQ(AutoErr::Ok, core.replaceSeq(smaller));                                // shrinking frees room
  EQ(AutoErr::Ok, core.addSeq(mkSeq("one", {waitStep(1)})));
  EQ(AutoErr::Ok, core.addSeq(mkSeq("two", {waitStep(1)})));
  EQ(kMaxSeqs, core.seqCount());
  EQ(AutoErr::TooMany, core.addSeq(mkSeq("three", {waitStep(1)})));         // at most four sequences
  SeqDef grow = mkSeq("big", {});
  grow.nSteps = kMaxSteps;
  for (size_t i = 0; i < kMaxSteps; i++) grow.steps[i] = waitStep(1);
  EQ(AutoErr::TooMany, core.replaceSeq(grow));                              // growing past the pool is refused
}

void test_sequence_runs() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), waitStep(2), doStep(ActType::On, "r2"), waitStep(1), doStep(ActType::Set, "p1", 30)}));
  TEST_ASSERT_TRUE(core.requestCtl(AutomationCore::Ctl::SeqStart, "s1"));
  core.tick(env, 100);
  EQ(1, env.ns);                                                            // step 1 at once
  EQ(1, env.count("r1"));
  EQ(RS::Running, core.seqRun(0).st);
  run(core, env, 200, 2000);
  EQ(1, env.ns);                                                            // still in the 2 s wait
  run(core, env, 2100, 2300);
  EQ(2, env.ns); EQ(1, env.count("r2"));                                    // two seconds after the first wait started
  run(core, env, 2400, 3000);
  EQ(2, env.ns);
  run(core, env, 3100, 3500);
  EQ(3, env.ns); EQ(1, env.count("p1"));
  run(core, env, 3600, 4000);
  EQ(RS::Idle, core.seqRun(0).st);
  EQ(1, core.seqRun(0).done);
  EQ(EvKind::SeqDone, core.eventAt(0).kind);
  TEST_ASSERT_EQUAL_STRING("s1", core.eventAt(0).rule);
}

void test_sequence_start_ignored_while_running() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), waitStep(5)}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 1000);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");                     // already going
  run(core, env, 1100, 2000);
  EQ(1, env.ns);
  run(core, env, 2100, 7000);
  EQ(RS::Idle, core.seqRun(0).st);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");                     // and again after it finished
  run(core, env, 7100, 7500);
  EQ(2, env.ns);
  EQ(1, core.seqRun(0).done);
}

void test_sequence_stop() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), waitStep(10), doStep(ActType::On, "r2")}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 3000);
  core.requestCtl(AutomationCore::Ctl::SeqStop, "s1");
  run(core, env, 3100, 20000);
  EQ(1, env.ns);                                                            // the second step never ran
  EQ(RS::Idle, core.seqRun(0).st);
  EQ(1, core.seqRun(0).stopped); EQ(0, core.seqRun(0).done);
  EQ(EvKind::SeqStopped, core.eventAt(0).kind);
  TEST_ASSERT_EQUAL_STRING("stopped", core.eventAt(0).text);
  core.requestCtl(AutomationCore::Ctl::SeqStop, "s1");                      // stopping an idle one is harmless
  core.tick(env, 21000);
  EQ(1, core.seqRun(0).stopped);
}

void test_sequence_pause_resume() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), waitStep(10), doStep(ActType::On, "r2")}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 4000);                                                // 4 s into the wait
  core.requestCtl(AutomationCore::Ctl::SeqPause, "s1");
  run(core, env, 4100, 30000);
  EQ(1, env.ns);                                                            // paused: nothing happens
  EQ(RS::Paused, core.seqRun(0).st);
  core.requestCtl(AutomationCore::Ctl::SeqResume, "s1");
  run(core, env, 30100, 35000);
  EQ(1, env.ns);                                                            // about 6 s of the wait are left
  run(core, env, 35100, 36500);
  EQ(2, env.ns);
  EQ(RS::Idle, core.seqRun(0).st);
}

void test_sequence_pause_only_when_running() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {waitStep(5)}));
  core.requestCtl(AutomationCore::Ctl::SeqPause, "s1");
  core.requestCtl(AutomationCore::Ctl::SeqResume, "s1");
  core.tick(env, 100);
  EQ(RS::Idle, core.seqRun(0).st);                                          // neither does anything to an idle sequence
}

void test_sequence_repeat() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::Toggle, "r1"), waitStep(1)}, 3));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 10000);
  EQ(3, env.ns);                                                            // three passes
  EQ(1, core.seqRun(0).done);
  EQ(RS::Idle, core.seqRun(0).st);
}

void test_sequence_repeat_forever_until_stopped() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::Toggle, "r1"), waitStep(1)}, 0));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 10000);
  TEST_ASSERT_TRUE(env.ns >= 8);
  EQ(RS::Running, core.seqRun(0).st);
  core.requestCtl(AutomationCore::Ctl::SeqStop, "s1");
  size_t n = env.ns;
  run(core, env, 10100, 20000);
  EQ(n, env.ns);
  EQ(0, core.seqRun(0).done);
}

void test_sequence_steps_per_pass() {
  AutomationCore core; Fake env;
  SeqDef q;
  strcpy(q.id, "s1"); q.nSteps = 12;
  for (size_t i = 0; i < 12; i++) q.steps[i] = doStep(ActType::Toggle, "r1");
  core.addSeq(q);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  core.tick(env, 100);
  EQ(AutomationCore::kStepsPerPass, env.ns);                                // a long run of steps is spread over passes
  core.tick(env, 200);
  EQ(12, env.ns);
  core.tick(env, 300);
  EQ(RS::Idle, core.seqRun(0).st);
}

void test_sequence_until() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), untilStep("t1", Prop::Temp, Op::Lt, 25, 30), doStep(ActType::On, "r2")}));
  env.set("t1", Prop::Temp, 40);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 5000);
  EQ(1, env.ns);                                                            // waiting for it to cool down
  env.set("t1", Prop::Temp, 20);
  run(core, env, 5100, 5400);
  EQ(2, env.ns);
  EQ(1, env.count("r2"));
  EQ(RS::Idle, core.seqRun(0).st);
}

void test_sequence_until_already_true() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {untilStep("t1", Prop::Temp, Op::Lt, 25, 30), doStep(ActType::On, "r2")}));
  env.set("t1", Prop::Temp, 10);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  core.tick(env, 100);
  EQ(1, env.ns);                                                            // no waiting when it already holds
}

void test_sequence_until_timeout_continues() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {untilStep("t1", Prop::Temp, Op::Lt, 25, 5), doStep(ActType::On, "r2")}));
  env.set("t1", Prop::Temp, 40);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 5000);
  EQ(0, env.ns);
  run(core, env, 5100, 5500);
  EQ(1, env.ns);                                                            // timed out after 5 s and carried on
  bool timeout = false;
  for (size_t i = 0; i < core.eventCount(); i++) if (core.eventAt(i).kind == EvKind::Failed && !strcmp(core.eventAt(i).text, "timeout")) timeout = true;
  TEST_ASSERT_TRUE(timeout);
  EQ(1, core.seqRun(0).done);
}

void test_sequence_until_timeout_stops() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {untilStep("t1", Prop::Temp, Op::Lt, 25, 5, true), doStep(ActType::On, "r2")}));
  env.set("t1", Prop::Temp, 40);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 8000);
  EQ(0, env.ns);                                                            // stopped: the next step never ran
  EQ(RS::Idle, core.seqRun(0).st);
  EQ(1, core.seqRun(0).stopped); EQ(0, core.seqRun(0).done);
  TEST_ASSERT_EQUAL_STRING("timeout", core.eventAt(0).text);
}

void test_sequence_pause_during_until() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {untilStep("t1", Prop::Temp, Op::Lt, 25, 10, true), doStep(ActType::On, "r2")}));
  env.set("t1", Prop::Temp, 40);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 6000);                                                // 6 s of the 10 s used
  core.requestCtl(AutomationCore::Ctl::SeqPause, "s1");
  run(core, env, 6100, 60000);
  EQ(RS::Paused, core.seqRun(0).st);                                        // no timeout while paused
  core.requestCtl(AutomationCore::Ctl::SeqResume, "s1");
  run(core, env, 60100, 63000);
  EQ(RS::Running, core.seqRun(0).st);                                       // about 4 s left
  run(core, env, 63100, 65000);
  EQ(RS::Idle, core.seqRun(0).st);
  EQ(1, core.seqRun(0).stopped);
}

void test_sequence_guard_skip_and_stop() {
  Step skip = doStep(ActType::On, "r1");
  skip.hasGuard = true; skip.cond.kind = CondKind::Compare; strcpy(skip.cond.device, "t1"); skip.cond.prop = Prop::Temp; skip.cond.op = Op::Gt; skip.cond.value = 30;
  Step stop = skip;
  stop.stopOnFalse = true;
  {
    AutomationCore core; Fake env;
    core.addSeq(mkSeq("s1", {skip, doStep(ActType::On, "r2")}));
    env.set("t1", Prop::Temp, 20);
    core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
    run(core, env, 100, 500);
    EQ(0, env.count("r1")); EQ(1, env.count("r2"));                         // the guarded step was skipped, the next ran
    EQ(1, core.seqRun(0).done);
  }
  {
    AutomationCore core; Fake env;
    core.addSeq(mkSeq("s1", {skip, doStep(ActType::On, "r2")}));
    env.set("t1", Prop::Temp, 40);
    core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
    run(core, env, 100, 500);
    EQ(1, env.count("r1")); EQ(1, env.count("r2"));                         // a true guard runs the step
  }
  {
    AutomationCore core; Fake env;
    core.addSeq(mkSeq("s1", {stop, doStep(ActType::On, "r2")}));
    env.set("t1", Prop::Temp, 20);
    core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
    run(core, env, 100, 500);
    EQ(0, env.ns);                                                          // a false guard stops the sequence
    EQ(1, core.seqRun(0).stopped);
    TEST_ASSERT_EQUAL_STRING("guard", core.eventAt(0).text);
  }
}

void test_sequence_time_guard() {
  AutomationCore core; Fake env;
  Step s = doStep(ActType::On, "r1");
  s.hasGuard = true; s.cond.kind = CondKind::Time; s.cond.from = 8 * 60; s.cond.to = 20 * 60; s.cond.days = 0x7F;
  core.addSeq(mkSeq("s1", {s}));
  env.haveClock = true; env.clk.weekday = 2; env.clk.minuteOfDay = 21 * 60;
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 500);
  EQ(0, env.ns);                                                            // outside the window
  env.clk.minuteOfDay = 12 * 60;
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 600, 1000);
  EQ(1, env.ns);
}

void test_sequence_failed_step_goes_on() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), doStep(ActType::On, "r2")}));
  env.cmdResult = CmdErr::QueueFull;
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 500);
  EQ(1, core.seqRun(0).done);                                               // a failed step is logged, the sequence carries on
  size_t failed = 0;
  for (size_t i = 0; i < core.eventCount(); i++) if (core.eventAt(i).kind == EvKind::Failed) failed++;
  EQ(2, failed);
}

void test_sequence_interlock_and_missing_device() {
  AutomationCore core; Fake env;
  core.settings.nLocks = 1;
  strcpy(core.settings.locks[0].a, "r1"); strcpy(core.settings.locks[0].b, "r2");
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), doStep(ActType::On, "removed")}));
  env.set("r2", Prop::State, 1);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 500);
  EQ(0, env.ns);                                                            // the interlock applies inside sequences too
  bool blocked = false, failed = false;
  for (size_t i = 0; i < core.eventCount(); i++) { if (core.eventAt(i).kind == EvKind::Blocked) blocked = true; if (core.eventAt(i).kind == EvKind::Failed) failed = true; }
  TEST_ASSERT_TRUE(blocked); TEST_ASSERT_TRUE(failed);
}

void test_sequence_replace_stops_it() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {waitStep(10), doStep(ActType::On, "r1")}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 1000);
  EQ(RS::Running, core.seqRun(0).st);
  core.replaceSeq(mkSeq("s1", {doStep(ActType::On, "r2")}));
  EQ(RS::Idle, core.seqRun(0).st);
  run(core, env, 1100, 20000);
  EQ(0, env.ns);                                                            // the old run is gone
}

void test_sequence_control_unknown() {
  AutomationCore core;
  TEST_ASSERT_FALSE(core.requestCtl(AutomationCore::Ctl::SeqStart, "nope"));
  TEST_ASSERT_FALSE(core.requestCtl(AutomationCore::Ctl::TimerStart, "nope"));
  core.addSeq(mkSeq("s1", {waitStep(1)}));
  TEST_ASSERT_FALSE(core.requestCtl(AutomationCore::Ctl::TimerStart, "s1"));   // a sequence id is not a timer
  TEST_ASSERT_TRUE(core.requestCtl(AutomationCore::Ctl::SeqStart, "s1"));
  for (int i = 0; i < 10; i++) core.requestCtl(AutomationCore::Ctl::SeqStop, "s1");   // the queue is small and does not overflow
}

void test_timer_runs() {
  AutomationCore core; Fake env;
  EQ(AutoErr::Ok, core.addTimer(mkTimer("tm", 10)));
  EQ(AutoErr::Duplicate, core.addTimer(mkTimer("tm", 5)));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  core.tick(env, 100);
  TEST_ASSERT_TRUE(core.timerRun(0).running);
  EQ(10000, core.timerRemainingMs(0, 100));
  EQ(5000, core.timerRemainingMs(0, 5100));
  run(core, env, 200, 10000);
  TEST_ASSERT_TRUE(core.timerRun(0).running);                               // 100 + 10000 = due at 10.1 s
  run(core, env, 10100, 10300);
  TEST_ASSERT_FALSE(core.timerRun(0).running);
  EQ(1, core.timerRun(0).finished);
  EQ(0, core.timerRemainingMs(0, 10300));
  EQ(EvKind::TimerDone, core.eventAt(0).kind);
  TEST_ASSERT_EQUAL_STRING("tm", core.eventAt(0).rule);
}

void test_timer_stop_and_restart() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 10));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  run(core, env, 100, 6000);
  core.requestCtl(AutomationCore::Ctl::TimerStop, "tm");
  run(core, env, 6100, 30000);
  EQ(0, core.timerRun(0).finished);                                         // stopped before it finished
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  run(core, env, 30100, 36000);
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");                   // starting a running timer restarts it
  run(core, env, 36100, 45000);
  EQ(0, core.timerRun(0).finished);
  run(core, env, 45100, 46500);
  EQ(1, core.timerRun(0).finished);
}

void test_timer_repeat() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 5, true));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  run(core, env, 100, 21000);
  EQ(4, core.timerRun(0).finished);                                         // 5, 10, 15, 20 s
  TEST_ASSERT_TRUE(core.timerRun(0).running);
  core.tick(env, 900000);                                                   // a long stall: one finish, no burst
  EQ(5, core.timerRun(0).finished);
}

void test_timer_store() {
  AutomationCore core;
  for (size_t i = 0; i < kMaxTimers; i++) { char id[8]; snprintf(id, sizeof id, "t%u", unsigned(i)); EQ(AutoErr::Ok, core.addTimer(mkTimer(id, 5))); }
  EQ(AutoErr::TooMany, core.addTimer(mkTimer("extra", 5)));
  EQ(AutoErr::Ok, core.replaceTimer(mkTimer("t2", 99)));
  EQ(99, core.timer(2).durationS);
  EQ(AutoErr::NotFound, core.replaceTimer(mkTimer("zz", 5)));
  TEST_ASSERT_TRUE(core.removeTimer("t0"));
  EQ(kMaxTimers - 1, core.timerCount());
  EQ(0, core.findTimer("t1"));
  TEST_ASSERT_FALSE(core.removeTimer("t0"));
}

void test_timer_replace_stops_it() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 10));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  core.tick(env, 100);
  core.replaceTimer(mkTimer("tm", 20));
  TEST_ASSERT_FALSE(core.timerRun(0).running);
}

void test_rule_starts_and_stops_things() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r1"), waitStep(30)}));
  core.addTimer(mkTimer("tm", 60));
  Rule go = base("go", TrigType::Manual);
  go.nActs = 2;
  go.acts[0].type = ActType::SeqStart; strcpy(go.acts[0].device, "s1");
  go.acts[1].type = ActType::TimerStart; strcpy(go.acts[1].device, "tm");
  Rule halt = base("halt", TrigType::Manual);
  halt.nActs = 2;
  halt.acts[0].type = ActType::SeqStop; strcpy(halt.acts[0].device, "s1");
  halt.acts[1].type = ActType::TimerStop; strcpy(halt.acts[1].device, "tm");
  core.add(go); core.add(halt);
  core.requestRun("go");
  run(core, env, 100, 1000);
  EQ(RS::Running, core.seqRun(0).st);
  TEST_ASSERT_TRUE(core.timerRun(0).running);
  EQ(1, env.count("r1"));
  core.requestRun("halt");
  run(core, env, 1100, 2000);
  EQ(RS::Idle, core.seqRun(0).st);
  TEST_ASSERT_FALSE(core.timerRun(0).running);
  EQ(1, core.seqRun(0).stopped);
}

void test_rule_action_on_missing_target() {
  AutomationCore core; Fake env;
  Rule go = base("go", TrigType::Manual);
  go.acts[0].type = ActType::SeqStart; strcpy(go.acts[0].device, "gone");
  core.add(go);
  core.requestRun("go");
  core.tick(env, 100);
  EQ(Last::NoDevice, core.state(0).last);
  EQ(1, core.state(0).failed);
}

void test_timer_trigger() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 5));
  Rule r = base("on_done", TrigType::Timer); strcpy(r.trig.device, "tm");
  core.add(r);
  run(core, env, 0, 1000);
  EQ(0, env.ns);
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  run(core, env, 1100, 6000);
  EQ(0, env.ns);
  run(core, env, 6100, 6500);
  EQ(1, env.ns);                                                            // the rule ran once the timer finished
  run(core, env, 6600, 9000);
  EQ(1, env.ns);
}

void test_timer_trigger_ignores_the_past() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 1));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  run(core, env, 0, 3000);
  EQ(1, core.timerRun(0).finished);
  Rule r = base("late", TrigType::Timer); strcpy(r.trig.device, "tm");
  core.add(r);                                                              // added after it already finished once
  run(core, env, 3100, 5000);
  EQ(0, env.ns);
}

void test_sequence_trigger_done_and_stopped() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {doStep(ActType::On, "r2"), waitStep(2)}));
  Rule done = base("done", TrigType::Sequence); strcpy(done.trig.device, "s1"); strcpy(done.acts[0].device, "r1");
  Rule stopped = base("stopped", TrigType::Sequence); strcpy(stopped.trig.device, "s1"); stopped.trig.onStopped = true; strcpy(stopped.acts[0].device, "p1");
  stopped.acts[0].type = ActType::Set; stopped.acts[0].value = 10;
  core.add(done); core.add(stopped);
  run(core, env, 0, 500);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 600, 4000);
  EQ(1, env.count("r1")); EQ(0, env.count("p1"));                           // finished normally
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 4100, 4700);
  core.requestCtl(AutomationCore::Ctl::SeqStop, "s1");
  run(core, env, 4800, 6000);
  EQ(1, env.count("r1")); EQ(1, env.count("p1"));                           // stopped early
}

void test_sequence_removed_while_trigger_watches() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {waitStep(1)}));
  Rule r = base("w", TrigType::Sequence); strcpy(r.trig.device, "s1");
  core.add(r);
  run(core, env, 0, 500);
  core.removeSeq("s1");
  run(core, env, 600, 3000);
  EQ(0, env.ns);                                                            // a trigger on a missing sequence just waits
}

void test_engine_off_pauses_sequences() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {waitStep(1), doStep(ActType::On, "r1")}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  core.tick(env, 100);
  core.settings.enabled = false;
  run(core, env, 200, 5000);
  EQ(0, env.ns);                                                            // the engine switch freezes everything
  core.settings.enabled = true;
  run(core, env, 5100, 5500);
  EQ(1, env.ns);
}

// ---- extra checks found by the P7b mutation run

void test_names_are_the_wire_names() {
  TEST_ASSERT_EQUAL_STRING("do", stepName(StepType::Do));
  TEST_ASSERT_EQUAL_STRING("wait", stepName(StepType::Wait));
  TEST_ASSERT_EQUAL_STRING("until", stepName(StepType::Until));
  StepType t;
  TEST_ASSERT_TRUE(parseStep("wait", t)); EQ(StepType::Wait, t);
  TEST_ASSERT_TRUE(parseStep("until", t)); EQ(StepType::Until, t);
  TEST_ASSERT_TRUE(parseStep("do", t)); EQ(StepType::Do, t);
  TEST_ASSERT_EQUAL_STRING("sequence_done", evKindName(EvKind::SeqDone));
  TEST_ASSERT_EQUAL_STRING("sequence_stopped", evKindName(EvKind::SeqStopped));
  TEST_ASSERT_EQUAL_STRING("timer_done", evKindName(EvKind::TimerDone));
  TEST_ASSERT_EQUAL_STRING("fired", evKindName(EvKind::Fired));
  TrigType tt; ActType at;
  TEST_ASSERT_TRUE(parseTrig("timer", tt)); EQ(7, tt);
  TEST_ASSERT_TRUE(parseTrig("sequence", tt)); EQ(8, tt);
  TEST_ASSERT_TRUE(parseAct("seq_start", at)); EQ(7, at);
  TEST_ASSERT_TRUE(parseAct("seq_stop", at)); EQ(8, at);
  TEST_ASSERT_TRUE(parseAct("timer_start", at)); EQ(9, at);
  TEST_ASSERT_TRUE(parseAct("timer_stop", at)); EQ(10, at);
}

void test_remove_keeps_other_run_state() {
  {
    AutomationCore core; Fake env;
    core.addSeq(mkSeq("a", {waitStep(30)}));
    core.addSeq(mkSeq("b", {waitStep(30)}));
    core.requestCtl(AutomationCore::Ctl::SeqStart, "b");
    core.tick(env, 100);
    EQ(RS::Running, core.seqRun(1).st);
    core.removeSeq("a");
    EQ(0, core.findSeq("b"));
    EQ(RS::Running, core.seqRun(0).st);                                 // b kept running after moving down
  }
  {
    AutomationCore core; Fake env;
    core.addTimer(mkTimer("a", 30));
    core.addTimer(mkTimer("b", 30));
    core.requestCtl(AutomationCore::Ctl::TimerStart, "b");
    core.tick(env, 100);
    core.removeTimer("a");
    TEST_ASSERT_TRUE(core.timerRun(0).running);
  }
}

void test_timer_remaining_never_negative() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 5));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  core.tick(env, 100);                                                  // ends at 5100
  EQ(0, core.timerRemainingMs(0, 5100));
  EQ(0, core.timerRemainingMs(0, 6000));                                // due but not yet noticed by a tick
  EQ(1, core.timerRemainingMs(0, 5099));
}

void test_ctl_queue_full() {
  AutomationCore core;
  core.addSeq(mkSeq("s1", {waitStep(1)}));
  for (size_t i = 0; i < AutomationCore::kCtlSlots; i++) TEST_ASSERT_TRUE(core.requestCtl(AutomationCore::Ctl::SeqStop, "s1"));
  TEST_ASSERT_FALSE(core.requestCtl(AutomationCore::Ctl::SeqStop, "s1"));   // no room: refused, nothing written past the end
}

void test_restart_keeps_counters() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {waitStep(30)}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  core.tick(env, 100);
  core.requestCtl(AutomationCore::Ctl::SeqStop, "s1");
  core.tick(env, 200);
  EQ(1, core.seqRun(0).stopped);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  core.tick(env, 300);
  EQ(RS::Running, core.seqRun(0).st);
  EQ(1, core.seqRun(0).stopped);                                        // the history survives a restart
}

void test_pause_twice_keeps_the_first_remainder() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {waitStep(10), doStep(ActType::On, "r1")}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 4000);                                            // 6 s of the wait left (it ends at 10.1 s)
  core.requestCtl(AutomationCore::Ctl::SeqPause, "s1");
  core.tick(env, 4100);
  core.tick(env, 9000);
  core.requestCtl(AutomationCore::Ctl::SeqPause, "s1");                 // a second pause changes nothing
  core.tick(env, 9100);
  core.requestCtl(AutomationCore::Ctl::SeqResume, "s1");
  run(core, env, 9200, 14000);
  EQ(0, env.ns);                                                        // about 6 s still to wait from the resume
  run(core, env, 14100, 16000);
  EQ(1, env.ns);
}

void test_timer_repeat_keeps_its_phase() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 5, true));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  core.tick(env, 100);                                                  // due at 5100
  core.tick(env, 5150);                                                 // noticed 50 ms late
  EQ(1, core.timerRun(0).finished);
  core.tick(env, 10100);                                                // the next one is still due at 10100
  EQ(2, core.timerRun(0).finished);
}

void test_timer_repeat_does_not_burst() {
  AutomationCore core; Fake env;
  core.addTimer(mkTimer("tm", 5, true));
  core.requestCtl(AutomationCore::Ctl::TimerStart, "tm");
  core.tick(env, 100);
  core.tick(env, 900000);                                               // a long stall
  EQ(1, core.timerRun(0).finished);
  core.tick(env, 900100);
  core.tick(env, 900200);
  EQ(1, core.timerRun(0).finished);                                     // one finish, not one per catch-up step
}

void test_consecutive_until_steps_each_wait() {
  AutomationCore core; Fake env;
  core.addSeq(mkSeq("s1", {untilStep("t1", Prop::Temp, Op::Lt, 25, 5), untilStep("t1", Prop::Temp, Op::Lt, 10, 5), doStep(ActType::On, "r1")}));
  env.set("t1", Prop::Temp, 40);
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 5000);
  EQ(0, env.ns);
  run(core, env, 5100, 5400);                                           // the first times out at 5 s
  EQ(0, env.ns);
  run(core, env, 5500, 9900);
  EQ(0, env.ns);                                                        // the second has its own full 5 s
  run(core, env, 10000, 10600);
  EQ(1, env.ns);
}

void test_step_events_and_timer_steps() {
  AutomationCore core; Fake env;
  Step ev; ev.type = StepType::Do; ev.act.type = ActType::Event; strcpy(ev.act.text, "hello");
  core.addTimer(mkTimer("tm", 60));
  core.addSeq(mkSeq("s1", {ev, doStep(ActType::TimerStart, "tm"), doStep(ActType::TimerStart, "gone")}));
  core.requestCtl(AutomationCore::Ctl::SeqStart, "s1");
  run(core, env, 100, 500);
  bool custom = false, failed = false;
  for (size_t i = 0; i < core.eventCount(); i++) {
    if (core.eventAt(i).kind == EvKind::Custom && !strcmp(core.eventAt(i).text, "hello")) custom = true;
    if (core.eventAt(i).kind == EvKind::Failed && !strcmp(core.eventAt(i).text, "gone")) failed = true;
  }
  TEST_ASSERT_TRUE(custom);                                             // the event step is logged
  TEST_ASSERT_TRUE(failed);                                             // a timer that does not exist is logged as a failure
  TEST_ASSERT_TRUE(core.timerRun(0).running);                           // and a real one was started
  EQ(1, core.seqRun(0).done);
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
  RUN_TEST(test_new_names);
  RUN_TEST(test_validate_rule_targets);
  RUN_TEST(test_validate_steps);
  RUN_TEST(test_validate_sequence_loop);
  RUN_TEST(test_validate_timer);
  RUN_TEST(test_seq_store);
  RUN_TEST(test_seq_limits);
  RUN_TEST(test_sequence_runs);
  RUN_TEST(test_sequence_start_ignored_while_running);
  RUN_TEST(test_sequence_stop);
  RUN_TEST(test_sequence_pause_resume);
  RUN_TEST(test_sequence_pause_only_when_running);
  RUN_TEST(test_sequence_repeat);
  RUN_TEST(test_sequence_repeat_forever_until_stopped);
  RUN_TEST(test_sequence_steps_per_pass);
  RUN_TEST(test_sequence_until);
  RUN_TEST(test_sequence_until_already_true);
  RUN_TEST(test_sequence_until_timeout_continues);
  RUN_TEST(test_sequence_until_timeout_stops);
  RUN_TEST(test_sequence_pause_during_until);
  RUN_TEST(test_sequence_guard_skip_and_stop);
  RUN_TEST(test_sequence_time_guard);
  RUN_TEST(test_sequence_failed_step_goes_on);
  RUN_TEST(test_sequence_interlock_and_missing_device);
  RUN_TEST(test_sequence_replace_stops_it);
  RUN_TEST(test_sequence_control_unknown);
  RUN_TEST(test_timer_runs);
  RUN_TEST(test_timer_stop_and_restart);
  RUN_TEST(test_timer_repeat);
  RUN_TEST(test_timer_store);
  RUN_TEST(test_timer_replace_stops_it);
  RUN_TEST(test_rule_starts_and_stops_things);
  RUN_TEST(test_rule_action_on_missing_target);
  RUN_TEST(test_timer_trigger);
  RUN_TEST(test_timer_trigger_ignores_the_past);
  RUN_TEST(test_sequence_trigger_done_and_stopped);
  RUN_TEST(test_sequence_removed_while_trigger_watches);
  RUN_TEST(test_engine_off_pauses_sequences);
  RUN_TEST(test_names_are_the_wire_names);
  RUN_TEST(test_remove_keeps_other_run_state);
  RUN_TEST(test_timer_remaining_never_negative);
  RUN_TEST(test_ctl_queue_full);
  RUN_TEST(test_restart_keeps_counters);
  RUN_TEST(test_pause_twice_keeps_the_first_remainder);
  RUN_TEST(test_timer_repeat_keeps_its_phase);
  RUN_TEST(test_timer_repeat_does_not_burst);
  RUN_TEST(test_consecutive_until_steps_each_wait);
  RUN_TEST(test_step_events_and_timer_steps);
  return UNITY_END();
}
