#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "config/ConfigRules.h"
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

// ---- schema

void test_schema_check() {
  EQ(SchemaCheck::Ok, configSchemaCheck(-1));   // no schema_version: a file from before P5
  EQ(SchemaCheck::Ok, configSchemaCheck(1));
  EQ(SchemaCheck::Bad, configSchemaCheck(0));
  EQ(SchemaCheck::TooNew, configSchemaCheck(kConfigSchema + 1));
  EQ(SchemaCheck::TooNew, configSchemaCheck(1000));
}

// ---- factory reset list

void test_config_files_listed() {
  EQ(9, kConfigFileCount);
  TEST_ASSERT_TRUE(isConfigFile("/config/network.json"));
  TEST_ASSERT_TRUE(isConfigFile("/config/network.json.tmp"));
  TEST_ASSERT_TRUE(isConfigFile("/config/devices.json"));
  TEST_ASSERT_TRUE(isConfigFile("/config/devices.json.tmp"));
  TEST_ASSERT_TRUE(isConfigFile("/config/devices.json.bak"));
  TEST_ASSERT_TRUE(isConfigFile("/config/devices.json.new"));
  TEST_ASSERT_TRUE(isConfigFile("/config/dashboard.json"));
  TEST_ASSERT_TRUE(isConfigFile("/config/dashboard.json.tmp"));
  TEST_ASSERT_TRUE(isConfigFile("/config/dashboard.json.bak"));
  TEST_ASSERT_FALSE(isConfigFile("/config/other.json"));
  TEST_ASSERT_FALSE(isConfigFile("/www/index.html"));  // the UI must survive a factory reset
  TEST_ASSERT_FALSE(isConfigFile(""));
}

// ---- editing a device in place: validateDevice with skip

void test_edit_ignores_its_old_self() {
  DeviceConfig o[2] = {dev("a", DeviceType::Relay, 5), dev("b", DeviceType::Relay, 12)};
  DeviceConfig edit = o[0];
  EQ(DevErr::Duplicate, validateDevice(edit, Chip::Esp8266, o, 2));       // as an add it clashes with itself
  EQ(DevErr::Ok, validateDevice(edit, Chip::Esp8266, o, 2, 0));           // as an edit it does not
  edit.pin = 14;
  EQ(DevErr::Ok, validateDevice(edit, Chip::Esp8266, o, 2, 0));           // a new free pin is fine
}

void test_edit_still_conflicts_with_others() {
  DeviceConfig o[2] = {dev("a", DeviceType::Relay, 5), dev("b", DeviceType::Relay, 12)};
  DeviceConfig edit = o[1];
  edit.pin = 5;  // a's pin
  EQ(DevErr::PinInUse, validateDevice(edit, Chip::Esp8266, o, 2, 1));
  edit = o[1];
  strcpy(edit.id, "a");  // a's id
  EQ(DevErr::Duplicate, validateDevice(edit, Chip::Esp8266, o, 2, 1));
}

void test_edit_skip_index_is_the_right_one() {
  DeviceConfig o[2] = {dev("a", DeviceType::Relay, 5), dev("b", DeviceType::Relay, 12)};
  DeviceConfig edit = o[1];
  edit.pin = 5;                                                         // a's pin
  EQ(DevErr::PinInUse, validateDevice(edit, Chip::Esp8266, o, 2, 1));   // b is skipped, a is not
  EQ(DevErr::Duplicate, validateDevice(edit, Chip::Esp8266, o, 2, 0));  // skipping the wrong entry leaves b to clash with
}

void test_edit_pwm_frequency_alone_is_free() {
  DeviceConfig p = dev("p", DeviceType::Pwm, 4);
  p.freqHz = 1000;
  DeviceConfig o[1] = {p};
  DeviceConfig edit = dev("p2", DeviceType::Pwm, 12);
  edit.freqHz = 500;
  EQ(DevErr::FreqConflict, validateDevice(edit, Chip::Esp8266, o, 1));    // a second PWM device must share the frequency
  EQ(DevErr::Ok, validateDevice(edit, Chip::Esp8266, o, 1, 0));           // replacing the only PWM user may change it
}

