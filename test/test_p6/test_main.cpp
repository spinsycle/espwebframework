#include <initializer_list>
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "dashboard/DashboardRules.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

#define EQ(a, b) TEST_ASSERT_EQUAL(int(a), int(b))

// ---- a small registry of devices for the lookups

static DeviceConfig mk(const char* id, DeviceType t, uint8_t model = 0) {
  DeviceConfig c;
  strcpy(c.id, id);
  c.type = t;
  c.model = model;
  return c;
}
static DeviceConfig gDevs[] = {
    mk("r1", DeviceType::Relay),  mk("in1", DeviceType::Input), mk("p1", DeviceType::Pwm),
    mk("m1", DeviceType::Motor),  mk("s1", DeviceType::Servo),  mk("t1", DeviceType::Sensor, 0),
    mk("env", DeviceType::Sensor, 1), mk("ser", DeviceType::Serial), mk("r2", DeviceType::Relay),
};
static bool find(void*, const char* id, DeviceConfig& out) {
  for (const DeviceConfig& d : gDevs)
    if (!strcmp(d.id, id)) { out = d; return true; }
  return false;
}

static DashComp comp(const char* id, CompType t, const char* dev, Prop p) {
  DashComp c;
  strcpy(c.id, id);
  c.type = t;
  strcpy(c.device, dev);
  c.prop = p;
  c.col = 1; c.row = 1; c.w = 1; c.h = 1;
  return c;
}
static DashErr check(const DashComp& c, size_t nTabs = 0) { return validateComponent(c, find, nullptr, nTabs); }

// ---- device properties

void test_device_props() {
  PropAccess a[5];
  EQ(1, deviceProps(gDevs[0], a));  // relay
  EQ(Prop::State, a[0].prop); TEST_ASSERT_TRUE(a[0].write);
  EQ(2, deviceProps(gDevs[1], a));  // input: read only
  TEST_ASSERT_FALSE(a[0].write); TEST_ASSERT_FALSE(a[1].write);
  for (int i : {2, 3, 4}) { EQ(1, deviceProps(gDevs[i], a)); EQ(Prop::Level, a[0].prop); TEST_ASSERT_TRUE(a[0].write); }
  EQ(2, deviceProps(gDevs[5], a));  // ds18b20: temperature and ok
  EQ(4, deviceProps(gDevs[6], a));  // bme280 adds pressure and humidity
  EQ(2, deviceProps(gDevs[7], a));  // serial: text, count
  EQ(Prop::Text, a[0].prop);
}

void test_prop_names_round_trip() {
  for (uint8_t i = 1; i < uint8_t(Prop::kCount); i++) {
    Prop p;
    TEST_ASSERT_TRUE(parseProp(propName(Prop(i)), p));
    EQ(i, p);
  }
  Prop p;
  TEST_ASSERT_FALSE(parseProp("nope", p));
  TEST_ASSERT_FALSE(parseProp(nullptr, p));
  TEST_ASSERT_FALSE(parseProp("", p));
}

void test_comp_names_round_trip() {
  for (uint8_t i = 0; i < uint8_t(CompType::kCount); i++) {
    CompType t;
    TEST_ASSERT_TRUE(parseCompType(compTypeName(CompType(i)), t));
    EQ(i, t);
  }
  CompType t;
  TEST_ASSERT_TRUE(parseCompType("timer", t));
  EQ(CompType::Timer, t);
  TEST_ASSERT_FALSE(parseCompType("clock", t));
  TEST_ASSERT_FALSE(parseCompType("", t));
  EQ(14, CompType::kCount);
}

void test_levels() {
  int lo, hi;
  TEST_ASSERT_TRUE(levelRange(DeviceType::Pwm, lo, hi)); EQ(0, lo); EQ(100, hi);
  TEST_ASSERT_TRUE(levelRange(DeviceType::Motor, lo, hi)); EQ(-100, lo); EQ(100, hi);
  TEST_ASSERT_TRUE(levelRange(DeviceType::Servo, lo, hi)); EQ(0, lo); EQ(180, hi);
  TEST_ASSERT_FALSE(levelRange(DeviceType::Relay, lo, hi));
}

// ---- which component takes which property

