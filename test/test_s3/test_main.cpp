#include <unity.h>
#include <initializer_list>

#include "devices/DeviceTypes.h"
#include "devices/PinInfo.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

#define EQ(a, b) TEST_ASSERT_EQUAL(int(a), int(b))
static const Chip S3 = Chip::Esp32S3;

static DeviceConfig dev(const char* id, DeviceType t, int pin) {
  DeviceConfig c;
  strcpy(c.id, id);
  c.type = t;
  c.pin = int8_t(pin);
  return c;
}

void test_chip_enum_is_appended() {
  EQ(0, Chip::Esp8266);
  EQ(1, Chip::Esp32);       // the classic ESP32 keeps its value
  EQ(2, Chip::Esp32S3);
}

void test_s3_pin_ranges() {
  EQ(PinErr::Ok, checkPin(S3, 0, PinRole::Output));
  EQ(PinErr::Ok, checkPin(S3, 4, PinRole::Pwm));
  EQ(PinErr::Ok, checkPin(S3, 18, PinRole::Output));
  EQ(PinErr::Ok, checkPin(S3, 21, PinRole::Output));
  EQ(PinErr::Ok, checkPin(S3, 38, PinRole::Output));
  EQ(PinErr::Ok, checkPin(S3, 42, PinRole::Output));
  EQ(PinErr::Ok, checkPin(S3, 47, PinRole::Output));
  EQ(PinErr::Ok, checkPin(S3, 48, PinRole::Output));
  EQ(PinErr::Invalid, checkPin(S3, 49, PinRole::Input));
  EQ(PinErr::Invalid, checkPin(S3, -1, PinRole::Input));
  for (int p = 22; p <= 25; p++) EQ(PinErr::Invalid, checkPin(S3, p, PinRole::Input));    // these GPIOs do not exist
}

void test_s3_reserved_pins() {
  for (int p = 26; p <= 37; p++) EQ(PinErr::Reserved, checkPin(S3, p, PinRole::Input));   // flash, octal PSRAM
  EQ(PinErr::Reserved, checkPin(S3, 19, PinRole::Output));                                // native USB
  EQ(PinErr::Reserved, checkPin(S3, 20, PinRole::Input));
  EQ(PinErr::Reserved, checkPin(S3, 43, PinRole::Output));                                // UART0 (the serial log)
  EQ(PinErr::Reserved, checkPin(S3, 44, PinRole::Input));
  EQ(PinErr::Ok, checkPin(S3, 18, PinRole::Input));
  EQ(PinErr::Ok, checkPin(S3, 38, PinRole::Input));
}

void test_s3_gpio46_input_only() {
  EQ(PinErr::Ok, checkPin(S3, 46, PinRole::Input));
  EQ(PinErr::InputOnly, checkPin(S3, 46, PinRole::Output));
  EQ(PinErr::InputOnly, checkPin(S3, 46, PinRole::Pwm));
  EQ(PinErr::Ok, checkPin(S3, 45, PinRole::Output));
}

void test_s3_traits() {
  TEST_ASSERT_TRUE(pinHasPullup(S3, 4));
  TEST_ASSERT_TRUE(pinHasPullup(S3, 45));
  TEST_ASSERT_FALSE(pinHasPullup(S3, 46));
  for (int p : {0, 3, 45, 46}) TEST_ASSERT_TRUE(pinBootSensitive(S3, p));
  for (int p : {1, 2, 4, 5, 12, 15, 38}) TEST_ASSERT_FALSE(pinBootSensitive(S3, p));
  TEST_ASSERT_EQUAL_STRING("", pinLabel(S3, 5));            // no NodeMCU-style labels
}

