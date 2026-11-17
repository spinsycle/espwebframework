#include <unity.h>
#include <math.h>
#include <stdio.h>

#include "devices/DeviceTypes.h"
#include "devices/SensorMath.h"

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
static DeviceConfig motor(const char* id, int pwm, int d1, int d2 = -1) {
  DeviceConfig c = dev(id, DeviceType::Motor, pwm);
  c.pin2 = int8_t(d1);
  c.pin3 = int8_t(d2);
  return c;
}
static DeviceConfig pca(const char* id, int addr, int ch) {
  DeviceConfig c = dev(id, DeviceType::Servo, -1);
  c.model = 1;
  c.i2cAddr = uint8_t(addr);
  c.channel = uint8_t(ch);
  return c;
}
static DeviceConfig bme(const char* id, int addr) {
  DeviceConfig c = dev(id, DeviceType::Sensor, -1);
  c.model = 1;
  c.i2cAddr = uint8_t(addr);
  return c;
}
static DevErr check(const DeviceConfig& c, Chip chip, const DeviceConfig* o = nullptr, size_t n = 0) {
  return validateDevice(c, chip, o, n);
}

// ---- motor

void test_motor_valid_and_pins() {
  EQ(DevErr::Ok, check(motor("m", 12, 13), Chip::Esp8266));
  EQ(DevErr::Ok, check(motor("m", 12, 13, 14), Chip::Esp8266));
  EQ(DevErr::PinReserved, check(motor("m", 12, 6), Chip::Esp8266));          // dir pin on flash
  EQ(DevErr::PinReserved, check(motor("m", 12, 13, 7), Chip::Esp8266));      // IN2 on flash
  EQ(DevErr::PinNoPwm, check(motor("m", 16, 13), Chip::Esp8266));            // enable needs PWM
  EQ(DevErr::PinInputOnly, check(motor("m", 26, 34), Chip::Esp32));          // dir is an output
  EQ(DevErr::PinInUse, check(motor("m", 12, 12), Chip::Esp8266));            // same pin twice
  EQ(DevErr::PinInUse, check(motor("m", 12, 13, 13), Chip::Esp8266));
  EQ(DevErr::Pin, check(motor("m", 12, -1), Chip::Esp8266));                 // direction pin required
}

void test_motor_pins_collide_with_others() {
  DeviceConfig o[1] = {dev("r", DeviceType::Relay, 13)};
  EQ(DevErr::PinInUse, check(motor("m", 12, 13), Chip::Esp8266, o, 1));
  EQ(DevErr::PinInUse, check(motor("m", 12, 14, 13), Chip::Esp8266, o, 1));  // IN2 collides too
  DeviceConfig o2[1] = {motor("a", 12, 13, 14)};
  EQ(DevErr::PinInUse, check(dev("x", DeviceType::Relay, 14), Chip::Esp8266, o2, 1));  // pin3 is owned
  EQ(DevErr::PinInUse, check(dev("x", DeviceType::Relay, 13), Chip::Esp8266, o2, 1));  // pin2 is owned
}

void test_motor_ranges() {
  DeviceConfig c = motor("m", 12, 13);
  c.initial = 1;
  EQ(DevErr::Initial, check(c, Chip::Esp8266));  // always boots stopped
  c = motor("m", 12, 13);
  c.freqHz = 0;
  EQ(DevErr::Freq, check(c, Chip::Esp8266));
  c.freqHz = 20001;
  EQ(DevErr::Freq, check(c, Chip::Esp8266));
}

void test_motor_drive() {
  MotorDrive d = motorDrive(60, false, false);
  EQ(0, d.dir1); EQ(60, d.duty);
  d = motorDrive(-60, false, false);
  EQ(1, d.dir1); EQ(60, d.duty);
  d = motorDrive(-60, true, false);  // invert flips direction
  EQ(0, d.dir1);
  d = motorDrive(60, false, true);
  EQ(1, d.dir1); EQ(0, d.dir2);
  d = motorDrive(-60, false, true);
  EQ(0, d.dir1); EQ(1, d.dir2);
  d = motorDrive(-60, true, true);
  EQ(1, d.dir1); EQ(0, d.dir2);
  d = motorDrive(0, false, true);  // stopped: both low
  EQ(0, d.dir1); EQ(0, d.dir2); EQ(0, d.duty);
  d = motorDrive(250, false, false);  // clamps
  EQ(100, d.duty);
  d = motorDrive(-250, false, false);
  EQ(100, d.duty);
}

