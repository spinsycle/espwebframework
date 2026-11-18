#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "core/EventRing.h"
#include "core/HealthRules.h"
#include "devices/DeviceTypes.h"
#include "devices/PinInfo.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

#define EQ(a, b) TEST_ASSERT_EQUAL(int(a), int(b))

static DeviceConfig dev(DeviceType t, int pin) {
  DeviceConfig c;
  strcpy(c.id, "d1");
  c.type = t;
  c.pin = int8_t(pin);
  if (t == DeviceType::Motor) c.pin2 = 12;
  return c;
}

static DevErr check(const DeviceConfig& c) { return validateDevice(c, Chip::Esp8266, nullptr, 0); }

// ---- failsafe timing

void test_failsafe_off_never_fires() {
  TEST_ASSERT_FALSE(failsafeDue(0, 0, 0));
  TEST_ASSERT_FALSE(failsafeDue(4000000000u, 0, 0));
}

void test_failsafe_fires_exactly_at_the_timeout() {
  TEST_ASSERT_FALSE(failsafeDue(1999, 0, 2));
  TEST_ASSERT_TRUE(failsafeDue(2000, 0, 2));
  TEST_ASSERT_TRUE(failsafeDue(2001, 0, 2));
  TEST_ASSERT_FALSE(failsafeDue(5999, 4000, 2));
  TEST_ASSERT_TRUE(failsafeDue(6000, 4000, 2));
}

void test_failsafe_across_the_millis_wrap() {
  uint32_t last = 0xFFFFFC00u;  // 1024 ms before the wrap
  TEST_ASSERT_FALSE(failsafeDue(100, last, 2));  // 1124 ms later
  TEST_ASSERT_TRUE(failsafeDue(1000, last, 2));  // 2024 ms later
  TEST_ASSERT_TRUE(failsafeDue(100, last, 1));   // 1124 ms later: due, though the deadline itself did not wrap
  TEST_ASSERT_FALSE(failsafeDue(0xFFFFFE00u, last, 1));  // 512 ms later
}

void test_failsafe_the_longest_timeout_does_not_overflow() {
  TEST_ASSERT_FALSE(failsafeDue(3599999u, 0, kMaxFailsafeS));
  TEST_ASSERT_TRUE(failsafeDue(3600000u, 0, kMaxFailsafeS));
}

void test_newer_stamp() {
  EQ(newerStamp(10, 20), 20);
  EQ(newerStamp(20, 10), 20);
  EQ(newerStamp(5, 5), 5);
  TEST_ASSERT_EQUAL_UINT32(5u, newerStamp(0xFFFFFFF0u, 5u));  // 5 is later: the clock wrapped between them
  TEST_ASSERT_EQUAL_UINT32(5u, newerStamp(5u, 0xFFFFFFF0u));
}

// ---- what is safe

void test_which_devices_have_a_safe_state() {
  TEST_ASSERT_TRUE(failsafeApplies(DeviceType::Relay));
  TEST_ASSERT_TRUE(failsafeApplies(DeviceType::Pwm));
  TEST_ASSERT_TRUE(failsafeApplies(DeviceType::Motor));
  TEST_ASSERT_TRUE(failsafeApplies(DeviceType::Servo));
  TEST_ASSERT_FALSE(failsafeApplies(DeviceType::Input));
  TEST_ASSERT_FALSE(failsafeApplies(DeviceType::Sensor));
  TEST_ASSERT_FALSE(failsafeApplies(DeviceType::Serial));
}

void test_safe_targets() {
  SafeTarget t = safeTarget(dev(DeviceType::Relay, 2));
  TEST_ASSERT_TRUE(t.act);
  TEST_ASSERT_FALSE(t.on);
  t = safeTarget(dev(DeviceType::Pwm, 14));
  TEST_ASSERT_TRUE(t.act);
  EQ(t.duty, 0);
  t = safeTarget(dev(DeviceType::Motor, 14));
  TEST_ASSERT_TRUE(t.act);
  EQ(t.val, 0);
  t = safeTarget(dev(DeviceType::Input, 12));
  TEST_ASSERT_FALSE(t.act);
  t = safeTarget(dev(DeviceType::Sensor, 12));
  TEST_ASSERT_FALSE(t.act);
}

void test_servo_holds_unless_given_a_safe_angle() {
  DeviceConfig c = dev(DeviceType::Servo, 14);
  TEST_ASSERT_FALSE(safeTarget(c).act);  // hold
  c.safeAngle = 0;
  TEST_ASSERT_TRUE(safeTarget(c).act);
  EQ(safeTarget(c).val, 0);
  c.safeAngle = 135;
  EQ(safeTarget(c).val, 135);
}