void test_need_accepts() {
  TEST_ASSERT_TRUE(needAccepts(Need::WriteState, Prop::State, true));
  TEST_ASSERT_FALSE(needAccepts(Need::WriteState, Prop::State, false));  // an input cannot be toggled
  TEST_ASSERT_FALSE(needAccepts(Need::WriteState, Prop::Level, true));
  TEST_ASSERT_TRUE(needAccepts(Need::WriteLevel, Prop::Level, true));
  TEST_ASSERT_FALSE(needAccepts(Need::WriteLevel, Prop::Level, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadNumber, Prop::Temp, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadNumber, Prop::Humidity, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadNumber, Prop::Pressure, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadNumber, Prop::Count, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadNumber, Prop::Level, true));
  TEST_ASSERT_FALSE(needAccepts(Need::ReadNumber, Prop::State, false));
  TEST_ASSERT_FALSE(needAccepts(Need::ReadNumber, Prop::Text, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadState, Prop::State, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadState, Prop::Ok, false));
  TEST_ASSERT_FALSE(needAccepts(Need::ReadState, Prop::Level, false));
  TEST_ASSERT_TRUE(needAccepts(Need::ReadText, Prop::Text, false));
  TEST_ASSERT_FALSE(needAccepts(Need::ReadText, Prop::Temp, false));
  TEST_ASSERT_FALSE(needAccepts(Need::Card, Prop::State, true));
}

void test_comp_needs() {
  EQ(Need::WriteState, compNeed(CompType::Toggle));
  EQ(Need::Action, compNeed(CompType::PushButton));
  EQ(Need::WriteLevel, compNeed(CompType::Slider));
  EQ(Need::WriteLevel, compNeed(CompType::Joystick));
  EQ(Need::WriteLevel, compNeed(CompType::Dropdown));
  EQ(Need::ReadNumber, compNeed(CompType::Gauge));
  EQ(Need::ReadNumber, compNeed(CompType::Numeric));
  EQ(Need::ReadNumber, compNeed(CompType::Progress));
  EQ(Need::ReadNumber, compNeed(CompType::Graph));
  EQ(Need::ReadState, compNeed(CompType::Status));
  EQ(Need::ReadText, compNeed(CompType::Text));
  EQ(Need::Card, compNeed(CompType::DeviceCard));
  EQ(Need::Devices, compNeed(CompType::Group));
  EQ(Need::Timer, compNeed(CompType::Timer));
}

// ---- validateComponent: bindings

void test_toggle() {
  EQ(DashErr::Ok, check(comp("a", CompType::Toggle, "r1", Prop::State)));
  EQ(DashErr::Prop, check(comp("a", CompType::Toggle, "in1", Prop::State)));   // an input is read only
  EQ(DashErr::Prop, check(comp("a", CompType::Toggle, "r1", Prop::Level)));    // a relay has no level
  EQ(DashErr::Prop, check(comp("a", CompType::Toggle, "r1", Prop::None)));
  EQ(DashErr::Device, check(comp("a", CompType::Toggle, "nope", Prop::State)));
  EQ(DashErr::Device, check(comp("a", CompType::Toggle, "", Prop::State)));
}

void test_slider_and_readouts() {
  EQ(DashErr::Ok, check(comp("a", CompType::Slider, "p1", Prop::Level)));
  EQ(DashErr::Ok, check(comp("a", CompType::Slider, "m1", Prop::Level)));
  EQ(DashErr::Ok, check(comp("a", CompType::Slider, "s1", Prop::Level)));
  EQ(DashErr::Prop, check(comp("a", CompType::Slider, "r1", Prop::State)));
  EQ(DashErr::Ok, check(comp("a", CompType::Gauge, "t1", Prop::Temp)));
  EQ(DashErr::Ok, check(comp("a", CompType::Numeric, "env", Prop::Humidity)));
  EQ(DashErr::Prop, check(comp("a", CompType::Numeric, "t1", Prop::Humidity)));  // a DS18B20 has none
  EQ(DashErr::Ok, check(comp("a", CompType::Progress, "p1", Prop::Level)));
  DashComp g = comp("a", CompType::Graph, "in1", Prop::Count);
  g.w = 2;
  EQ(DashErr::Ok, check(g));
  EQ(DashErr::Prop, check(comp("a", CompType::Gauge, "r1", Prop::State)));
  EQ(DashErr::Ok, check(comp("a", CompType::Status, "t1", Prop::Ok)));
  EQ(DashErr::Ok, check(comp("a", CompType::Status, "in1", Prop::State)));
  EQ(DashErr::Prop, check(comp("a", CompType::Status, "p1", Prop::Level)));
  EQ(DashErr::Ok, check(comp("a", CompType::Text, "ser", Prop::Text)));
  EQ(DashErr::Prop, check(comp("a", CompType::Text, "r1", Prop::State)));
}