// ---- servo

void test_servo_direct() {
  DeviceConfig c = dev("s", DeviceType::Servo, 14);
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  EQ(DevErr::PinNoPwm, check(dev("s", DeviceType::Servo, 16), Chip::Esp8266));
  c.initial = 181;
  EQ(DevErr::Initial, check(c, Chip::Esp8266));
  c.initial = 180;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.minUs = 299;
  EQ(DevErr::Pulse, check(c, Chip::Esp8266));
  c.minUs = 300; c.maxUs = 3001;
  EQ(DevErr::Pulse, check(c, Chip::Esp8266));
  c.maxUs = 3000;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.minUs = 1500; c.maxUs = 1500;  // min must be below max
  EQ(DevErr::Pulse, check(c, Chip::Esp8266));
}

void test_servo_pca9685() {
  EQ(DevErr::Ok, check(pca("s", 0x40, 0), Chip::Esp8266));
  EQ(DevErr::Ok, check(pca("s", 0x7F, 15), Chip::Esp8266));
  EQ(DevErr::I2cAddr, check(pca("s", 0x3F, 0), Chip::Esp8266));
  EQ(DevErr::I2cAddr, check(pca("s", 0x80, 0), Chip::Esp8266));
  EQ(DevErr::I2cAddr, check(pca("s", 0x70, 0), Chip::Esp8266));  // all-call address
  EQ(DevErr::Channel, check(pca("s", 0x40, 16), Chip::Esp8266));
}

void test_i2c_shared_bus_and_addresses() {
  DeviceConfig o[2] = {pca("a", 0x40, 0), bme("b", 0x76)};
  EQ(DevErr::Ok, check(pca("c", 0x40, 1), Chip::Esp8266, o, 2));             // another channel on the same board
  EQ(DevErr::AddrInUse, check(pca("c", 0x40, 0), Chip::Esp8266, o, 2));      // same board, same channel
  EQ(DevErr::AddrInUse, check(bme("c", 0x76), Chip::Esp8266, o, 2));         // two sensors, one address
  EQ(DevErr::Ok, check(bme("c", 0x77), Chip::Esp8266, o, 2));
  EQ(DevErr::AddrInUse, check(pca("c", 0x76, 0), Chip::Esp8266, o, 2));      // servo board on a sensor's address
  // the bus pins belong to the I2C devices
  EQ(DevErr::PinInUse, check(dev("i", DeviceType::Input, 4), Chip::Esp8266, o, 2));
  EQ(DevErr::PinInUse, check(dev("i", DeviceType::Input, 5), Chip::Esp8266, o, 2));
  EQ(DevErr::Ok, check(dev("i", DeviceType::Input, 12), Chip::Esp8266, o, 2));
  EQ(DevErr::PinInUse, check(dev("i", DeviceType::Input, 21), Chip::Esp32, o, 2));
  EQ(DevErr::PinInUse, check(dev("i", DeviceType::Input, 22), Chip::Esp32, o, 2));
  // and the other way round: an I2C device is refused when something already sits on the bus pins
  DeviceConfig b[1] = {dev("btn", DeviceType::Input, 4)};
  EQ(DevErr::PinInUse, check(bme("c", 0x76), Chip::Esp8266, b, 1));
  EQ(DevErr::Ok, check(bme("c", 0x76), Chip::Esp32, b, 1));  // GPIO4 is not the ESP32 bus
}

void test_i2c_pins_free() {
  DeviceConfig o[2] = {dev("a", DeviceType::Input, 12), pca("p", 0x40, 0)};
  TEST_ASSERT_TRUE(i2cPinsFree(Chip::Esp8266, o, 2));
  o[0].pin = 4;
  TEST_ASSERT_FALSE(i2cPinsFree(Chip::Esp8266, o, 2));
  o[0].pin = 5;
  TEST_ASSERT_FALSE(i2cPinsFree(Chip::Esp8266, o, 2));
  TEST_ASSERT_TRUE(i2cPinsFree(Chip::Esp32, o, 2));
  DeviceConfig m[1] = {motor("m", 12, 13, 5)};  // IN2 on the SCL pin
  TEST_ASSERT_FALSE(i2cPinsFree(Chip::Esp8266, m, 1));
  TEST_ASSERT_EQUAL(4, i2cSda(Chip::Esp8266));
  TEST_ASSERT_EQUAL(5, i2cScl(Chip::Esp8266));
  TEST_ASSERT_EQUAL(21, i2cSda(Chip::Esp32));
  TEST_ASSERT_EQUAL(22, i2cScl(Chip::Esp32));
}