// ---- config validation

void test_defaults_are_enabled_and_no_failsafe() {
  DeviceConfig c;
  TEST_ASSERT_TRUE(c.enabled);
  EQ(c.failsafeS, 0);
  EQ(c.safeAngle, -1);
}

void test_failsafe_seconds_limits() {
  DeviceConfig c = dev(DeviceType::Relay, 2);
  c.failsafeS = 30;
  EQ(check(c), DevErr::Ok);
  c.failsafeS = kMaxFailsafeS;
  EQ(check(c), DevErr::Ok);
  c.failsafeS = kMaxFailsafeS + 1;
  EQ(check(c), DevErr::Failsafe);
}

void test_failsafe_only_on_devices_that_drive_something() {
  for (DeviceType t : {DeviceType::Relay, DeviceType::Pwm, DeviceType::Motor}) {
    DeviceConfig c = dev(t, 14);
    c.failsafeS = 5;
    EQ(check(c), DevErr::Ok);
  }
  DeviceConfig s = dev(DeviceType::Servo, 14);
  s.failsafeS = 5;
  EQ(check(s), DevErr::Ok);
  DeviceConfig in = dev(DeviceType::Input, 12);
  in.failsafeS = 5;
  EQ(check(in), DevErr::Failsafe);
  DeviceConfig se = dev(DeviceType::Sensor, 12);
  se.failsafeS = 5;
  EQ(check(se), DevErr::Failsafe);
}

void test_safe_angle_only_on_servos_in_range() {
  DeviceConfig s = dev(DeviceType::Servo, 14);
  for (int a : {-1, 0, 90, 180}) { s.safeAngle = int16_t(a); EQ(check(s), DevErr::Ok); }
  s.safeAngle = 181;
  EQ(check(s), DevErr::SafeAngle);
  s.safeAngle = -2;
  EQ(check(s), DevErr::SafeAngle);
  DeviceConfig r = dev(DeviceType::Relay, 2);
  r.safeAngle = 90;
  EQ(check(r), DevErr::SafeAngle);
}

void test_disabled_device_still_validates() {
  DeviceConfig c = dev(DeviceType::Relay, 2);
  c.enabled = false;
  EQ(check(c), DevErr::Ok);
}

void test_error_names_and_fields() {
  TEST_ASSERT_EQUAL_STRING("bad_failsafe", devErrCode(DevErr::Failsafe));
  TEST_ASSERT_EQUAL_STRING("failsafe_s", devErrField(DevErr::Failsafe));
  TEST_ASSERT_EQUAL_STRING("bad_safe_angle", devErrCode(DevErr::SafeAngle));
  TEST_ASSERT_EQUAL_STRING("safe_angle", devErrField(DevErr::SafeAngle));
}

void test_disabled_command_error() {
  TEST_ASSERT_EQUAL_STRING("disabled", cmdErrCode(CmdErr::Disabled));
  EQ(cmdErrHttp(CmdErr::Disabled), 409);
  TEST_ASSERT_EQUAL_STRING("id", cmdErrField(CmdErr::Disabled));
}

// ---- the event ring

void test_ring_starts_empty() {
  EventRing r;
  EQ(r.size(), 0);
  SysEvent e;
  TEST_ASSERT_FALSE(r.newest(0, e));
}

void test_ring_returns_newest_first() {
  EventRing r;
  r.add(1, 0, "boot", "power on");
  r.add(2, 1700000000u, "failsafe", "d1");
  SysEvent e;
  TEST_ASSERT_TRUE(r.newest(0, e));
  TEST_ASSERT_EQUAL_STRING("failsafe", e.kind);
  TEST_ASSERT_EQUAL_UINT32(1700000000u, e.epoch);
  TEST_ASSERT_EQUAL_UINT32(2u, e.upS);
  TEST_ASSERT_TRUE(r.newest(1, e));
  TEST_ASSERT_EQUAL_STRING("boot", e.kind);
  TEST_ASSERT_FALSE(r.newest(2, e));
}

void test_ring_drops_the_oldest_when_full() {
  EventRing r;
  for (uint32_t i = 0; i < kSysEventCap + 5; i++) {
    char t[8];
    snprintf(t, sizeof t, "e%u", unsigned(i));
    r.add(i, 0, "k", t);
  }
  EQ(r.size(), kSysEventCap);
  TEST_ASSERT_EQUAL_UINT32(kSysEventCap + 5, r.total());
  SysEvent e;
  TEST_ASSERT_TRUE(r.newest(0, e));
  EQ(e.upS, kSysEventCap + 4);
  TEST_ASSERT_TRUE(r.newest(kSysEventCap - 1, e));
  EQ(e.upS, 5);  // events 0..4 scrolled out
  TEST_ASSERT_FALSE(r.newest(kSysEventCap, e));
}