static bool timerFind(void*, const char* id) { return !strcmp(id, "tm"); }

static bool anyTimer(void*, const char*) { return true; }

void test_timer_component_id_must_be_terminated() {
  DashComp t = comp("a", CompType::Timer, "tm", Prop::None);
  t.w = 2;
  memset(t.device, 'x', kIdCap);                                                // no terminator
  EQ(DashErr::Device, validateComponent(t, find, nullptr, 0, anyTimer, nullptr));
  strcpy(t.device, "tm");
  EQ(DashErr::Ok, validateComponent(t, find, nullptr, 0, anyTimer, nullptr));
}

void test_timer_component() {
  DashComp t = comp("a", CompType::Timer, "tm", Prop::None);
  t.w = 2;
  EQ(DashErr::Ok, validateComponent(t, find, nullptr, 0, timerFind, nullptr));
  EQ(DashErr::Device, validateComponent(t, find, nullptr, 0));                   // no timer table: refused
  strcpy(t.device, "gone");
  EQ(DashErr::Device, validateComponent(t, find, nullptr, 0, timerFind, nullptr));
  t.device[0] = 0;
  EQ(DashErr::Device, validateComponent(t, find, nullptr, 0, timerFind, nullptr));
  strcpy(t.device, "r1");                                                         // a device id is not a timer id
  EQ(DashErr::Device, validateComponent(t, find, nullptr, 0, timerFind, nullptr));
  strcpy(t.device, "tm");
  t.w = 1;
  EQ(DashErr::Layout, validateComponent(t, find, nullptr, 0, timerFind, nullptr));  // wide enough for a time and a button
  TEST_ASSERT_FALSE(needAccepts(Need::Timer, Prop::State, true));
  TEST_ASSERT_FALSE(needAccepts(Need::Timer, Prop::Level, false));
  TEST_ASSERT_EQUAL_STRING("timer", compTypeName(CompType::Timer));
  uint8_t w, h;
  compMinSize(CompType::Timer, w, h);
  EQ(2, w); EQ(1, h);
}

void test_device_card() {
  EQ(DashErr::Ok, check(comp("a", CompType::DeviceCard, "r1", Prop::None)));
  EQ(DashErr::Ok, check(comp("a", CompType::DeviceCard, "ser", Prop::None)));
  EQ(DashErr::Device, check(comp("a", CompType::DeviceCard, "gone", Prop::None)));
}

void test_push_button() {
  DashComp c = comp("a", CompType::PushButton, "r1", Prop::None);
  c.cmd.action = CmdAction::Toggle;
  EQ(DashErr::Ok, check(c));
  c.cmd.action = CmdAction::Set; c.cmd.hasValue = c.cmd.isBool = true; c.cmd.value = 1;
  EQ(DashErr::Ok, check(c));                       // relay on
  c.cmd.isBool = false; c.cmd.value = 1;
  EQ(DashErr::Action, check(c));                   // a relay wants true or false, not 1
  DashComp p = comp("a", CompType::PushButton, "p1", Prop::None);
  p.cmd.action = CmdAction::Set; p.cmd.hasValue = true; p.cmd.value = 100;
  EQ(DashErr::Ok, check(p));
  p.cmd.value = 101;
  EQ(DashErr::Action, check(p));
  DashComp m = comp("a", CompType::PushButton, "m1", Prop::None);
  m.cmd.action = CmdAction::Stop;
  EQ(DashErr::Ok, check(m));
  DashComp i = comp("a", CompType::PushButton, "in1", Prop::None);
  i.cmd.action = CmdAction::Toggle;
  EQ(DashErr::Action, check(i));                   // an input cannot be commanded
  DashComp s = comp("a", CompType::PushButton, "ser", Prop::None);
  s.cmd.action = CmdAction::Send; s.cmd.isText = true; strcpy(s.cmd.text, "go");
  EQ(DashErr::Ok, check(s));
  s.cmd.text[0] = 0;
  EQ(DashErr::Action, check(s));
  DashComp n = comp("a", CompType::PushButton, "r1", Prop::None);
  EQ(DashErr::Action, check(n));                   // no action at all
}

