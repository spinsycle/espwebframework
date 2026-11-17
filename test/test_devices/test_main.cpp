#include <unity.h>
#include <math.h>
#include <stdio.h>

#include "devices/DeviceTypes.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

#define EQ(a, b) TEST_ASSERT_EQUAL(int(a), int(b))

static DeviceConfig dev(const char* id, DeviceType t, int pin) {
  DeviceConfig c;
  strcpy(c.id, id);
  c.type = t;
  c.pin = int8_t(pin);
  return c;
}

// ---- pins

void test_esp8266_pins() {
  EQ(PinErr::Ok, checkPin(Chip::Esp8266, 5, PinRole::Output));
  EQ(PinErr::Ok, checkPin(Chip::Esp8266, 16, PinRole::Output));
  EQ(PinErr::Ok, checkPin(Chip::Esp8266, 16, PinRole::Input));
  EQ(PinErr::NoPwm, checkPin(Chip::Esp8266, 16, PinRole::Pwm));
  for (int p = 6; p <= 11; p++) EQ(PinErr::Reserved, checkPin(Chip::Esp8266, p, PinRole::Output));
  EQ(PinErr::Reserved, checkPin(Chip::Esp8266, 1, PinRole::Output));
  EQ(PinErr::Reserved, checkPin(Chip::Esp8266, 3, PinRole::Input));
  EQ(PinErr::Invalid, checkPin(Chip::Esp8266, 17, PinRole::Output));
  EQ(PinErr::Invalid, checkPin(Chip::Esp8266, -1, PinRole::Output));
}

void test_esp32_pins() {
  EQ(PinErr::Ok, checkPin(Chip::Esp32, 26, PinRole::Output));
  EQ(PinErr::Ok, checkPin(Chip::Esp32, 33, PinRole::Pwm));
  EQ(PinErr::Ok, checkPin(Chip::Esp32, 34, PinRole::Input));
  EQ(PinErr::InputOnly, checkPin(Chip::Esp32, 34, PinRole::Output));
  EQ(PinErr::InputOnly, checkPin(Chip::Esp32, 39, PinRole::Pwm));
  EQ(PinErr::Reserved, checkPin(Chip::Esp32, 6, PinRole::Output));
  EQ(PinErr::Reserved, checkPin(Chip::Esp32, 11, PinRole::Input));
  EQ(PinErr::Reserved, checkPin(Chip::Esp32, 1, PinRole::Output));
  EQ(PinErr::Invalid, checkPin(Chip::Esp32, 20, PinRole::Output));
  EQ(PinErr::Invalid, checkPin(Chip::Esp32, 28, PinRole::Output));
  EQ(PinErr::Invalid, checkPin(Chip::Esp32, 40, PinRole::Input));
}

void test_pin_traits() {
  TEST_ASSERT_FALSE(pinHasPullup(Chip::Esp8266, 16));
  TEST_ASSERT_TRUE(pinHasPullup(Chip::Esp8266, 5));
  TEST_ASSERT_FALSE(pinHasPullup(Chip::Esp32, 34));
  TEST_ASSERT_TRUE(pinHasPullup(Chip::Esp32, 33));
  TEST_ASSERT_TRUE(pinBootSensitive(Chip::Esp8266, 15));
  TEST_ASSERT_FALSE(pinBootSensitive(Chip::Esp8266, 5));
  TEST_ASSERT_TRUE(pinBootSensitive(Chip::Esp32, 12));
  TEST_ASSERT_FALSE(pinBootSensitive(Chip::Esp32, 26));
}

// ---- validation

void test_valid_device_and_id_rules() {
  DeviceConfig c = dev("relay-1", DeviceType::Relay, 5);
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  strcpy(c.id, "");
  EQ(DevErr::Id, validateDevice(c, Chip::Esp8266, nullptr, 0));
  strcpy(c.id, "Relay");  // uppercase not allowed
  EQ(DevErr::Id, validateDevice(c, Chip::Esp8266, nullptr, 0));
  strcpy(c.id, "a b");
  EQ(DevErr::Id, validateDevice(c, Chip::Esp8266, nullptr, 0));
  strcpy(c.id, "abcdefghijklmnop");  // 16 chars: ok
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  memset(c.id, 'a', sizeof c.id);    // no terminator
  EQ(DevErr::Id, validateDevice(c, Chip::Esp8266, nullptr, 0));
  strcpy(c.id, "r0");                // every allowed character class
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  strcpy(c.id, "z_9-a");
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  TEST_ASSERT_TRUE(detail::validDeviceId("abcdefghijklmnop"));
  TEST_ASSERT_FALSE(detail::validDeviceId("abcdefghijklmnopq"));  // 17 chars
}