void test_edit_pwm_frequency_with_other_user_conflicts() {
  DeviceConfig p = dev("p", DeviceType::Pwm, 4);
  p.freqHz = 1000;
  DeviceConfig q = dev("q", DeviceType::Pwm, 12);
  q.freqHz = 1000;
  DeviceConfig o[2] = {p, q};
  DeviceConfig edit = p;
  edit.freqHz = 500;
  EQ(DevErr::FreqConflict, validateDevice(edit, Chip::Esp8266, o, 2, 0));
}

void test_edit_serial_limit_excludes_self() {
  DeviceConfig s = dev("s", DeviceType::Serial, 12);
  s.pin2 = 13;
  s.baud = 9600;
  DeviceConfig o[1] = {s};
  DeviceConfig edit = dev("s2", DeviceType::Serial, 14);
  edit.pin2 = 15;
  edit.baud = 19200;
  EQ(DevErr::SerialLimit, validateDevice(edit, Chip::Esp8266, o, 1));
  EQ(DevErr::Ok, validateDevice(edit, Chip::Esp8266, o, 1, 0));
}

void test_edit_i2c_address_excludes_self() {
  DeviceConfig b = dev("env", DeviceType::Sensor, -1);
  b.model = 1;
  b.i2cAddr = 0x76;
  DeviceConfig o[1] = {b};
  DeviceConfig edit = b;
  strcpy(edit.id, "env2");
  EQ(DevErr::AddrInUse, validateDevice(edit, Chip::Esp8266, o, 1));
  EQ(DevErr::Ok, validateDevice(edit, Chip::Esp8266, o, 1, 0));
}

void test_edit_when_list_is_full() {
  static DeviceConfig o[kMaxDevices];
  for (size_t i = 0; i < kMaxDevices; i++) snprintf(o[i].id, kIdCap, "d%u", unsigned(i));
  DeviceConfig add = dev("new", DeviceType::Relay, 5);
  EQ(DevErr::Full, validateDevice(add, Chip::Esp8266, o, kMaxDevices));
  DeviceConfig edit = dev("d0", DeviceType::Relay, 5);
  TEST_ASSERT_TRUE(validateDevice(edit, Chip::Esp8266, o, kMaxDevices, 0) != DevErr::Full);  // an edit never grows the list
}

// ---- import validation: each device is checked against the ones before it

static DevErr firstFailure(const DeviceConfig* list, size_t n, size_t& at) {
  for (size_t i = 0; i < n; i++) {
    DevErr e = validateDevice(list[i], Chip::Esp8266, list, i);
    if (e != DevErr::Ok) { at = i; return e; }
  }
  return DevErr::Ok;
}

void test_import_list_validation() {
  size_t at = 99;
  DeviceConfig good[3] = {dev("a", DeviceType::Relay, 5), dev("b", DeviceType::Relay, 12), dev("c", DeviceType::Input, 14)};
  EQ(DevErr::Ok, firstFailure(good, 3, at));
  DeviceConfig dup[3] = {good[0], good[1], dev("a", DeviceType::Relay, 14)};
  EQ(DevErr::Duplicate, firstFailure(dup, 3, at));
  EQ(2, at);
  DeviceConfig clash[2] = {good[0], dev("z", DeviceType::Relay, 5)};
  EQ(DevErr::PinInUse, firstFailure(clash, 2, at));
  EQ(1, at);
  DeviceConfig bad[1] = {dev("r", DeviceType::Relay, 6)};  // GPIO6 is flash on the ESP8266
  EQ(DevErr::PinReserved, firstFailure(bad, 1, at));
  EQ(0, at);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_schema_check);
  RUN_TEST(test_config_files_listed);
  RUN_TEST(test_edit_ignores_its_old_self);
  RUN_TEST(test_edit_still_conflicts_with_others);
  RUN_TEST(test_edit_skip_index_is_the_right_one);
  RUN_TEST(test_edit_pwm_frequency_alone_is_free);
  RUN_TEST(test_edit_pwm_frequency_with_other_user_conflicts);
  RUN_TEST(test_edit_serial_limit_excludes_self);
  RUN_TEST(test_edit_i2c_address_excludes_self);
  RUN_TEST(test_edit_when_list_is_full);
  RUN_TEST(test_import_list_validation);
  return UNITY_END();
}