void test_joystick() {
  DashComp j = comp("a", CompType::Joystick, "m1", Prop::Level);
  j.w = 2; j.h = 2;
  strcpy(j.device2, "s1");
  EQ(DashErr::Ok, check(j));
  strcpy(j.device2, "m1");
  EQ(DashErr::Device, check(j));                   // both axes on one device
  strcpy(j.device2, "r1");
  EQ(DashErr::Prop, check(j));                     // a relay has no level
  strcpy(j.device2, "gone");
  EQ(DashErr::Device, check(j));
  j.device2[0] = 0;
  EQ(DashErr::Device, check(j));
  strcpy(j.device2, "p1");
  j.w = 1;
  EQ(DashErr::Layout, check(j));                   // too small to use
  j.w = 2; j.h = 1;
  EQ(DashErr::Layout, check(j));
}

void test_dropdown() {
  DashComp d = comp("a", CompType::Dropdown, "p1", Prop::Level);
  EQ(DashErr::Options, check(d));                  // no options
  d.nOpts = 2;
  strcpy(d.opts[0].label, "Off"); d.opts[0].value = 0;
  strcpy(d.opts[1].label, "Full"); d.opts[1].value = 100;
  EQ(DashErr::Ok, check(d));
  d.opts[1].value = 101;
  EQ(DashErr::Options, check(d));                  // outside the pwm range
  d.opts[1].value = -1;
  EQ(DashErr::Options, check(d));
  d.opts[1].value = 100;
  d.opts[0].label[0] = 0;
  EQ(DashErr::Options, check(d));                  // empty label
  DashComp m = comp("a", CompType::Dropdown, "m1", Prop::Level);
  m.nOpts = 1; strcpy(m.opts[0].label, "Back"); m.opts[0].value = -100;
  EQ(DashErr::Ok, check(m));                       // a motor may go negative
  DashComp s = comp("a", CompType::Dropdown, "s1", Prop::Level);
  s.nOpts = 1; strcpy(s.opts[0].label, "End"); s.opts[0].value = 180;
  EQ(DashErr::Ok, check(s));
  s.opts[0].value = 181;
  EQ(DashErr::Options, check(s));
  DashComp many = comp("a", CompType::Dropdown, "p1", Prop::Level);
  many.nOpts = kMaxOptions;
  for (size_t i = 0; i < kMaxOptions; i++) { strcpy(many.opts[i].label, "x"); many.opts[i].value = 1; }
  EQ(DashErr::Ok, check(many));
  strcpy(many.opts[kMaxOptions].label, "x"); many.opts[kMaxOptions].value = 1;
  many.nOpts = kMaxOptions + 1;
  EQ(DashErr::Options, check(many));              // a ninth, valid option is still too many
}

void test_group() {
  DashComp g = comp("a", CompType::Group, "", Prop::None);
  g.w = 2; g.h = 2;
  EQ(DashErr::Device, check(g));                   // empty group
  g.nDevs = 2; strcpy(g.devs[0], "r1"); strcpy(g.devs[1], "p1");
  EQ(DashErr::Ok, check(g));
  strcpy(g.devs[1], "r1");
  EQ(DashErr::Device, check(g));                   // same device twice
  strcpy(g.devs[1], "gone");
  EQ(DashErr::Device, check(g));
  const char* ids[] = {"r1", "in1", "p1", "m1", "s1", "t1", "env", "ser", "r2"};
  for (size_t i = 0; i < 9; i++) strcpy(g.devs[i], ids[i]);
  g.nDevs = kMaxGroupDevices;
  EQ(DashErr::Ok, check(g));                       // eight is the limit
  g.nDevs = kMaxGroupDevices + 1;
  EQ(DashErr::Device, check(g));                   // a ninth valid device is too many
  g.nDevs = 1; strcpy(g.devs[0], "r1"); g.w = 1;
  EQ(DashErr::Layout, check(g));                   // a group needs room
}

// ---- layout, id, label, tab