void test_name_unterminated() {
  DeviceConfig c = dev("a", DeviceType::Relay, 5);
  memset(c.name, 'x', sizeof c.name);
  EQ(DevErr::Name, validateDevice(c, Chip::Esp8266, nullptr, 0));
}

void test_duplicate_and_pin_in_use() {
  DeviceConfig others[2] = {dev("a", DeviceType::Relay, 5), dev("b", DeviceType::Input, 4)};
  DeviceConfig c = dev("a", DeviceType::Relay, 12);
  EQ(DevErr::Duplicate, validateDevice(c, Chip::Esp8266, others, 2));
  c = dev("c", DeviceType::Relay, 4);
  EQ(DevErr::PinInUse, validateDevice(c, Chip::Esp8266, others, 2));
  c = dev("c", DeviceType::Relay, 12);
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, others, 2));
}

void test_pin_errors_map() {
  DeviceConfig c = dev("a", DeviceType::Relay, 7);
  EQ(DevErr::PinReserved, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c = dev("a", DeviceType::Relay, 34);
  EQ(DevErr::PinInputOnly, validateDevice(c, Chip::Esp32, nullptr, 0));
  c = dev("a", DeviceType::Pwm, 16);
  EQ(DevErr::PinNoPwm, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c = dev("a", DeviceType::Relay, 99);
  EQ(DevErr::Pin, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c = dev("a", DeviceType::Relay, -1);  // pin missing from the request
  EQ(DevErr::Pin, validateDevice(c, Chip::Esp8266, nullptr, 0));
}

void test_pullup_rules() {
  DeviceConfig c = dev("a", DeviceType::Input, 16);
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c.pullup = true;
  EQ(DevErr::PinNoPullup, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c = dev("a", DeviceType::Input, 34);
  c.pullup = true;
  EQ(DevErr::PinNoPullup, validateDevice(c, Chip::Esp32, nullptr, 0));
  c.pullup = false;
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp32, nullptr, 0));
}

void test_ranges() {
  DeviceConfig c = dev("a", DeviceType::Input, 5);
  c.debounceMs = 1000;
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c.debounceMs = 1001;
  EQ(DevErr::Debounce, validateDevice(c, Chip::Esp8266, nullptr, 0));

  c = dev("a", DeviceType::Pwm, 5);
  c.freqHz = 0;
  EQ(DevErr::Freq, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c.freqHz = 20000;
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c.freqHz = 20001;
  EQ(DevErr::Freq, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c.freqHz = 1000;
  c.initial = 100;
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c.initial = 101;
  EQ(DevErr::Initial, validateDevice(c, Chip::Esp8266, nullptr, 0));

  c = dev("a", DeviceType::Relay, 5);
  c.initial = 1;
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp8266, nullptr, 0));
  c.initial = 2;
  EQ(DevErr::Initial, validateDevice(c, Chip::Esp8266, nullptr, 0));
}

void test_limits() {
  static DeviceConfig many[kMaxDevices];
  for (size_t i = 0; i < kMaxDevices; i++) {
    snprintf(many[i].id, sizeof many[i].id, "d%u", unsigned(i));
    many[i].pin = int8_t(i);
  }
  DeviceConfig c = dev("new", DeviceType::Relay, 33);
  EQ(DevErr::Full, validateDevice(c, Chip::Esp32, many, kMaxDevices));
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp32, many, kMaxDevices - 1));

  DeviceConfig pw[kMaxPwm];
  for (size_t i = 0; i < kMaxPwm; i++) {
    snprintf(pw[i].id, sizeof pw[i].id, "p%u", unsigned(i));
    pw[i].type = DeviceType::Pwm;
    pw[i].pin = int8_t(i + 12);
  }
  c = dev("new", DeviceType::Pwm, 2);
  EQ(DevErr::PwmLimit, validateDevice(c, Chip::Esp32, pw, kMaxPwm));
  c.type = DeviceType::Relay;
  EQ(DevErr::Ok, validateDevice(c, Chip::Esp32, pw, kMaxPwm));
}