void test_ring_cuts_long_text_and_survives_null() {
  EventRing r;
  char longText[100], longKind[100];
  memset(longText, 'x', sizeof longText);
  longText[99] = 0;
  memset(longKind, 'k', sizeof longKind);
  longKind[99] = 0;
  r.add(1, 0, longKind, longText);
  r.add(2, 0, nullptr, nullptr);
  SysEvent e;
  TEST_ASSERT_TRUE(r.newest(1, e));
  EQ(strlen(e.kind), kEventKindCap - 1);
  EQ(strlen(e.text), kEventTextCap - 1);
  TEST_ASSERT_TRUE(r.newest(0, e));
  EQ(strlen(e.kind), 0);
  EQ(strlen(e.text), 0);
}

void test_ring_text_that_just_fits() {
  EventRing r;
  char t[kEventTextCap];
  memset(t, 'a', kEventTextCap - 1);
  t[kEventTextCap - 1] = 0;
  r.add(1, 0, "k", t);
  SysEvent e;
  TEST_ASSERT_TRUE(r.newest(0, e));
  TEST_ASSERT_EQUAL_STRING(t, e.text);
}

// ---- reading a reset reason

void test_watchdog_reasons() {
  TEST_ASSERT_TRUE(isWatchdogReason("Hardware Watchdog"));
  TEST_ASSERT_TRUE(isWatchdogReason("Software Watchdog"));
  TEST_ASSERT_TRUE(isWatchdogReason("Task watchdog"));
  TEST_ASSERT_TRUE(isWatchdogReason("Interrupt watchdog"));
  TEST_ASSERT_TRUE(isWatchdogReason("RTC WDT reset"));
  TEST_ASSERT_FALSE(isWatchdogReason("Power on"));
  TEST_ASSERT_FALSE(isWatchdogReason("Software/System restart"));
  TEST_ASSERT_FALSE(isWatchdogReason("External System"));
  TEST_ASSERT_FALSE(isWatchdogReason(""));
  TEST_ASSERT_FALSE(isWatchdogReason(nullptr));
}

void test_empty_word_matches_nothing() {
  TEST_ASSERT_FALSE(reasonHas("Power on", ""));
  TEST_ASSERT_FALSE(reasonHas("", "x"));
  TEST_ASSERT_FALSE(reasonHas(nullptr, "x"));
  TEST_ASSERT_FALSE(reasonHas("Power on", nullptr));
  TEST_ASSERT_TRUE(reasonHas("Power on", "power"));
}

void test_fault_reasons() {
  TEST_ASSERT_TRUE(isFaultReason("Hardware Watchdog"));
  TEST_ASSERT_TRUE(isFaultReason("Exception"));
  TEST_ASSERT_TRUE(isFaultReason("Fatal exception (28)"));
  TEST_ASSERT_TRUE(isFaultReason("Brownout"));
  TEST_ASSERT_TRUE(isFaultReason("Software panic"));
  TEST_ASSERT_FALSE(isFaultReason("Power on"));
  TEST_ASSERT_FALSE(isFaultReason("Software/System restart"));
  TEST_ASSERT_FALSE(isFaultReason("Deep-Sleep Wake"));
  TEST_ASSERT_FALSE(isFaultReason("External System"));
}

// ---- the GPIO inspector's facts

void test_nodemcu_labels() {
  TEST_ASSERT_EQUAL_STRING("D0", pinLabel(Chip::Esp8266, 16));
  TEST_ASSERT_EQUAL_STRING("D1", pinLabel(Chip::Esp8266, 5));
  TEST_ASSERT_EQUAL_STRING("D2", pinLabel(Chip::Esp8266, 4));
  TEST_ASSERT_EQUAL_STRING("D3", pinLabel(Chip::Esp8266, 0));
  TEST_ASSERT_EQUAL_STRING("D4", pinLabel(Chip::Esp8266, 2));
  TEST_ASSERT_EQUAL_STRING("D5", pinLabel(Chip::Esp8266, 14));
  TEST_ASSERT_EQUAL_STRING("D6", pinLabel(Chip::Esp8266, 12));
  TEST_ASSERT_EQUAL_STRING("D7", pinLabel(Chip::Esp8266, 13));
  TEST_ASSERT_EQUAL_STRING("D8", pinLabel(Chip::Esp8266, 15));
  TEST_ASSERT_EQUAL_STRING("RX", pinLabel(Chip::Esp8266, 3));
  TEST_ASSERT_EQUAL_STRING("TX", pinLabel(Chip::Esp8266, 1));
  TEST_ASSERT_EQUAL_STRING("", pinLabel(Chip::Esp32, 5));
}