void test_layout_bounds() {
  DashComp c = comp("a", CompType::Toggle, "r1", Prop::State);
  c.col = 4; c.w = 1;
  EQ(DashErr::Ok, check(c));
  c.col = 4; c.w = 2;
  EQ(DashErr::Layout, check(c));                   // runs off the right edge
  c.col = 0; c.w = 1;
  EQ(DashErr::Layout, check(c));                   // columns start at 1
  c.col = 1; c.w = 4;
  EQ(DashErr::Ok, check(c));
  c.w = 5;
  EQ(DashErr::Layout, check(c));
  c.w = 1; c.row = 64;
  EQ(DashErr::Ok, check(c));
  c.row = 65;
  EQ(DashErr::Layout, check(c));
  c.row = 0;
  EQ(DashErr::Layout, check(c));
  c.row = 1; c.h = 4;
  EQ(DashErr::Ok, check(c));
  c.h = 5;
  EQ(DashErr::Layout, check(c));
  c.h = 0;
  EQ(DashErr::Layout, check(c));
  c.h = 1; c.w = 0;
  EQ(DashErr::Layout, check(c));
}

void test_graph_min_width() {
  DashComp g = comp("a", CompType::Graph, "t1", Prop::Temp);
  EQ(DashErr::Layout, check(g));                   // a graph is at least 2 wide
  g.w = 2;
  EQ(DashErr::Ok, check(g));
}

void test_id_label_tab() {
  DashComp c = comp("a", CompType::Toggle, "r1", Prop::State);
  strcpy(c.id, "Bad Id");
  EQ(DashErr::Id, check(c));
  c.id[0] = 0;
  EQ(DashErr::Id, check(c));
  memset(c.id, 'a', kIdCap);                       // no terminator
  EQ(DashErr::Id, check(c));
  c = comp("a", CompType::Toggle, "r1", Prop::State);
  memset(c.label, 'x', kLabelCap);                 // no terminator
  EQ(DashErr::Label, check(c));
  c.label[0] = 0;
  EQ(DashErr::Ok, check(c));                       // an empty label is allowed (the UI shows the device name)
  strcpy(c.label, "Workshop Light");
  EQ(DashErr::Ok, check(c));
  c.type = CompType::kCount;
  EQ(DashErr::Type, check(c));
  c.type = CompType::Toggle;
  c.tab = 0;
  EQ(DashErr::Ok, check(c, 0));                    // no tabs: one implicit page
  c.tab = 1;
  EQ(DashErr::Tab, check(c, 0));
  EQ(DashErr::Ok, check(c, 2));
  c.tab = 2;
  EQ(DashErr::Tab, check(c, 2));
}

// ---- the whole list

void test_dashset_duplicates_and_overlap() {
  DashSet s;
  DashComp a = comp("a", CompType::Toggle, "r1", Prop::State);
  EQ(DashErr::Ok, s.add(a));
  EQ(DashErr::Duplicate, s.add(a));                // same id
  DashComp b = comp("b", CompType::Toggle, "r1", Prop::State);
  EQ(DashErr::Overlap, s.add(b));                  // same cell
  b.col = 2;
  EQ(DashErr::Ok, s.add(b));                       // next cell over
  DashComp c = comp("c", CompType::Gauge, "t1", Prop::Temp);
  c.col = 2; c.row = 2; c.w = 2; c.h = 1;
  EQ(DashErr::Ok, s.add(c));                       // below b
  DashComp d = comp("d", CompType::Gauge, "t1", Prop::Temp);
  d.col = 3; d.row = 2; d.w = 2;
  EQ(DashErr::Overlap, s.add(d));                  // shares column 3 with c
  d.row = 3;
  EQ(DashErr::Ok, s.add(d));
  DashComp e = comp("e", CompType::Toggle, "r1", Prop::State);
  e.tab = 1;
  EQ(DashErr::Ok, s.add(e));                       // same cell as a, different tab
  EQ(5, s.size());
}