void test_servo_math() {
  EQ(500, servoPulseUs(0, 500, 2400));
  EQ(2400, servoPulseUs(180, 500, 2400));
  EQ(1450, servoPulseUs(90, 500, 2400));
  EQ(500, servoPulseUs(-5, 500, 2400));   // clamps
  EQ(2400, servoPulseUs(999, 500, 2400));
  EQ(1500, servoPulseUs(90, 1000, 2000));
  EQ(511, servoPulseUs(1, 500, 2400));   // 10.55 rounds up
  EQ(751, servoDuty10k(1501));            // 750.5 rounds up
  EQ(328, pcaTicks(1600));                // 327.68 rounds up
  EQ(750, servoDuty10k(1500));            // 1.5 ms of 20 ms = 7.5 %
  EQ(250, servoDuty10k(500));
  EQ(307, pcaTicks(1500));                // 1500/20000 * 4096 = 307.2
  EQ(4096, pcaTicks(20000));
}

// ---- sensor

void test_sensor_validation() {
  DeviceConfig c = dev("t", DeviceType::Sensor, 13);
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  EQ(DevErr::PinInputOnly, check(dev("t", DeviceType::Sensor, 34), Chip::Esp32));  // 1-Wire needs an output
  c.intervalS = 0;
  EQ(DevErr::Interval, check(c, Chip::Esp8266));
  c.intervalS = 3601;
  EQ(DevErr::Interval, check(c, Chip::Esp8266));
  c.intervalS = 3600;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  EQ(DevErr::I2cAddr, check(bme("b", 0x75), Chip::Esp8266));
  EQ(DevErr::I2cAddr, check(bme("b", 0x78), Chip::Esp8266));
  EQ(DevErr::Ok, check(bme("b", 0x77), Chip::Esp8266));
  c = bme("b", 0x76);
  c.model = 2;
  EQ(DevErr::Model, check(c, Chip::Esp8266));
  DeviceConfig o[1] = {dev("t", DeviceType::Sensor, 13)};
  EQ(DevErr::PinInUse, check(dev("u", DeviceType::Relay, 13), Chip::Esp8266, o, 1));
}

// ---- serial

void test_serial_validation() {
  DeviceConfig c = dev("u", DeviceType::Serial, 12);
  c.pin2 = 13;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.pin2 = 12;
  EQ(DevErr::PinInUse, check(c, Chip::Esp8266));
  c.pin2 = 13; c.baud = 115200;
  EQ(DevErr::Baud, check(c, Chip::Esp8266));   // software serial tops out at 38400
  EQ(DevErr::Ok, check(c, Chip::Esp32));
  c.baud = 57600;
  EQ(DevErr::Baud, check(c, Chip::Esp8266));
  EQ(DevErr::Ok, check(c, Chip::Esp32));
  c.baud = 38400;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.baud = 1200;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.baud = 1201;
  EQ(DevErr::Baud, check(c, Chip::Esp8266));
  c.baud = 12345;
  EQ(DevErr::Baud, check(c, Chip::Esp32));
  c.baud = 9600;
  DeviceConfig e32 = dev("u", DeviceType::Serial, 34);  // RX may be input-only, TX may not
  e32.pin2 = 17;
  EQ(DevErr::Ok, check(e32, Chip::Esp32));
  e32.pin = 16; e32.pin2 = 35;
  EQ(DevErr::PinInputOnly, check(e32, Chip::Esp32));
  DeviceConfig o[1] = {c};
  DeviceConfig d2 = dev("v", DeviceType::Serial, 14);
  d2.pin2 = 15;
  EQ(DevErr::SerialLimit, check(d2, Chip::Esp8266, o, 1));  // one serial device
  EQ(DevErr::PinInUse, check(dev("w", DeviceType::Relay, 13), Chip::Esp8266, o, 1));  // TX pin is owned
}

// ---- PWM sharing