void test_pin_notes() {
  TEST_ASSERT_EQUAL_STRING("flash", pinNote(Chip::Esp8266, 6));
  TEST_ASSERT_EQUAL_STRING("flash", pinNote(Chip::Esp8266, 11));
  TEST_ASSERT_EQUAL_STRING("uart", pinNote(Chip::Esp8266, 1));
  TEST_ASSERT_EQUAL_STRING("uart", pinNote(Chip::Esp8266, 3));
  TEST_ASSERT_EQUAL_STRING("strapping", pinNote(Chip::Esp8266, 0));
  TEST_ASSERT_EQUAL_STRING("strapping", pinNote(Chip::Esp8266, 15));
  TEST_ASSERT_EQUAL_STRING("no_pwm", pinNote(Chip::Esp8266, 16));
  TEST_ASSERT_EQUAL_STRING("", pinNote(Chip::Esp8266, 5));
  TEST_ASSERT_EQUAL_STRING("input_only", pinNote(Chip::Esp32, 34));
  TEST_ASSERT_EQUAL_STRING("strapping", pinNote(Chip::Esp32, 12));
  TEST_ASSERT_EQUAL_STRING("", pinNote(Chip::Esp32, 25));
}

void test_which_pins_are_listed_and_read() {
  int listed = 0, readable = 0;
  for (int g = 0; g <= 16; g++) { listed += pinListed(Chip::Esp8266, g); readable += pinReadable(Chip::Esp8266, g); }
  EQ(listed, 17);
  EQ(readable, 17 - 8);  // GPIO 1, 3 and 6-11 are never read
  TEST_ASSERT_FALSE(pinListed(Chip::Esp8266, 17));
  TEST_ASSERT_FALSE(pinListed(Chip::Esp8266, -1));
  TEST_ASSERT_FALSE(pinListed(Chip::Esp32, 20));
  TEST_ASSERT_FALSE(pinListed(Chip::Esp32, 24));
  TEST_ASSERT_FALSE(pinListed(Chip::Esp32, 28));
  TEST_ASSERT_FALSE(pinListed(Chip::Esp32, 40));
  TEST_ASSERT_TRUE(pinListed(Chip::Esp32, 39));
  TEST_ASSERT_TRUE(pinListed(Chip::Esp32, 6));
  TEST_ASSERT_FALSE(pinReadable(Chip::Esp32, 6));
  TEST_ASSERT_TRUE(pinReadable(Chip::Esp32, 34));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_failsafe_off_never_fires);
  RUN_TEST(test_failsafe_fires_exactly_at_the_timeout);
  RUN_TEST(test_failsafe_across_the_millis_wrap);
  RUN_TEST(test_failsafe_the_longest_timeout_does_not_overflow);
  RUN_TEST(test_newer_stamp);
  RUN_TEST(test_which_devices_have_a_safe_state);
  RUN_TEST(test_safe_targets);
  RUN_TEST(test_servo_holds_unless_given_a_safe_angle);
  RUN_TEST(test_defaults_are_enabled_and_no_failsafe);
  RUN_TEST(test_failsafe_seconds_limits);
  RUN_TEST(test_failsafe_only_on_devices_that_drive_something);
  RUN_TEST(test_safe_angle_only_on_servos_in_range);
  RUN_TEST(test_disabled_device_still_validates);
  RUN_TEST(test_error_names_and_fields);
  RUN_TEST(test_disabled_command_error);
  RUN_TEST(test_ring_starts_empty);
  RUN_TEST(test_ring_returns_newest_first);
  RUN_TEST(test_ring_drops_the_oldest_when_full);
  RUN_TEST(test_ring_cuts_long_text_and_survives_null);
  RUN_TEST(test_ring_text_that_just_fits);
  RUN_TEST(test_watchdog_reasons);
  RUN_TEST(test_empty_word_matches_nothing);
  RUN_TEST(test_fault_reasons);
  RUN_TEST(test_nodemcu_labels);
  RUN_TEST(test_pin_notes);
  RUN_TEST(test_which_pins_are_listed_and_read);
  return UNITY_END();
}