void test_s3_notes() {
  TEST_ASSERT_EQUAL_STRING("flash", pinNote(S3, 30));
  TEST_ASSERT_EQUAL_STRING("usb", pinNote(S3, 19));
  TEST_ASSERT_EQUAL_STRING("usb", pinNote(S3, 20));
  TEST_ASSERT_EQUAL_STRING("uart", pinNote(S3, 43));
  TEST_ASSERT_EQUAL_STRING("uart", pinNote(S3, 44));
  TEST_ASSERT_EQUAL_STRING("strapping", pinNote(S3, 45));
  TEST_ASSERT_EQUAL_STRING("strapping", pinNote(S3, 0));
  TEST_ASSERT_EQUAL_STRING("", pinNote(S3, 4));
  // The classic ESP32 notes are unchanged: 1 and 3 are its UART.
  TEST_ASSERT_EQUAL_STRING("uart", pinNote(Chip::Esp32, 1));
  TEST_ASSERT_EQUAL_STRING("flash", pinNote(Chip::Esp32, 6));
  TEST_ASSERT_TRUE(pinListed(S3, 38));
  TEST_ASSERT_FALSE(pinListed(S3, 23));
  TEST_ASSERT_TRUE(pinListed(S3, 30));                       // listed (reserved ones are shown, never read)
  TEST_ASSERT_FALSE(pinReadable(S3, 30));
  TEST_ASSERT_TRUE(pinReadable(S3, 4));
}

void test_s3_i2c_pins_and_baud() {
  EQ(8, i2cSda(S3));
  EQ(9, i2cScl(S3));
  EQ(21, i2cSda(Chip::Esp32));
  EQ(22, i2cScl(Chip::Esp32));
  EQ(4, i2cSda(Chip::Esp8266));
  EQ(5, i2cScl(Chip::Esp8266));
  TEST_ASSERT_TRUE(baudOk(S3, 115200));        // a hardware UART: not limited to 38400 like the ESP8266's software serial
  TEST_ASSERT_TRUE(baudOk(Chip::Esp32, 115200));
  TEST_ASSERT_FALSE(baudOk(Chip::Esp8266, 115200));
  TEST_ASSERT_FALSE(baudOk(S3, 12345));
}

void test_s3_validates_devices() {
  EQ(DevErr::Ok, validateDevice(dev("r1", DeviceType::Relay, 4), S3, nullptr, 0));
  EQ(DevErr::PinReserved, validateDevice(dev("r1", DeviceType::Relay, 30), S3, nullptr, 0));
  EQ(DevErr::PinReserved, validateDevice(dev("r1", DeviceType::Relay, 43), S3, nullptr, 0));
  EQ(DevErr::Pin, validateDevice(dev("r1", DeviceType::Relay, 23), S3, nullptr, 0));
  EQ(DevErr::PinInputOnly, validateDevice(dev("r1", DeviceType::Relay, 46), S3, nullptr, 0));
  EQ(DevErr::Ok, validateDevice(dev("b1", DeviceType::Input, 46), S3, nullptr, 0));
}

void test_s3_pwm_shares_one_frequency() {
  // 8 LEDC channels share 4 timers on the S3, so (like the ESP8266) every PWM device uses one frequency.
  DeviceConfig o[1] = {dev("p", DeviceType::Pwm, 4)};               // 1000 Hz
  EQ(DevErr::FreqConflict, validateDevice(dev("s", DeviceType::Servo, 5), S3, o, 1));   // a servo is 50 Hz
  EQ(DevErr::FreqConflict, validateDevice(dev("s", DeviceType::Servo, 5), Chip::Esp8266, o, 1));
  EQ(DevErr::Ok, validateDevice(dev("s", DeviceType::Servo, 5), Chip::Esp32, o, 1));    // the classic ESP32 keeps one timer per slot
  DeviceConfig same = dev("p2", DeviceType::Pwm, 6);
  EQ(DevErr::Ok, validateDevice(same, S3, o, 1));                                       // the same frequency is fine
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_chip_enum_is_appended);
  RUN_TEST(test_s3_pin_ranges);
  RUN_TEST(test_s3_reserved_pins);
  RUN_TEST(test_s3_gpio46_input_only);
  RUN_TEST(test_s3_traits);
  RUN_TEST(test_s3_notes);
  RUN_TEST(test_s3_i2c_pins_and_baud);
  RUN_TEST(test_s3_validates_devices);
  RUN_TEST(test_s3_pwm_shares_one_frequency);
  return UNITY_END();
}