// ---- commands

static Command cmd(CmdAction a, bool has, bool isBool, double v) {
  Command c;
  c.action = a; c.hasValue = has; c.isBool = isBool; c.value = v;
  return c;
}

void test_relay_commands() {
  EQ(CmdErr::Ok, checkCommand(DeviceType::Relay, cmd(CmdAction::Set, true, true, 1)));
  EQ(CmdErr::Ok, checkCommand(DeviceType::Relay, cmd(CmdAction::Toggle, false, false, 0)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Relay, cmd(CmdAction::Set, false, false, 0)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Relay, cmd(CmdAction::Set, true, false, 1)));  // number, not bool
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Relay, cmd(CmdAction::None, false, false, 0)));
}

void test_pwm_commands() {
  EQ(CmdErr::Ok, checkCommand(DeviceType::Pwm, cmd(CmdAction::Set, true, false, 0)));
  EQ(CmdErr::Ok, checkCommand(DeviceType::Pwm, cmd(CmdAction::Set, true, false, 100)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Pwm, cmd(CmdAction::Set, true, false, 100.5)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Pwm, cmd(CmdAction::Set, true, false, -0.5)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Pwm, cmd(CmdAction::Set, true, false, NAN)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Pwm, cmd(CmdAction::Set, true, true, 1)));  // bool rejected
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Pwm, cmd(CmdAction::Set, false, false, 0)));
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Pwm, cmd(CmdAction::Toggle, false, false, 0)));
}

void test_input_not_commandable() {
  EQ(CmdErr::NotCommandable, checkCommand(DeviceType::Input, cmd(CmdAction::Set, true, true, 1)));
}

void test_cmd_error_maps() {
  EQ(202, cmdErrHttp(CmdErr::Ok));
  EQ(404, cmdErrHttp(CmdErr::NotFound));
  EQ(409, cmdErrHttp(CmdErr::NotCommandable));
  EQ(503, cmdErrHttp(CmdErr::QueueFull));
  EQ(400, cmdErrHttp(CmdErr::BadAction));
  EQ(400, cmdErrHttp(CmdErr::BadValue));
  EQ(400, cmdErrHttp(CmdErr::BadId));
  TEST_ASSERT_EQUAL_STRING("not_found", cmdErrCode(CmdErr::NotFound));
  TEST_ASSERT_EQUAL_STRING("not_commandable", cmdErrCode(CmdErr::NotCommandable));
  TEST_ASSERT_EQUAL_STRING("bad_action", cmdErrCode(CmdErr::BadAction));
  TEST_ASSERT_EQUAL_STRING("bad_value", cmdErrCode(CmdErr::BadValue));
  TEST_ASSERT_EQUAL_STRING("busy", cmdErrCode(CmdErr::QueueFull));
  TEST_ASSERT_EQUAL_STRING("bad_id", cmdErrCode(CmdErr::BadId));
  TEST_ASSERT_EQUAL_STRING("action", cmdErrField(CmdErr::BadAction));
  TEST_ASSERT_EQUAL_STRING("value", cmdErrField(CmdErr::BadValue));
  TEST_ASSERT_EQUAL_STRING("id", cmdErrField(CmdErr::NotFound));
  TEST_ASSERT_EQUAL_STRING("id", cmdErrField(CmdErr::BadId));
}

void test_parse_helpers() {
  CmdAction a;
  TEST_ASSERT_TRUE(parseAction("set", a)); EQ(CmdAction::Set, a);
  TEST_ASSERT_TRUE(parseAction("toggle", a)); EQ(CmdAction::Toggle, a);
  TEST_ASSERT_FALSE(parseAction("flip", a));
  TEST_ASSERT_FALSE(parseAction(nullptr, a));
  DeviceType t;
  TEST_ASSERT_TRUE(parseDeviceType("input", t)); EQ(DeviceType::Input, t);
  TEST_ASSERT_FALSE(parseDeviceType("stepper", t));
}

