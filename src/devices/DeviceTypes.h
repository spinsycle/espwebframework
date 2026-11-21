#pragma once
// Device config, pin policy, command checks, debounce. Pure C++ (no Arduino) so it is unit-tested natively.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace espwf {

// ---- pin policy -----------------------------------------------------------------------------

// Esp32S3 is appended: the classic ESP32 (WROOM-32 DevKit) keeps its value. S3 pin rules follow Espressif's
// ESP32-S3 datasheet and the common 8 MB flash / octal PSRAM modules (N8R8); a plain N8 module wastes GPIO 33-37.
enum class Chip : uint8_t { Esp8266, Esp32, Esp32S3 };
enum class PinRole : uint8_t { Output, Input, Pwm };
enum class PinErr : uint8_t { Ok, Invalid, Reserved, InputOnly, NoPwm };

// GPIO numbers, not D-labels. Reserved = flash, or UART0 (kept free for the serial log).
inline PinErr checkPin(Chip chip, int gpio, PinRole role) {
  if (gpio < 0) return PinErr::Invalid;
  if (chip == Chip::Esp8266) {
    if (gpio > 16) return PinErr::Invalid;
    if ((gpio >= 6 && gpio <= 11) || gpio == 1 || gpio == 3) return PinErr::Reserved;
    if (role == PinRole::Pwm && gpio == 16) return PinErr::NoPwm;  // GPIO16 has no PWM timer
    return PinErr::Ok;
  }
  if (chip == Chip::Esp32S3) {
    if (gpio > 48 || (gpio >= 22 && gpio <= 25)) return PinErr::Invalid;  // 22-25 do not exist
    // 26-37: SPI flash, and octal flash / PSRAM on the R8 modules. 19, 20: the native USB. 43, 44: UART0, the serial log.
    if ((gpio >= 26 && gpio <= 37) || gpio == 19 || gpio == 20 || gpio == 43 || gpio == 44) return PinErr::Reserved;
    if (gpio == 46 && role != PinRole::Input) return PinErr::InputOnly;
    return PinErr::Ok;
  }
  if (gpio > 39 || gpio == 20 || gpio == 24 || (gpio >= 28 && gpio <= 31)) return PinErr::Invalid;
  if ((gpio >= 6 && gpio <= 11) || gpio == 1 || gpio == 3) return PinErr::Reserved;
  if (gpio >= 34 && role != PinRole::Input) return PinErr::InputOnly;
  return PinErr::Ok;
}

inline bool pinHasPullup(Chip chip, int gpio) {
  if (chip == Chip::Esp8266) return gpio != 16;
  if (chip == Chip::Esp32S3) return gpio != 46;
  return gpio < 34;
}

// Strapping pins: usable, but what is wired to them can stop the board from booting.
inline bool pinBootSensitive(Chip chip, int gpio) {
  if (chip == Chip::Esp8266) return gpio == 0 || gpio == 2 || gpio == 15;
  if (chip == Chip::Esp32S3) return gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46;
  return gpio == 0 || gpio == 2 || gpio == 5 || gpio == 12 || gpio == 15;
}

// ---- device config --------------------------------------------------------------------------

// Append only: the numbers are never saved, but the names are, and tests depend on the order.
enum class DeviceType : uint8_t { Relay = 0, Input = 1, Pwm = 2, Motor = 3, Servo = 4, Sensor = 5, Serial = 6 };

inline const char* deviceTypeName(DeviceType t) {
  switch (t) {
    case DeviceType::Relay: return "relay";
    case DeviceType::Input: return "input";
    case DeviceType::Motor: return "motor";
    case DeviceType::Servo: return "servo";
    case DeviceType::Sensor: return "sensor";
    case DeviceType::Serial: return "serial";
    default: return "pwm";
  }
}

inline bool parseDeviceType(const char* s, DeviceType& out) {
  if (!s) return false;
  if (!strcmp(s, "relay")) { out = DeviceType::Relay; return true; }
  if (!strcmp(s, "input")) { out = DeviceType::Input; return true; }
  if (!strcmp(s, "pwm")) { out = DeviceType::Pwm; return true; }
  if (!strcmp(s, "motor")) { out = DeviceType::Motor; return true; }
  if (!strcmp(s, "servo")) { out = DeviceType::Servo; return true; }
  if (!strcmp(s, "sensor")) { out = DeviceType::Sensor; return true; }
  if (!strcmp(s, "serial")) { out = DeviceType::Serial; return true; }
  return false;
}