void test_dashset_overlap_each_edge() {
  DashSet s;
  DashComp big = comp("big", CompType::Group, "", Prop::None);
  big.col = 2; big.row = 2; big.w = 2; big.h = 2;
  EQ(DashErr::Ok, s.add(big));                     // occupies columns 2-3, rows 2-3
  struct { int col, row, w, h; bool clash; } t[] = {
      {1, 1, 1, 1, false}, {1, 2, 1, 1, false}, {4, 2, 1, 1, false}, {2, 1, 1, 1, false}, {2, 4, 1, 1, false},
      {3, 3, 1, 1, true},  {1, 3, 2, 1, true},  {3, 1, 1, 2, true},  {3, 3, 2, 2, true},  {1, 1, 4, 4, true},
  };
  int k = 0;
  for (auto& x : t) {
    DashSet one;
    one.add(big);
    DashComp c = comp("x", CompType::Toggle, "r1", Prop::State);
    c.col = uint8_t(x.col); c.row = uint8_t(x.row); c.w = uint8_t(x.w); c.h = uint8_t(x.h);
    char msg[32];
    snprintf(msg, sizeof msg, "case %d", k++);
    TEST_ASSERT_EQUAL_MESSAGE(x.clash ? int(DashErr::Overlap) : int(DashErr::Ok), int(one.add(c)), msg);
  }
}

void test_dashset_limit() {
  DashSet s;
  for (size_t i = 0; i < kMaxComponents; i++) {
    DashComp c = comp("x", CompType::Toggle, "r1", Prop::State);
    snprintf(c.id, kIdCap, "c%u", unsigned(i));
    c.col = uint8_t(i % 4 + 1);
    c.row = uint8_t(i / 4 + 1);
    EQ(DashErr::Ok, s.add(c));
  }
  DashComp extra = comp("extra", CompType::Toggle, "r1", Prop::State);
  extra.row = 20;
  EQ(DashErr::TooMany, s.add(extra));
}

void test_tab_names() {
  char n[kMaxTabs + 1][kTabCap] = {{0}};
  EQ(DashErr::Ok, validateTabNames(n, 0));
  strcpy(n[0], "Main"); strcpy(n[1], "Garage");
  EQ(DashErr::Ok, validateTabNames(n, 2));
  strcpy(n[1], "Main");
  EQ(DashErr::Tabs, validateTabNames(n, 2));       // duplicate
  n[1][0] = 0;
  EQ(DashErr::Tabs, validateTabNames(n, 2));       // empty
  for (size_t i = 0; i < kMaxTabs + 1; i++) snprintf(n[i], kTabCap, "t%u", unsigned(i));
  EQ(DashErr::Ok, validateTabNames(n, kMaxTabs));
  EQ(DashErr::Tabs, validateTabNames(n, kMaxTabs + 1));
  memset(n[0], 'x', kTabCap);                      // no terminator
  EQ(DashErr::Tabs, validateTabNames(n, 1));
}

void test_error_names() {
  EQ(0, strlen(dashErrCode(DashErr::Ok)));
  TEST_ASSERT_EQUAL_STRING("overlap", dashErrCode(DashErr::Overlap));
  TEST_ASSERT_EQUAL_STRING("unknown_device", dashErrCode(DashErr::Device));
  TEST_ASSERT_EQUAL_STRING("bad_property", dashErrCode(DashErr::Prop));
  TEST_ASSERT_EQUAL_STRING("binding.device", dashErrField(DashErr::Device));
  TEST_ASSERT_EQUAL_STRING("layout", dashErrField(DashErr::Overlap));
  TEST_ASSERT_EQUAL_STRING("tabs", dashErrField(DashErr::Tabs));
  TEST_ASSERT_EQUAL_STRING("", dashErrField(DashErr::Ok));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_device_props);
  RUN_TEST(test_prop_names_round_trip);
  RUN_TEST(test_comp_names_round_trip);
  RUN_TEST(test_levels);
  RUN_TEST(test_need_accepts);
  RUN_TEST(test_comp_needs);
  RUN_TEST(test_toggle);
  RUN_TEST(test_slider_and_readouts);
  RUN_TEST(test_device_card);
  RUN_TEST(test_timer_component);
  RUN_TEST(test_timer_component_id_must_be_terminated);
  RUN_TEST(test_push_button);
  RUN_TEST(test_joystick);
  RUN_TEST(test_dropdown);
  RUN_TEST(test_group);
  RUN_TEST(test_layout_bounds);
  RUN_TEST(test_graph_min_width);
  RUN_TEST(test_id_label_tab);
  RUN_TEST(test_dashset_duplicates_and_overlap);
  RUN_TEST(test_dashset_overlap_each_edge);
  RUN_TEST(test_dashset_limit);
  RUN_TEST(test_tab_names);
  RUN_TEST(test_error_names);
  return UNITY_END();
}