// ---- mapping

void test_level_mapping() {
  TEST_ASSERT_TRUE(relayPinLevel(true, false));
  TEST_ASSERT_FALSE(relayPinLevel(false, false));
  TEST_ASSERT_FALSE(relayPinLevel(true, true));   // active-low: on = pin low
  TEST_ASSERT_TRUE(relayPinLevel(false, true));
  TEST_ASSERT_TRUE(inputActive(true, false));
  TEST_ASSERT_TRUE(inputActive(false, true));     // pull-up button: pressed = low = active
  TEST_ASSERT_FALSE(inputActive(true, true));
  EQ(0, pwmPermille(0, false));
  EQ(500, pwmPermille(50, false));
  EQ(1000, pwmPermille(100, false));
  EQ(1000, pwmPermille(0, true));
  EQ(750, pwmPermille(25, true));
  EQ(1000, pwmPermille(200, false));  // clamped
}

// ---- debounce

void test_debounce_basic() {
  Debouncer d;
  TEST_ASSERT_FALSE(d.update(false, 0, 30));     // first sample primes, no edge
  TEST_ASSERT_FALSE(d.update(true, 100, 30));    // change seen, timer starts
  TEST_ASSERT_FALSE(d.update(true, 129, 30));    // 29 ms: not yet
  TEST_ASSERT_FALSE(d.stable);
  TEST_ASSERT_TRUE(d.update(true, 130, 30));     // 30 ms: accepted
  TEST_ASSERT_TRUE(d.stable);
  TEST_ASSERT_FALSE(d.update(true, 500, 30));    // no repeat edge
}

void test_debounce_rejects_bounce() {
  Debouncer d;
  d.update(false, 0, 30);
  d.update(true, 100, 30);
  d.update(false, 110, 30);                      // bounced back before 30 ms
  TEST_ASSERT_FALSE(d.update(false, 200, 30));
  TEST_ASSERT_FALSE(d.stable);
  d.update(true, 300, 30);
  d.update(false, 310, 30);
  d.update(true, 320, 30);                       // timer restarted at 320
  TEST_ASSERT_FALSE(d.update(true, 349, 30));
  TEST_ASSERT_TRUE(d.update(true, 350, 30));
}

void test_debounce_primed_held_and_wrap() {
  Debouncer d;
  TEST_ASSERT_FALSE(d.update(true, 0, 30));      // held at boot: stable immediately, no edge
  TEST_ASSERT_TRUE(d.stable);
  Debouncer w;
  w.update(false, 0xFFFFFFF0u, 30);
  w.update(true, 0xFFFFFFF8u, 30);
  TEST_ASSERT_FALSE(w.update(true, 10, 30));     // millis() wrapped: 18 ms elapsed
  TEST_ASSERT_TRUE(w.update(true, 0x28, 30));    // 48 ms elapsed
}

void test_debounce_zero_ms() {
  Debouncer d;
  d.update(false, 0, 0);
  TEST_ASSERT_TRUE(d.update(true, 5, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_esp8266_pins);
  RUN_TEST(test_esp32_pins);
  RUN_TEST(test_pin_traits);
  RUN_TEST(test_valid_device_and_id_rules);
  RUN_TEST(test_name_unterminated);
  RUN_TEST(test_duplicate_and_pin_in_use);
  RUN_TEST(test_pin_errors_map);
  RUN_TEST(test_pullup_rules);
  RUN_TEST(test_ranges);
  RUN_TEST(test_limits);
  RUN_TEST(test_relay_commands);
  RUN_TEST(test_pwm_commands);
  RUN_TEST(test_input_not_commandable);
  RUN_TEST(test_cmd_error_maps);
  RUN_TEST(test_parse_helpers);
  RUN_TEST(test_level_mapping);
  RUN_TEST(test_debounce_basic);
  RUN_TEST(test_debounce_rejects_bounce);
  RUN_TEST(test_debounce_primed_held_and_wrap);
  RUN_TEST(test_debounce_zero_ms);
  return UNITY_END();
}