// Servo: 0 = pin driven directly, 1 = channel on a PCA9685. Sensor: 0 = DS18B20 (1-Wire), 1 = BME280 / BMP280.
constexpr uint8_t kModelCount = 2;
inline const char* modelName(DeviceType t, uint8_t m) {
  if (t == DeviceType::Servo) return m ? "pca9685" : "direct";
  return m ? "bme280" : "ds18b20";
}
inline bool parseModel(DeviceType t, const char* s, uint8_t& out) {
  if (!s) return false;
  for (uint8_t m = 0; m < kModelCount; m++)
    if (!strcmp(s, modelName(t, m))) { out = m; return true; }
  return false;
}

constexpr unsigned kIdCap = 17;    // 16 chars + NUL
constexpr unsigned kNameCap = 25;  // 24 chars + NUL
// Memory is the limit on the ESP8266: each device costs about 170 bytes of RAM for as long as the board runs.
#if defined(ESP8266)
constexpr size_t kMaxDevices = 16;
#else
constexpr size_t kMaxDevices = 32;
#endif
constexpr size_t kMaxPwm = 8;      // counts pwm, motor and direct-pin servo devices
constexpr unsigned kTextCap = 48;  // serial text: 47 chars + NUL      // ESP8266 software PWM and the ESP32 LEDC low-speed group both fit 8

struct DeviceConfig {
  char id[kIdCap] = {0};
  char name[kNameCap] = {0};
  DeviceType type = DeviceType::Relay;
  int8_t pin = -1;
  bool invert = false;       // relay: active-low; input: pressed reads low; pwm: inverted duty
  bool pullup = false;       // input only
  uint16_t debounceMs = 30;  // input only
  uint16_t freqHz = 1000;    // pwm only
  uint8_t initial = 0;       // relay: 0 off / 1 on at boot; pwm: duty percent at boot; servo: angle at boot
  int8_t pin2 = -1;          // motor: direction pin (IN1); serial: TX pin (pin is RX)
  int8_t pin3 = -1;          // motor: second direction pin (IN2), -1 = single direction pin
  uint8_t model = 0;         // servo / sensor, see modelName()
  uint8_t i2cAddr = 0;       // PCA9685 servo or BME280 sensor
  uint8_t channel = 0;       // PCA9685 channel 0..15
  uint16_t minUs = 500;      // servo pulse at 0 degrees (calibration knob)
  uint16_t maxUs = 2400;     // servo pulse at 180 degrees (calibration knob)
  uint16_t intervalS = 5;    // sensor: seconds between readings
  uint16_t longMs = 0;       // input: hold time that counts as a long press, 0 = off
  uint32_t baud = 9600;      // serial
  bool enabled = true;       // a disabled device sits in its safe state, refuses commands and reads nothing
  uint16_t failsafeS = 0;    // relay/pwm/motor/servo: seconds without a command or heartbeat before the safe state, 0 = off
  int16_t safeAngle = -1;    // servo: angle to move to on failsafe or disable, -1 = hold where it is
};

constexpr uint16_t kMaxFailsafeS = 3600;

enum class DevErr : uint8_t {
  Ok = 0, Id, Name, Type, Pin, PinReserved, PinInputOnly, PinNoPwm, PinNoPullup, PinInUse,
  Debounce, Freq, Initial, Duplicate, Full, PwmLimit, NotFound, Format,
  Model, I2cAddr, Channel, Pulse, Interval, Baud, LongMs, FreqConflict, SerialLimit, AddrInUse,
  Failsafe, SafeAngle
};

