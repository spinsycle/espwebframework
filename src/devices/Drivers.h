#pragma once
// Bus-level drivers used by the device registry: I2C (scan, PCA9685, BME280) and DS18B20 on 1-Wire.
// Everything here blocks for at most a few milliseconds and is called only from loop().
#include <stddef.h>
#include <stdint.h>

#include "DeviceTypes.h"

namespace espwf {
namespace drv {

// ---- I2C (pins fixed per chip, see i2cSda / i2cScl)
void i2cStart(Chip chip);  // idempotent
bool i2cProbe(uint8_t addr);

// PCA9685: 50 Hz servo board. Pulse widths are converted with the nominal 25 MHz clock; real chips run a
// few percent fast, which the servo's min_us / max_us settings absorb.
bool pcaInit(uint8_t addr);
void pcaSetUs(uint8_t addr, uint8_t channel, uint16_t us);
void pcaOff(uint8_t addr, uint8_t channel);  // no pulses: the servo goes limp

// BME280 (or BMP280, which has no humidity). One measurement at a time: trigger, wait ~20 ms, read.
bool bmeBegin(uint8_t addr, bool& hasHum);
bool bmeTrigger(uint8_t addr);
bool bmeRead(uint8_t addr, bool hasHum, float& tC, float& hPct, float& pHpa);

// ---- DS18B20 (one per pin, no ROM search). The handle owns a OneWire object.
void* dsBegin(int pin);
void dsEnd(void* h);
bool dsConvert(void* h);              // start a conversion; false if nothing answers on the line
bool dsRead(void* h, float& tC);      // read the result (wait 800 ms after dsConvert); false on a bad CRC

}  // namespace drv
}  // namespace espwf
