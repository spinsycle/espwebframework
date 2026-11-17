#pragma once
// Device config, pin policy, command checks, debounce. Pure C++ (no Arduino) so it is unit-tested natively.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace espwf {

// ---- pin policy -----------------------------------------------------------------------------

enum class Chip : uint8_t { Esp8266, Esp32 };
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
  if (gpio > 39 || gpio == 20 || gpio == 24 || (gpio >= 28 && gpio <= 31)) return PinErr::Invalid;
  if ((gpio >= 6 && gpio <= 11) || gpio == 1 || gpio == 3) return PinErr::Reserved;
  if (gpio >= 34 && role != PinRole::Input) return PinErr::InputOnly;
  return PinErr::Ok;
}

inline bool pinHasPullup(Chip chip, int gpio) { return chip == Chip::Esp8266 ? gpio != 16 : gpio < 34; }

// Strapping pins: usable, but what is wired to them can stop the board from booting.
inline bool pinBootSensitive(Chip chip, int gpio) {
  if (chip == Chip::Esp8266) return gpio == 0 || gpio == 2 || gpio == 15;
  return gpio == 0 || gpio == 2 || gpio == 5 || gpio == 12 || gpio == 15;
}

// ---- device config --------------------------------------------------------------------------

enum class DeviceType : uint8_t { Relay = 0, Input = 1, Pwm = 2 };

inline const char* deviceTypeName(DeviceType t) {
  switch (t) {
    case DeviceType::Relay: return "relay";
    case DeviceType::Input: return "input";
    default: return "pwm";
  }
}

inline bool parseDeviceType(const char* s, DeviceType& out) {
  if (!s) return false;
  if (!strcmp(s, "relay")) { out = DeviceType::Relay; return true; }
  if (!strcmp(s, "input")) { out = DeviceType::Input; return true; }
  if (!strcmp(s, "pwm")) { out = DeviceType::Pwm; return true; }
  return false;
}

constexpr unsigned kIdCap = 17;    // 16 chars + NUL
constexpr unsigned kNameCap = 25;  // 24 chars + NUL
constexpr size_t kMaxDevices = 32;
constexpr size_t kMaxPwm = 8;      // ESP8266 software PWM and the ESP32 LEDC low-speed group both fit 8

struct DeviceConfig {
  char id[kIdCap] = {0};
  char name[kNameCap] = {0};
  DeviceType type = DeviceType::Relay;
  int8_t pin = -1;
  bool invert = false;       // relay: active-low; input: pressed reads low; pwm: inverted duty
  bool pullup = false;       // input only
  uint16_t debounceMs = 30;  // input only
  uint16_t freqHz = 1000;    // pwm only
  uint8_t initial = 0;       // relay: 0 off / 1 on at boot; pwm: duty percent at boot
};

enum class DevErr : uint8_t {
  Ok = 0, Id, Name, Type, Pin, PinReserved, PinInputOnly, PinNoPwm, PinNoPullup, PinInUse,
  Debounce, Freq, Initial, Duplicate, Full, PwmLimit, NotFound, Format
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
    default: return "bad_format";
  }
}

inline const char* devErrField(DevErr e) {
  switch (e) {
    case DevErr::Id: case DevErr::Duplicate: return "id";
    case DevErr::Name: return "name";
    case DevErr::Type: case DevErr::PwmLimit: return "type";
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

// `others` = the devices already registered (never includes `c` itself).
inline DevErr validateDevice(const DeviceConfig& c, Chip chip, const DeviceConfig* others, size_t n) {
  using namespace detail;
  if (!devTerminated(c.id, kIdCap) || !validDeviceId(c.id)) return DevErr::Id;
  if (!devTerminated(c.name, kNameCap)) return DevErr::Name;
  if (uint8_t(c.type) > uint8_t(DeviceType::Pwm)) return DevErr::Type;
  if (n >= kMaxDevices) return DevErr::Full;

  size_t pwmCount = 0;
  for (size_t i = 0; i < n; i++) {
    if (!strcmp(others[i].id, c.id)) return DevErr::Duplicate;
    if (others[i].type == DeviceType::Pwm) pwmCount++;
  }
  if (c.type == DeviceType::Pwm && pwmCount >= kMaxPwm) return DevErr::PwmLimit;

  PinRole role = c.type == DeviceType::Input ? PinRole::Input
               : c.type == DeviceType::Pwm ? PinRole::Pwm : PinRole::Output;
  switch (checkPin(chip, c.pin, role)) {
    case PinErr::Ok: break;
    case PinErr::Reserved: return DevErr::PinReserved;
    case PinErr::InputOnly: return DevErr::PinInputOnly;
    case PinErr::NoPwm: return DevErr::PinNoPwm;
    default: return DevErr::Pin;
  }
  if (c.type == DeviceType::Input && c.pullup && !pinHasPullup(chip, c.pin)) return DevErr::PinNoPullup;
  for (size_t i = 0; i < n; i++)
    if (others[i].pin == c.pin) return DevErr::PinInUse;

  if (c.type == DeviceType::Input && c.debounceMs > 1000) return DevErr::Debounce;
  if (c.type == DeviceType::Pwm && (c.freqHz < 1 || c.freqHz > 20000)) return DevErr::Freq;
  if (c.type == DeviceType::Relay && c.initial > 1) return DevErr::Initial;
  if (c.type == DeviceType::Pwm && c.initial > 100) return DevErr::Initial;
  return DevErr::Ok;
}

// ---- commands -------------------------------------------------------------------------------

enum class CmdAction : uint8_t { None, Set, Toggle };
enum class CmdErr : uint8_t { Ok, NotFound, NotCommandable, BadAction, BadValue, QueueFull, BadId };

// One place for how a command error is named and which HTTP status it maps to (REST and WebSocket share it).
inline const char* cmdErrCode(CmdErr e) {
  switch (e) {
    case CmdErr::Ok: return "";
    case CmdErr::NotFound: return "not_found";
    case CmdErr::NotCommandable: return "not_commandable";
    case CmdErr::BadAction: return "bad_action";
    case CmdErr::BadValue: return "bad_value";
    case CmdErr::QueueFull: return "busy";
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
    case CmdErr::NotCommandable: return 409;
    case CmdErr::QueueFull: return 503;
    default: return 400;
  }
}

struct Command {
  CmdAction action = CmdAction::None;
  bool hasValue = false;
  bool isBool = false;
  double value = 0;  // relay: 0/1; pwm: percent
};

inline bool parseAction(const char* s, CmdAction& out) {
  if (!s) return false;
  if (!strcmp(s, "set")) { out = CmdAction::Set; return true; }
  if (!strcmp(s, "toggle")) { out = CmdAction::Toggle; return true; }
  return false;
}

inline CmdErr checkCommand(DeviceType t, const Command& c) {
  if (t == DeviceType::Input) return CmdErr::NotCommandable;
  if (c.action == CmdAction::None) return CmdErr::BadAction;
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