inline const char* devErrCode(DevErr e) {
  switch (e) {
    case DevErr::Ok: return "";
    case DevErr::Id: return "bad_id";
    case DevErr::Name: return "bad_name";
    case DevErr::Type: return "bad_type";
    case DevErr::Pin: return "bad_pin";
    case DevErr::PinReserved: return "pin_reserved";
    case DevErr::PinInputOnly: return "pin_input_only";
    case DevErr::PinNoPwm: return "pin_no_pwm";
    case DevErr::PinNoPullup: return "pin_no_pullup";
    case DevErr::PinInUse: return "pin_in_use";
    case DevErr::Debounce: return "bad_debounce";
    case DevErr::Freq: return "bad_freq";
    case DevErr::Initial: return "bad_initial";
    case DevErr::Duplicate: return "duplicate_id";
    case DevErr::Full: return "too_many_devices";
    case DevErr::PwmLimit: return "too_many_pwm";
    case DevErr::NotFound: return "not_found";
    case DevErr::Model: return "bad_model";
    case DevErr::I2cAddr: return "bad_i2c_addr";
    case DevErr::Channel: return "bad_channel";
    case DevErr::Pulse: return "bad_pulse";
    case DevErr::Interval: return "bad_interval";
    case DevErr::Baud: return "bad_baud";
    case DevErr::LongMs: return "bad_long_ms";
    case DevErr::FreqConflict: return "freq_conflict";
    case DevErr::SerialLimit: return "too_many_serial";
    case DevErr::AddrInUse: return "address_in_use";
    case DevErr::Failsafe: return "bad_failsafe";
    case DevErr::SafeAngle: return "bad_safe_angle";
    default: return "bad_format";
  }
}

inline const char* devErrField(DevErr e) {
  switch (e) {
    case DevErr::Id: case DevErr::Duplicate: return "id";
    case DevErr::Name: return "name";
    case DevErr::Type: case DevErr::PwmLimit: case DevErr::SerialLimit: return "type";
    case DevErr::Model: return "model";
    case DevErr::I2cAddr: case DevErr::AddrInUse: return "i2c_addr";
    case DevErr::Channel: return "channel";
    case DevErr::Pulse: return "min_us";
    case DevErr::Interval: return "interval_s";
    case DevErr::Failsafe: return "failsafe_s";
    case DevErr::SafeAngle: return "safe_angle";
    case DevErr::Baud: return "baud";
    case DevErr::LongMs: return "long_ms";
    case DevErr::FreqConflict: return "freq_hz";
    case DevErr::Pin: case DevErr::PinReserved: case DevErr::PinInputOnly:
    case DevErr::PinNoPwm: case DevErr::PinNoPullup: case DevErr::PinInUse: return "pin";
    case DevErr::Debounce: return "debounce_ms";
    case DevErr::Freq: return "freq_hz";
    case DevErr::Initial: return "initial";
    default: return "";
  }
}

namespace detail {
inline bool devTerminated(const char* s, unsigned cap) { return memchr(s, 0, cap) != nullptr; }
inline bool validDeviceId(const char* s) {
  size_t n = strlen(s);
  if (n < 1 || n > kIdCap - 1) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}
}  // namespace detail

// I2C bus pins are fixed per chip (the default Wire pins of the NodeMCU and the ESP32 DevKit).
inline int i2cSda(Chip chip) { return chip == Chip::Esp8266 ? 4 : chip == Chip::Esp32S3 ? 8 : 21; }
inline int i2cScl(Chip chip) { return chip == Chip::Esp8266 ? 5 : chip == Chip::Esp32S3 ? 9 : 22; }

inline bool isI2cDevice(const DeviceConfig& c) {
  return (c.type == DeviceType::Servo || c.type == DeviceType::Sensor) && c.model == 1;
}
inline uint8_t defaultI2cAddr(const DeviceConfig& c) { return c.type == DeviceType::Servo ? 0x40 : 0x76; }
inline bool usesPwmSlot(const DeviceConfig& c) {
  return c.type == DeviceType::Pwm || c.type == DeviceType::Motor || (c.type == DeviceType::Servo && c.model == 0);
}
inline uint16_t pwmFreq(const DeviceConfig& c) { return c.type == DeviceType::Servo ? 50 : c.freqHz; }

// ---- failsafe -------------------------------------------------------------------------------

