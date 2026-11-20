#include <unity.h>
#include "net/MqttConfig.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

static MqttConfig defaults() {
  MqttConfig c;
  setMqttDefaults(c, "espwf-abcdef");
  return c;
}

void test_defaults_valid_and_off() {
  MqttConfig c = defaults();
  TEST_ASSERT_FALSE(c.enabled);
  TEST_ASSERT_EQUAL_STRING("espwf-abcdef", c.base);
  TEST_ASSERT_EQUAL_UINT16(1883, c.port);
  TEST_ASSERT_EQUAL(int(MqttCfgError::Ok), int(validateMqttConfig(c)));
}

void test_host_rules() {
  MqttConfig c = defaults();
  c.enabled = true;
  TEST_ASSERT_EQUAL(int(MqttCfgError::Host), int(validateMqttConfig(c)));   // enabled needs a broker
  strcpy(c.host, "192.168.1.10");
  TEST_ASSERT_EQUAL(int(MqttCfgError::Ok), int(validateMqttConfig(c)));
  strcpy(c.host, "broker.home");
  TEST_ASSERT_EQUAL(int(MqttCfgError::Ok), int(validateMqttConfig(c)));
  strcpy(c.host, "bad host");
  TEST_ASSERT_EQUAL(int(MqttCfgError::Host), int(validateMqttConfig(c)));
  strcpy(c.host, "a/b");
  TEST_ASSERT_EQUAL(int(MqttCfgError::Host), int(validateMqttConfig(c)));
  c.enabled = false;                                                         // a bad stored host is rejected even when off
  TEST_ASSERT_EQUAL(int(MqttCfgError::Host), int(validateMqttConfig(c)));
  memset(c.host, 'a', sizeof c.host);                                        // no NUL
  TEST_ASSERT_EQUAL(int(MqttCfgError::Host), int(validateMqttConfig(c)));
}

void test_port_and_keepalive() {
  MqttConfig c = defaults();
  c.port = 0;
  TEST_ASSERT_EQUAL(int(MqttCfgError::Port), int(validateMqttConfig(c)));
  c.port = 65535;
  TEST_ASSERT_EQUAL(int(MqttCfgError::Ok), int(validateMqttConfig(c)));
  c.keepAlive = 4;
  TEST_ASSERT_EQUAL(int(MqttCfgError::KeepAlive), int(validateMqttConfig(c)));
  c.keepAlive = 5;
  TEST_ASSERT_EQUAL(int(MqttCfgError::Ok), int(validateMqttConfig(c)));
  c.keepAlive = 601;
  TEST_ASSERT_EQUAL(int(MqttCfgError::KeepAlive), int(validateMqttConfig(c)));
  c.keepAlive = 600;
  TEST_ASSERT_EQUAL(int(MqttCfgError::Ok), int(validateMqttConfig(c)));
}

void test_base_topic_rules() {
  MqttConfig c = defaults();
  const char* bad[] = {"", "/lead", "trail/", "a//b", "a b", "a+b", "a#", "caf\xc3\xa9"};
  for (const char* b : bad) {
    strcpy(c.base, b);
    TEST_ASSERT_EQUAL_MESSAGE(int(MqttCfgError::Base), int(validateMqttConfig(c)), b);
  }
  const char* good[] = {"irrigation", "farm/valves", "a_b-c/9"};
  for (const char* b : good) {
    strcpy(c.base, b);
    TEST_ASSERT_EQUAL_MESSAGE(int(MqttCfgError::Ok), int(validateMqttConfig(c)), b);
  }
  memset(c.base, 'a', 40); c.base[40] = 0;      // exactly 40
  TEST_ASSERT_EQUAL(int(MqttCfgError::Ok), int(validateMqttConfig(c)));
  memset(c.base, 'a', sizeof c.base);           // no NUL
  TEST_ASSERT_EQUAL(int(MqttCfgError::Base), int(validateMqttConfig(c)));
}

void test_topic_building() {
  char t[kMqttTopicCap];
  TEST_ASSERT_TRUE(mqttTopic(t, sizeof t, "farm", "valve1", "state"));
  TEST_ASSERT_EQUAL_STRING("farm/valve1/state", t);
  TEST_ASSERT_TRUE(mqttTopic(t, sizeof t, "farm", nullptr, "status"));
  TEST_ASSERT_EQUAL_STRING("farm/status", t);
  char small[8];
  TEST_ASSERT_FALSE(mqttTopic(small, sizeof small, "farm", "valve1", "state"));   // does not fit
  char exact[18];                                                                   // "farm/valve1/state" is 17 chars + NUL
  TEST_ASSERT_TRUE(mqttTopic(exact, sizeof exact, "farm", "valve1", "state"));
  char one_short[17];
  TEST_ASSERT_FALSE(mqttTopic(one_short, sizeof one_short, "farm", "valve1", "state"));
}