void test_pwm_slots_and_frequency() {
  DeviceConfig pw = dev("p", DeviceType::Pwm, 14);   // 1000 Hz
  DeviceConfig o[1] = {pw};
  EQ(DevErr::FreqConflict, check(dev("s", DeviceType::Servo, 12), Chip::Esp8266, o, 1));  // 50 Hz vs 1000 Hz
  EQ(DevErr::Ok, check(dev("s", DeviceType::Servo, 12), Chip::Esp32, o, 1));              // ESP32 has per-channel timers
  EQ(DevErr::Ok, check(motor("m", 12, 13), Chip::Esp8266, o, 1));                         // motor default is 1000 Hz
  DeviceConfig m2 = motor("m", 12, 13);
  m2.freqHz = 2000;
  EQ(DevErr::FreqConflict, check(m2, Chip::Esp8266, o, 1));
  DeviceConfig p50 = dev("q", DeviceType::Pwm, 14);
  p50.freqHz = 50;
  EQ(DevErr::Ok, check(dev("s", DeviceType::Servo, 12), Chip::Esp8266, &p50, 1));  // same 50 Hz: no conflict
  DeviceConfig sv[1] = {dev("s", DeviceType::Servo, 12)};
  EQ(DevErr::Ok, check(pca("x", 0x40, 0), Chip::Esp8266, sv, 1));                         // PCA servos use no PWM slot
  // all eight slots can be any mix of pwm, motor and direct servo
  DeviceConfig mix[kMaxPwm];
  for (size_t i = 0; i < kMaxPwm; i++) {
    snprintf(mix[i].id, sizeof mix[i].id, "x%u", unsigned(i));
    mix[i].pin = int8_t(12 + i);
    mix[i].type = i % 3 == 0 ? DeviceType::Pwm : i % 3 == 1 ? DeviceType::Motor : DeviceType::Servo;
    mix[i].pin2 = int8_t(30 + i);
  }
  EQ(DevErr::PwmLimit, check(dev("n", DeviceType::Pwm, 2), Chip::Esp32, mix, kMaxPwm));
  EQ(DevErr::PwmLimit, check(dev("n", DeviceType::Servo, 2), Chip::Esp32, mix, kMaxPwm));
  EQ(DevErr::Ok, check(pca("n", 0x40, 0), Chip::Esp32, mix, kMaxPwm));
}

void test_input_long_press_range() {
  DeviceConfig c = dev("b", DeviceType::Input, 4);
  c.longMs = 0;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.longMs = 99;
  EQ(DevErr::LongMs, check(c, Chip::Esp8266));
  c.longMs = 100;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.longMs = 10000;
  EQ(DevErr::Ok, check(c, Chip::Esp8266));
  c.longMs = 10001;
  EQ(DevErr::LongMs, check(c, Chip::Esp8266));
}

// ---- commands

static Command num(CmdAction a, double v) {
  Command c;
  c.action = a; c.hasValue = true; c.value = v;
  return c;
}
static Command text(const char* s) {
  Command c;
  c.action = CmdAction::Send; c.isText = true;
  strncpy(c.text, s, sizeof c.text - 1);
  return c;
}

void test_motor_servo_commands() {
  EQ(CmdErr::Ok, checkCommand(DeviceType::Motor, num(CmdAction::Set, 100)));
  EQ(CmdErr::Ok, checkCommand(DeviceType::Motor, num(CmdAction::Set, -100)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Motor, num(CmdAction::Set, 100.5)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Motor, num(CmdAction::Set, -101)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Motor, num(CmdAction::Set, NAN)));
  Command stop;
  stop.action = CmdAction::Stop;
  EQ(CmdErr::Ok, checkCommand(DeviceType::Motor, stop));
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Servo, stop));   // a servo has no stop
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Motor, num(CmdAction::Toggle, 1)));
  Command b = num(CmdAction::Set, 1);
  b.isBool = true;
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Motor, b));
  Command none;
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Motor, none));
  Command noval;
  noval.action = CmdAction::Set;
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Motor, noval));
  EQ(CmdErr::Ok, checkCommand(DeviceType::Servo, num(CmdAction::Set, 0)));
  EQ(CmdErr::Ok, checkCommand(DeviceType::Servo, num(CmdAction::Set, 180)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Servo, num(CmdAction::Set, -1)));
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Servo, num(CmdAction::Set, 181)));
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Servo, num(CmdAction::Toggle, 1)));
  // the older types refuse the new actions
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Relay, stop));
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Pwm, stop));
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Relay, text("x")));
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Pwm, text("x")));
}