// Only devices that drive something have a safe state. Inputs, sensors and the serial port have nothing to switch off.
inline bool failsafeApplies(DeviceType t) {
  return t == DeviceType::Relay || t == DeviceType::Pwm || t == DeviceType::Motor || t == DeviceType::Servo;
}

// No command and no heartbeat for `timeoutS` seconds. 0 = never. `now` and `lastAlive` are millis(), so they may wrap.
inline bool failsafeDue(uint32_t now, uint32_t lastAlive, uint16_t timeoutS) {
  return timeoutS != 0 && uint32_t(now - lastAlive) >= uint32_t(timeoutS) * 1000u;
}

// The later of two millis() stamps, wrap-safe.
inline uint32_t newerStamp(uint32_t a, uint32_t b) { return int32_t(a - b) >= 0 ? a : b; }

// Where a device goes when it fails safe or is disabled. act=false: leave it exactly as it is (a servo with no safe angle).
struct SafeTarget {
  bool act = false;
  bool on = false;     // relay
  uint8_t duty = 0;    // pwm percent
  int16_t val = 0;     // motor speed / servo angle
};
inline SafeTarget safeTarget(const DeviceConfig& c) {
  SafeTarget t;
  switch (c.type) {
    case DeviceType::Relay: case DeviceType::Pwm: case DeviceType::Motor: t.act = true; break;  // off, 0 %, stopped
    case DeviceType::Servo:
      if (c.safeAngle >= 0) { t.act = true; t.val = c.safeAngle; }
      break;
    default: break;
  }
  return t;
}

// The GPIOs a device occupies (the I2C bus pins are added for I2C devices). Returns how many were written (max 5).
inline size_t pinsOf(const DeviceConfig& c, Chip chip, int8_t* out) {
  size_t n = 0;
  switch (c.type) {
    case DeviceType::Motor:
      out[n++] = c.pin; out[n++] = c.pin2;
      if (c.pin3 >= 0) out[n++] = c.pin3;
      break;
    case DeviceType::Serial: out[n++] = c.pin; out[n++] = c.pin2; break;
    case DeviceType::Servo: if (c.model == 0) out[n++] = c.pin; break;
    case DeviceType::Sensor: if (c.model == 0) out[n++] = c.pin; break;
    default: out[n++] = c.pin; break;
  }
  if (isI2cDevice(c)) { out[n++] = int8_t(i2cSda(chip)); out[n++] = int8_t(i2cScl(chip)); }
  return n;
}

// May the I2C bus be (re)started, i.e. no registered device sits on its two pins?
inline bool i2cPinsFree(Chip chip, const DeviceConfig* devs, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (isI2cDevice(devs[i])) continue;  // those share the bus on purpose
    int8_t p[5];
    size_t k = pinsOf(devs[i], chip, p);
    for (size_t j = 0; j < k; j++)
      if (p[j] == i2cSda(chip) || p[j] == i2cScl(chip)) return false;
  }
  return true;
}

// A guess at what answered a bus scan, from its address alone.
inline const char* i2cKnownName(uint8_t a) {
  if (a == 0x76 || a == 0x77) return "BME280 / BMP280";
  if (a == 0x70) return "PCA9685 all-call";
  if (a >= 0x40 && a <= 0x7F) return "PCA9685 or similar";
  if (a == 0x3C || a == 0x3D) return "OLED display";
  return "";
}

