#pragma once
// P0 placeholder. The real HAL (GPIO, PWM, WiFi, timers, UART, I2C, FS, pin maps)
// arrives in P1; chip-specific code lives in hal_esp8266/ and hal_esp32/ only.
namespace espwf {
class IHardwarePlatform {
 public:
  virtual const char* chipName() const = 0;
  virtual ~IHardwarePlatform() = default;
};
}  // namespace espwf