void test_serial_and_sensor_commands() {
  EQ(CmdErr::Ok, checkCommand(DeviceType::Serial, text("AT")));
  EQ(CmdErr::Ok, checkCommand(DeviceType::Serial, text("x")));  // one character is enough
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Serial, text("")));
  char longest[kTextCap];
  memset(longest, 'a', sizeof longest - 1);
  longest[sizeof longest - 1] = 0;
  EQ(CmdErr::Ok, checkCommand(DeviceType::Serial, text(longest)));  // 47 characters
  Command nul;
  nul.action = CmdAction::Send;
  nul.isText = true;
  memset(nul.text, 'a', sizeof nul.text);                           // no terminator
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Serial, nul));
  Command notText = num(CmdAction::Send, 1);
  strcpy(notText.text, "abc");  // leftover text without isText is not a line to send
  EQ(CmdErr::BadValue, checkCommand(DeviceType::Serial, notText));
  EQ(CmdErr::BadAction, checkCommand(DeviceType::Serial, num(CmdAction::Set, 1)));
  EQ(CmdErr::NotCommandable, checkCommand(DeviceType::Sensor, num(CmdAction::Set, 1)));
  EQ(CmdErr::NotCommandable, checkCommand(DeviceType::Sensor, Command()));
}

void test_names_and_actions() {
  DeviceType t;
  TEST_ASSERT_TRUE(parseDeviceType("motor", t)); EQ(DeviceType::Motor, t);
  TEST_ASSERT_TRUE(parseDeviceType("servo", t)); EQ(DeviceType::Servo, t);
  TEST_ASSERT_TRUE(parseDeviceType("sensor", t)); EQ(DeviceType::Sensor, t);
  TEST_ASSERT_TRUE(parseDeviceType("serial", t)); EQ(DeviceType::Serial, t);
  TEST_ASSERT_FALSE(parseDeviceType("stepper", t));
  TEST_ASSERT_EQUAL_STRING("motor", deviceTypeName(DeviceType::Motor));
  TEST_ASSERT_EQUAL_STRING("servo", deviceTypeName(DeviceType::Servo));
  TEST_ASSERT_EQUAL_STRING("sensor", deviceTypeName(DeviceType::Sensor));
  TEST_ASSERT_EQUAL_STRING("serial", deviceTypeName(DeviceType::Serial));
  CmdAction a;
  TEST_ASSERT_TRUE(parseAction("stop", a)); EQ(CmdAction::Stop, a);
  TEST_ASSERT_TRUE(parseAction("send", a)); EQ(CmdAction::Send, a);
  uint8_t m;
  TEST_ASSERT_TRUE(parseModel(DeviceType::Servo, "pca9685", m)); EQ(1, m);
  TEST_ASSERT_TRUE(parseModel(DeviceType::Servo, "direct", m)); EQ(0, m);
  TEST_ASSERT_TRUE(parseModel(DeviceType::Sensor, "bme280", m)); EQ(1, m);
  TEST_ASSERT_TRUE(parseModel(DeviceType::Sensor, "ds18b20", m)); EQ(0, m);
  TEST_ASSERT_FALSE(parseModel(DeviceType::Sensor, "pca9685", m));
  TEST_ASSERT_EQUAL_STRING("pca9685", modelName(DeviceType::Servo, 1));
  TEST_ASSERT_EQUAL_STRING("bme280", modelName(DeviceType::Sensor, 1));
}

void test_i2c_names_and_defaults() {
  TEST_ASSERT_EQUAL_STRING("BME280 / BMP280", i2cKnownName(0x76));
  TEST_ASSERT_EQUAL_STRING("BME280 / BMP280", i2cKnownName(0x77));
  TEST_ASSERT_EQUAL_STRING("PCA9685 all-call", i2cKnownName(0x70));
  TEST_ASSERT_EQUAL_STRING("PCA9685 or similar", i2cKnownName(0x40));
  TEST_ASSERT_EQUAL_STRING("PCA9685 or similar", i2cKnownName(0x7F));
  TEST_ASSERT_EQUAL_STRING("", i2cKnownName(0x3F));
  TEST_ASSERT_EQUAL_STRING("OLED display", i2cKnownName(0x3C));
  TEST_ASSERT_EQUAL_STRING("OLED display", i2cKnownName(0x3D));
  TEST_ASSERT_EQUAL_STRING("", i2cKnownName(0x10));
  EQ(0x40, defaultI2cAddr(pca("s", 0, 0)));
  EQ(0x76, defaultI2cAddr(bme("b", 0)));
}

