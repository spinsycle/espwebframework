#pragma once
// What the read-only GPIO inspector says about each pin. Pure C++, unit-tested natively.
#include "DeviceTypes.h"

namespace espwf {

// The NodeMCU's silkscreen names, so the page can say "D1" next to GPIO 5. The ESP32 DevKit is labelled by GPIO already.
inline const char* pinLabel(Chip chip, int gpio) {
  if (chip != Chip::Esp8266) return "";
  switch (gpio) {
    case 16: return "D0"; case 5: return "D1"; case 4: return "D2"; case 0: return "D3"; case 2: return "D4";
    case 14: return "D5"; case 12: return "D6"; case 13: return "D7"; case 15: return "D8";
    case 3: return "RX"; case 1: return "TX"; case 9: return "SD2"; case 10: return "SD3";
    default: return "";
  }
}

// Why a pin needs care, or "" if it does not. Reserved pins are listed but never read.
inline const char* pinNote(Chip chip, int gpio) {
  if (checkPin(chip, gpio, PinRole::Input) == PinErr::Reserved) return (gpio == 1 || gpio == 3) ? "uart" : "flash";
  if (pinBootSensitive(chip, gpio)) return "strapping";
  if (checkPin(chip, gpio, PinRole::Output) == PinErr::InputOnly) return "input_only";
  if (checkPin(chip, gpio, PinRole::Pwm) == PinErr::NoPwm) return "no_pwm";
  return "";
}

inline bool pinListed(Chip chip, int gpio) { return checkPin(chip, gpio, PinRole::Input) != PinErr::Invalid; }
inline bool pinReadable(Chip chip, int gpio) { return checkPin(chip, gpio, PinRole::Input) == PinErr::Ok; }

}  // namespace espwf