void test_set_topic_parse() {
  char id[kIdCap];
  TEST_ASSERT_TRUE(mqttParseSetTopic("farm", "farm/valve1/set", id));
  TEST_ASSERT_EQUAL_STRING("valve1", id);
  TEST_ASSERT_TRUE(mqttParseSetTopic("a/b", "a/b/v_2/set", id));
  TEST_ASSERT_EQUAL_STRING("v_2", id);
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farm/valve1/state", id));
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farm/valve1/set/x", id));
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farm//set", id));                   // empty id
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "other/valve1/set", id));
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farmx/valve1/set", id));            // base must end at a '/'
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farmxvalve1/set", id));             // no '/' after the base
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farm/Valve1/set", id));             // ids are lower case
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farm/a/b/set", id));                // nested
  TEST_ASSERT_FALSE(mqttParseSetTopic("farm", "farm/12345678901234567/set", id));  // 17 chars: too long
  TEST_ASSERT_TRUE(mqttParseSetTopic("farm", "farm/1234567890123456/set", id));    // 16: fits
}

static bool cmd(DeviceType t, const char* s, Command& c) { return mqttParseCommand(t, s, strlen(s), c); }

void test_command_relay() {
  Command c;
  TEST_ASSERT_TRUE(cmd(DeviceType::Relay, "on", c));
  TEST_ASSERT_EQUAL(int(CmdAction::Set), int(c.action));
  TEST_ASSERT_TRUE(c.isBool);
  TEST_ASSERT_TRUE(c.value == 1);
  TEST_ASSERT_TRUE(cmd(DeviceType::Relay, "OFF", c));
  TEST_ASSERT_TRUE(c.value == 0);
  TEST_ASSERT_TRUE(cmd(DeviceType::Relay, " true \n", c));
  TEST_ASSERT_TRUE(c.value == 1);
  TEST_ASSERT_TRUE(cmd(DeviceType::Relay, "0", c));
  TEST_ASSERT_TRUE(c.isBool);
  TEST_ASSERT_TRUE(c.value == 0);
  TEST_ASSERT_TRUE(cmd(DeviceType::Relay, "1", c));
  TEST_ASSERT_TRUE(c.isBool);
  TEST_ASSERT_TRUE(c.value == 1);
  TEST_ASSERT_TRUE(cmd(DeviceType::Relay, "toggle", c));
  TEST_ASSERT_EQUAL(int(CmdAction::Toggle), int(c.action));
  TEST_ASSERT_FALSE(cmd(DeviceType::Relay, "50", c));
  TEST_ASSERT_FALSE(cmd(DeviceType::Relay, "maybe", c));
  TEST_ASSERT_FALSE(cmd(DeviceType::Relay, "", c));
  TEST_ASSERT_FALSE(cmd(DeviceType::Relay, "   ", c));
  // Every relay command passes the same check the REST API applies.
  cmd(DeviceType::Relay, "on", c);
  TEST_ASSERT_EQUAL(int(CmdErr::Ok), int(checkCommand(DeviceType::Relay, c)));
}

void test_command_numeric_devices() {
  Command c;
  TEST_ASSERT_TRUE(cmd(DeviceType::Pwm, "75", c));
  TEST_ASSERT_FALSE(c.isBool);
  TEST_ASSERT_TRUE(c.value == 75);
  TEST_ASSERT_EQUAL(int(CmdErr::Ok), int(checkCommand(DeviceType::Pwm, c)));
  TEST_ASSERT_TRUE(cmd(DeviceType::Pwm, "on", c));
  TEST_ASSERT_TRUE(c.value == 100);
  TEST_ASSERT_TRUE(cmd(DeviceType::Pwm, "off", c));
  TEST_ASSERT_TRUE(c.value == 0);
  TEST_ASSERT_TRUE(cmd(DeviceType::Motor, "-40.5", c));
  TEST_ASSERT_TRUE(c.value == -40.5);
  TEST_ASSERT_TRUE(cmd(DeviceType::Motor, "stop", c));
  TEST_ASSERT_EQUAL(int(CmdAction::Stop), int(c.action));
  TEST_ASSERT_TRUE(cmd(DeviceType::Motor, "off", c));
  TEST_ASSERT_EQUAL(int(CmdAction::Stop), int(c.action));
  TEST_ASSERT_TRUE(cmd(DeviceType::Servo, "90", c));
  TEST_ASSERT_TRUE(c.value == 90);
  TEST_ASSERT_FALSE(cmd(DeviceType::Servo, "90deg", c));    // the whole text must be a number
  TEST_ASSERT_FALSE(cmd(DeviceType::Servo, "nan", c));
  TEST_ASSERT_FALSE(cmd(DeviceType::Servo, "inf", c));
  TEST_ASSERT_FALSE(cmd(DeviceType::Servo, "", c));
  // Out of range still parses: the registry refuses it with bad_value.
  TEST_ASSERT_TRUE(cmd(DeviceType::Servo, "999", c));
  TEST_ASSERT_EQUAL(int(CmdErr::BadValue), int(checkCommand(DeviceType::Servo, c)));
}