void test_new_error_codes() {
  TEST_ASSERT_EQUAL_STRING("bad_model", devErrCode(DevErr::Model));
  TEST_ASSERT_EQUAL_STRING("model", devErrField(DevErr::Model));
  TEST_ASSERT_EQUAL_STRING("bad_i2c_addr", devErrCode(DevErr::I2cAddr));
  TEST_ASSERT_EQUAL_STRING("i2c_addr", devErrField(DevErr::I2cAddr));
  TEST_ASSERT_EQUAL_STRING("address_in_use", devErrCode(DevErr::AddrInUse));
  TEST_ASSERT_EQUAL_STRING("i2c_addr", devErrField(DevErr::AddrInUse));
  TEST_ASSERT_EQUAL_STRING("bad_channel", devErrCode(DevErr::Channel));
  TEST_ASSERT_EQUAL_STRING("channel", devErrField(DevErr::Channel));
  TEST_ASSERT_EQUAL_STRING("bad_pulse", devErrCode(DevErr::Pulse));
  TEST_ASSERT_EQUAL_STRING("min_us", devErrField(DevErr::Pulse));
  TEST_ASSERT_EQUAL_STRING("bad_interval", devErrCode(DevErr::Interval));
  TEST_ASSERT_EQUAL_STRING("interval_s", devErrField(DevErr::Interval));
  TEST_ASSERT_EQUAL_STRING("bad_baud", devErrCode(DevErr::Baud));
  TEST_ASSERT_EQUAL_STRING("baud", devErrField(DevErr::Baud));
  TEST_ASSERT_EQUAL_STRING("bad_long_ms", devErrCode(DevErr::LongMs));
  TEST_ASSERT_EQUAL_STRING("long_ms", devErrField(DevErr::LongMs));
  TEST_ASSERT_EQUAL_STRING("freq_conflict", devErrCode(DevErr::FreqConflict));
  TEST_ASSERT_EQUAL_STRING("freq_hz", devErrField(DevErr::FreqConflict));
  TEST_ASSERT_EQUAL_STRING("too_many_serial", devErrCode(DevErr::SerialLimit));
  TEST_ASSERT_EQUAL_STRING("type", devErrField(DevErr::SerialLimit));
}

// ---- sensor math

// Bosch BMP280 datasheet example (section 3.12): the pressure and temperature formulas are the BME280's too.
static Bme280Calib datasheetCalib() {
  Bme280Calib c = {};
  c.T1 = 27504; c.T2 = 26435; c.T3 = -1000;
  c.P1 = 36477; c.P2 = -10685; c.P3 = 3024; c.P4 = 2855; c.P5 = 140;
  c.P6 = -7; c.P7 = 15500; c.P8 = -14600; c.P9 = 6000;
  return c;
}

void test_bme280_datasheet_example() {
  Bme280Calib c = datasheetCalib();
  int32_t tf;
  float t = bme280Temp(c, 519888, tf);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 25.08f, t);
  TEST_ASSERT_EQUAL(128422, tf);
  float p = bme280Press(c, 415148, tf);
  TEST_ASSERT_FLOAT_WITHIN(0.0003f, 1006.53266f, p);  // 100653.27 Pa
}

void test_bme280_parse() {
  uint8_t a[26] = {0}, h[7] = {0};
  a[0] = 0x70; a[1] = 0x6B;          // T1 = 27504
  a[2] = 0x43; a[3] = 0x67;          // T2 = 26435
  a[4] = 0x18; a[5] = 0xFC;          // T3 = -1000
  a[8] = 0x43; a[9] = 0xD6;          // P2 = -10685
  a[25] = 75;                         // H1
  h[0] = 0x70; h[1] = 0x01;          // H2 = 368
  h[2] = 0;                           // H3
  h[3] = 0x13; h[4] = 0x0A | 0x30; h[5] = 0x03;  // H4 = 0x13<<4 | 0xA = 314, H5 = 3<<4 | 3 = 51
  h[6] = 0xFE;                        // H6 = -2
  Bme280Calib c = bme280ParseCalib(a, h);
  EQ(27504, c.T1); EQ(26435, c.T2); EQ(-1000, c.T3); EQ(-10685, c.P2);
  EQ(75, c.H1); EQ(368, c.H2); EQ(0, c.H3); EQ(314, c.H4); EQ(51, c.H5); EQ(-2, c.H6);
  uint8_t d[8] = {0x65, 0x5A, 0xC0, 0x7E, 0xED, 0x00, 0x7A, 0xBC};
  int32_t p, t, hh;
  bme280ParseRaw(d, p, t, hh);
  EQ(415148, p);   // 0x655AC
  EQ(519888, t);   // 0x7EED0
  EQ(0x7ABC, hh);
}