inline bool baudOk(Chip chip, uint32_t b) {
  static const uint32_t ok[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
  for (uint32_t v : ok)
    if (v == b) return chip != Chip::Esp8266 || b <= 38400;  // ESP8266 uses software serial
  return false;
}

// `others` = the devices already registered. When editing a device in place, `skip` is its index in `others`
// and it is ignored, so a device never conflicts with its old self.
inline DevErr validateDevice(const DeviceConfig& c, Chip chip, const DeviceConfig* others, size_t n, int skip = -1) {
  using namespace detail;
  if (!devTerminated(c.id, kIdCap) || !validDeviceId(c.id)) return DevErr::Id;
  if (!devTerminated(c.name, kNameCap)) return DevErr::Name;
  if (uint8_t(c.type) > uint8_t(DeviceType::Serial)) return DevErr::Type;
  if (c.model >= kModelCount) return DevErr::Model;
  if (n - (skip >= 0 ? 1 : 0) >= kMaxDevices) return DevErr::Full;

  size_t pwmCount = 0, serialCount = 0;
  for (size_t i = 0; i < n; i++) {
    if (int(i) == skip) continue;
    if (!strcmp(others[i].id, c.id)) return DevErr::Duplicate;
    if (usesPwmSlot(others[i])) pwmCount++;
    if (others[i].type == DeviceType::Serial) serialCount++;
  }
  if (usesPwmSlot(c) && pwmCount >= kMaxPwm) return DevErr::PwmLimit;
  if (c.type == DeviceType::Serial && serialCount >= 1) return DevErr::SerialLimit;
  if (chip != Chip::Esp32 && usesPwmSlot(c))  // one PWM frequency for the whole chip (ESP8266; the S3 has 8 LEDC channels on 4 timers)
    for (size_t i = 0; i < n; i++)
      if (int(i) != skip && usesPwmSlot(others[i]) && pwmFreq(others[i]) != pwmFreq(c)) return DevErr::FreqConflict;

  // Each GPIO the device uses, with the role it needs there. The I2C bus pins are checked for conflicts only.
  struct PinUse { int8_t pin; PinRole role; };
  PinUse uses[3];
  size_t nu = 0;
  switch (c.type) {
    case DeviceType::Input: uses[nu++] = {c.pin, PinRole::Input}; break;
    case DeviceType::Pwm: uses[nu++] = {c.pin, PinRole::Pwm}; break;
    case DeviceType::Motor:
      uses[nu++] = {c.pin, PinRole::Pwm};
      uses[nu++] = {c.pin2, PinRole::Output};
      if (c.pin3 >= 0) uses[nu++] = {c.pin3, PinRole::Output};
      break;
    case DeviceType::Serial:
      uses[nu++] = {c.pin, PinRole::Input};
      uses[nu++] = {c.pin2, PinRole::Output};
      break;
    case DeviceType::Servo: if (c.model == 0) uses[nu++] = {c.pin, PinRole::Pwm}; break;
    case DeviceType::Sensor: if (c.model == 0) uses[nu++] = {c.pin, PinRole::Output}; break;  // 1-Wire drives the line
    default: uses[nu++] = {c.pin, PinRole::Output}; break;
  }
  for (size_t u = 0; u < nu; u++) {
    switch (checkPin(chip, uses[u].pin, uses[u].role)) {
      case PinErr::Ok: break;
      case PinErr::Reserved: return DevErr::PinReserved;
      case PinErr::InputOnly: return DevErr::PinInputOnly;
      case PinErr::NoPwm: return DevErr::PinNoPwm;
      default: return DevErr::Pin;
    }
    for (size_t v = 0; v < u; v++)
      if (uses[v].pin == uses[u].pin) return DevErr::PinInUse;  // the same pin twice in one device
  }
  if (c.type == DeviceType::Input && c.pullup && !pinHasPullup(chip, c.pin)) return DevErr::PinNoPullup;
  {
    int8_t mine[5];
    size_t nm = pinsOf(c, chip, mine);
    for (size_t i = 0; i < n; i++) {
      if (int(i) == skip) continue;
      if (isI2cDevice(c) && isI2cDevice(others[i])) {  // two I2C devices share the bus, but not an address
        bool sameChannelBoard = c.type == DeviceType::Servo && others[i].type == DeviceType::Servo;
        if (c.i2cAddr == others[i].i2cAddr && !(sameChannelBoard && c.channel != others[i].channel))
          return DevErr::AddrInUse;
        continue;
      }
      int8_t theirs[5];
      size_t nt = pinsOf(others[i], chip, theirs);
      for (size_t a = 0; a < nm; a++)
        for (size_t b = 0; b < nt; b++)
          if (mine[a] == theirs[b]) return DevErr::PinInUse;
    }
  }

  if (c.type == DeviceType::Input && c.debounceMs > 1000) return DevErr::Debounce;
  if (c.type == DeviceType::Pwm && (c.freqHz < 1 || c.freqHz > 20000)) return DevErr::Freq;
  if (c.type == DeviceType::Relay && c.initial > 1) return DevErr::Initial;
  if (c.type == DeviceType::Pwm && c.initial > 100) return DevErr::Initial;
  if (c.type == DeviceType::Input && c.longMs != 0 && (c.longMs < 100 || c.longMs > 10000)) return DevErr::LongMs;
  if (c.type == DeviceType::Motor) {
    if (c.freqHz < 1 || c.freqHz > 20000) return DevErr::Freq;
    if (c.initial != 0) return DevErr::Initial;  // a motor always boots stopped
  }
  if (c.type == DeviceType::Servo) {
    if (c.initial > 180) return DevErr::Initial;
    if (c.minUs < 300 || c.maxUs > 3000 || c.minUs >= c.maxUs) return DevErr::Pulse;
    if (c.model == 1) {
      if (c.i2cAddr < 0x40 || c.i2cAddr > 0x7F || c.i2cAddr == 0x70) return DevErr::I2cAddr;
      if (c.channel > 15) return DevErr::Channel;
    }
  }
  if (c.type == DeviceType::Sensor) {
    if (c.intervalS < 1 || c.intervalS > 3600) return DevErr::Interval;
    if (c.model == 1 && c.i2cAddr != 0x76 && c.i2cAddr != 0x77) return DevErr::I2cAddr;
  }
  if (c.type == DeviceType::Serial && !baudOk(chip, c.baud)) return DevErr::Baud;
  if (c.failsafeS > kMaxFailsafeS || (c.failsafeS != 0 && !failsafeApplies(c.type))) return DevErr::Failsafe;
  if (c.safeAngle != -1 && (c.type != DeviceType::Servo || c.safeAngle < 0 || c.safeAngle > 180)) return DevErr::SafeAngle;
  return DevErr::Ok;
}

// ---- commands -------------------------------------------------------------------------------

enum class CmdAction : uint8_t { None, Set, Toggle, Stop, Send };
enum class CmdErr : uint8_t { Ok, NotFound, NotCommandable, BadAction, BadValue, QueueFull, BadId, Disabled };

// One place for how a command error is named and which HTTP status it maps to (REST and WebSocket share it).
inline const char* cmdErrCode(CmdErr e) {
  switch (e) {
    case CmdErr::Ok: return "";
    case CmdErr::NotFound: return "not_found";
    case CmdErr::NotCommandable: return "not_commandable";
    case CmdErr::BadAction: return "bad_action";
    case CmdErr::BadValue: return "bad_value";
    case CmdErr::QueueFull: return "busy";
    case CmdErr::Disabled: return "disabled";
    default: return "bad_id";
  }
}
inline const char* cmdErrField(CmdErr e) {
  switch (e) {
    case CmdErr::BadAction: return "action";
    case CmdErr::BadValue: return "value";
    case CmdErr::Ok: return "";
    default: return "id";
  }
}
inline int cmdErrHttp(CmdErr e) {
  switch (e) {
    case CmdErr::Ok: return 202;
    case CmdErr::NotFound: return 404;
    case CmdErr::NotCommandable: case CmdErr::Disabled: return 409;
    case CmdErr::QueueFull: return 503;
    default: return 400;
  }
}

struct Command {
  CmdAction action = CmdAction::None;
  bool hasValue = false;
  bool isBool = false;
  double value = 0;  // relay: 0/1; pwm: percent; motor: -100..100; servo: angle
  bool isText = false;
  char text[kTextCap] = {0};  // serial: the line to send
};

inline bool parseAction(const char* s, CmdAction& out) {
  if (!s) return false;
  if (!strcmp(s, "set")) { out = CmdAction::Set; return true; }
  if (!strcmp(s, "toggle")) { out = CmdAction::Toggle; return true; }
  if (!strcmp(s, "stop")) { out = CmdAction::Stop; return true; }
  if (!strcmp(s, "send")) { out = CmdAction::Send; return true; }
  return false;
}

inline CmdErr checkCommand(DeviceType t, const Command& c) {
  if (t == DeviceType::Input || t == DeviceType::Sensor) return CmdErr::NotCommandable;
  if (c.action == CmdAction::None) return CmdErr::BadAction;
  if (t == DeviceType::Serial) {
    if (c.action != CmdAction::Send) return CmdErr::BadAction;
    size_t len = c.isText ? strnlen(c.text, kTextCap) : 0;
    return (len >= 1 && len < kTextCap) ? CmdErr::Ok : CmdErr::BadValue;
  }
  if (t == DeviceType::Motor || t == DeviceType::Servo) {
    if (c.action == CmdAction::Stop) return t == DeviceType::Motor ? CmdErr::Ok : CmdErr::BadAction;
    if (c.action != CmdAction::Set) return CmdErr::BadAction;
    if (!c.hasValue || c.isBool) return CmdErr::BadValue;
    double lo = t == DeviceType::Motor ? -100 : 0, hi = t == DeviceType::Motor ? 100 : 180;
    return (c.value >= lo && c.value <= hi) ? CmdErr::Ok : CmdErr::BadValue;
  }
  if (c.action == CmdAction::Stop || c.action == CmdAction::Send) return CmdErr::BadAction;
  if (t == DeviceType::Relay) {
    if (c.action == CmdAction::Toggle) return CmdErr::Ok;
    return (c.hasValue && c.isBool) ? CmdErr::Ok : CmdErr::BadValue;
  }
  // Pwm
  if (c.action != CmdAction::Set) return CmdErr::BadAction;
  if (!c.hasValue || c.isBool) return CmdErr::BadValue;
  return (c.value >= 0 && c.value <= 100) ? CmdErr::Ok : CmdErr::BadValue;  // NaN fails both tests
}

// ---- pin level mapping ----------------------------------------------------------------------

inline bool relayPinLevel(bool on, bool invert) { return on != invert; }
inline bool inputActive(bool pinLevel, bool invert) { return pinLevel != invert; }
inline uint16_t pwmPermille(uint8_t percent, bool invert) {
  uint8_t p = percent > 100 ? 100 : percent;
  return uint16_t((invert ? 100 - p : p) * 10);
}

// Motor: signed speed percent to pin levels. One direction pin (pin3 < 0): dir = reverse. Two (IN1/IN2): one high.
struct MotorDrive { bool dir1; bool dir2; uint8_t duty; };
inline MotorDrive motorDrive(int speed, bool invert, bool twoPins) {
  if (speed > 100) speed = 100;
  if (speed < -100) speed = -100;
  bool rev = (speed < 0) != invert;
  MotorDrive d;
  d.duty = uint8_t(speed < 0 ? -speed : speed);
  if (twoPins) { d.dir1 = d.duty && !rev; d.dir2 = d.duty && rev; }
  else { d.dir1 = rev; d.dir2 = false; }
  return d;
}

// Servo: angle (0..180) to a pulse width in microseconds, between the calibrated min and max.
inline uint16_t servoPulseUs(int angle, uint16_t minUs, uint16_t maxUs) {
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  return uint16_t(minUs + (uint32_t(maxUs - minUs) * uint32_t(angle) + 90) / 180);
}
// 50 Hz frame: 1/10000 duty units (2 microseconds each), and PCA9685 ticks (4096 per 20 ms).
inline uint16_t servoDuty10k(uint16_t us) { return uint16_t((uint32_t(us) * 50 + 50) / 100); }
inline uint16_t pcaTicks(uint16_t us) { return uint16_t((uint32_t(us) * 4096 + 10000) / 20000); }

// ---- debounce -------------------------------------------------------------------------------

// A level must hold for `ms` before it becomes the stable value. The first sample is taken as the
// starting state, so booting with a button held produces no phantom edge.
struct Debouncer {
  bool stable = false;
  bool raw = false;
  bool primed = false;
  uint32_t since = 0;

  bool update(bool level, uint32_t now, uint16_t ms) {  // true when `stable` just changed
    if (!primed) { primed = true; raw = stable = level; since = now; return false; }
    if (level != raw) { raw = level; since = now; }
    if (raw != stable && uint32_t(now - since) >= ms) { stable = raw; return true; }
    return false;
  }
};

}  // namespace espwf