void test_command_serial_and_limits() {
  Command c;
  TEST_ASSERT_TRUE(cmd(DeviceType::Serial, "Hello World", c));    // text is kept as sent
  TEST_ASSERT_EQUAL(int(CmdAction::Send), int(c.action));
  TEST_ASSERT_EQUAL_STRING("Hello World", c.text);
  TEST_ASSERT_EQUAL(int(CmdErr::Ok), int(checkCommand(DeviceType::Serial, c)));
  char big[kTextCap + 1];
  memset(big, 'a', sizeof big);
  TEST_ASSERT_FALSE(mqttParseCommand(DeviceType::Serial, big, kTextCap, c));        // 48 chars: too long
  TEST_ASSERT_TRUE(mqttParseCommand(DeviceType::Serial, big, kTextCap - 1, c));     // 47: fits
  TEST_ASSERT_FALSE(mqttParseCommand(DeviceType::Relay, "on", 0, c));
}

void test_backoff() {
  TEST_ASSERT_EQUAL_UINT32(5000, mqttBackoffMs(0));
  TEST_ASSERT_EQUAL_UINT32(10000, mqttBackoffMs(1));
  TEST_ASSERT_EQUAL_UINT32(20000, mqttBackoffMs(2));
  TEST_ASSERT_EQUAL_UINT32(40000, mqttBackoffMs(3));
  TEST_ASSERT_EQUAL_UINT32(60000, mqttBackoffMs(4));
  TEST_ASSERT_EQUAL_UINT32(60000, mqttBackoffMs(5));
  TEST_ASSERT_EQUAL_UINT32(60000, mqttBackoffMs(1000));
}

void test_signature() {
  MqttSig a = mqttSig(true, 0, 0, 5, 1, true, false, 21.04f, 40.0f, 1000.0f);
  MqttSig b = mqttSig(true, 0, 0, 5, 1, true, false, 21.04f, 40.0f, 1000.0f);
  TEST_ASSERT_TRUE(mqttSigSame(a, b));
  TEST_ASSERT_TRUE(mqttSigSame(a, mqttSig(true, 0, 0, 5, 1, true, false, 21.01f, 40.0f, 1000.0f)));   // same tenth
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 0, 5, 1, true, false, 21.2f, 40.0f, 1000.0f)));   // temp moved
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 0, 5, 1, true, false, 21.04f, 40.3f, 1000.0f)));  // humidity
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 0, 5, 1, true, false, 21.04f, 40.0f, 1001.0f)));  // pressure
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(false, 0, 0, 5, 1, true, false, 21.04f, 40.0f, 1000.0f)));
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 1, 0, 5, 1, true, false, 21.04f, 40.0f, 1000.0f)));
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 1, 5, 1, true, false, 21.04f, 40.0f, 1000.0f)));
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 0, 6, 1, true, false, 21.04f, 40.0f, 1000.0f)));  // edges
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 0, 5, 2, true, false, 21.04f, 40.0f, 1000.0f)));  // long presses
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 0, 5, 1, false, false, 21.04f, 40.0f, 1000.0f))); // sensor ok
  TEST_ASSERT_FALSE(mqttSigSame(a, mqttSig(true, 0, 0, 5, 1, true, true, 21.04f, 40.0f, 1000.0f)));   // failsafe tripped
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_valid_and_off);
  RUN_TEST(test_host_rules);
  RUN_TEST(test_port_and_keepalive);
  RUN_TEST(test_base_topic_rules);
  RUN_TEST(test_topic_building);
  RUN_TEST(test_set_topic_parse);
  RUN_TEST(test_command_relay);
  RUN_TEST(test_command_numeric_devices);
  RUN_TEST(test_command_serial_and_limits);
  RUN_TEST(test_backoff);
  RUN_TEST(test_signature);
  return UNITY_END();
}