void test_bme280_humidity_properties() {
  Bme280Calib c = datasheetCalib();
  c.H1 = 75; c.H2 = 368; c.H3 = 0; c.H4 = 314; c.H5 = 51; c.H6 = 30;
  int32_t tf = 128422;
  float prev = -1;
  for (int32_t raw = 25000; raw <= 35000; raw += 2500) {
    float h = bme280Hum(c, raw, tf);
    TEST_ASSERT_TRUE(h >= 0 && h <= 100);
    TEST_ASSERT_TRUE(h > prev);  // more counts, more humidity
    prev = h;
  }
  TEST_ASSERT_EQUAL_FLOAT(0.0f, bme280Hum(c, 0, tf));       // clamps at the bottom
  TEST_ASSERT_EQUAL_FLOAT(100.0f, bme280Hum(c, 65535, tf));  // and the top
}

void test_bme280_press_guard() {
  Bme280Calib c = datasheetCalib();
  c.P1 = 0;  // would divide by zero
  TEST_ASSERT_EQUAL_FLOAT(0.0f, bme280Press(c, 415148, 128422));
}

void test_ds18b20() {
  // Maxim datasheet scratchpad: 85.0 C power-on value, CRC 0x1C
  uint8_t pad[9] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x1C};
  EQ(0x1C, dallasCrc8(pad, 8));
  float t = 0;
  TEST_ASSERT_FALSE(ds18b20Decode(pad, t));  // 85.0 is the unconverted value, never shown
  uint8_t ok[9] = {0x91, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0};  // 0x0191 = 25.0625
  ok[8] = dallasCrc8(ok, 8);
  TEST_ASSERT_TRUE(ds18b20Decode(ok, t));
  TEST_ASSERT_EQUAL_FLOAT(25.0625f, t);
  uint8_t neg[9] = {0x5E, 0xFF, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0};  // 0xFF5E = -10.125
  neg[8] = dallasCrc8(neg, 8);
  TEST_ASSERT_TRUE(ds18b20Decode(neg, t));
  TEST_ASSERT_EQUAL_FLOAT(-10.125f, t);
  ok[8] ^= 1;                                  // corrupt CRC
  TEST_ASSERT_FALSE(ds18b20Decode(ok, t));
  uint8_t flip[9] = {0x91, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0};
  flip[8] = dallasCrc8(flip, 8);
  flip[1] = 0x02;                              // data changed, CRC not
  TEST_ASSERT_FALSE(ds18b20Decode(flip, t));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_motor_valid_and_pins);
  RUN_TEST(test_motor_pins_collide_with_others);
  RUN_TEST(test_motor_ranges);
  RUN_TEST(test_motor_drive);
  RUN_TEST(test_servo_direct);
  RUN_TEST(test_servo_pca9685);
  RUN_TEST(test_i2c_shared_bus_and_addresses);
  RUN_TEST(test_i2c_pins_free);
  RUN_TEST(test_servo_math);
  RUN_TEST(test_sensor_validation);
  RUN_TEST(test_serial_validation);
  RUN_TEST(test_pwm_slots_and_frequency);
  RUN_TEST(test_input_long_press_range);
  RUN_TEST(test_motor_servo_commands);
  RUN_TEST(test_serial_and_sensor_commands);
  RUN_TEST(test_names_and_actions);
  RUN_TEST(test_i2c_names_and_defaults);
  RUN_TEST(test_new_error_codes);
  RUN_TEST(test_bme280_datasheet_example);
  RUN_TEST(test_bme280_parse);
  RUN_TEST(test_bme280_humidity_properties);
  RUN_TEST(test_bme280_press_guard);
  RUN_TEST(test_ds18b20);
  return UNITY_END();
}
