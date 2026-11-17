#include "Drivers.h"

#include <Arduino.h>
#include <OneWire.h>
#include <Wire.h>

#include "SensorMath.h"

namespace espwf {
namespace drv {

// ---- I2C ------------------------------------------------------------------------------------

static bool gI2cStarted = false;

void i2cStart(Chip chip) {
  if (gI2cStarted) return;
  Wire.begin(i2cSda(chip), i2cScl(chip));
  Wire.setClock(100000);
  gI2cStarted = true;
}

bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool wr(uint8_t addr, uint8_t reg, const uint8_t* d, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (n) Wire.write(d, n);
  return Wire.endTransmission() == 0;
}
static bool wr1(uint8_t addr, uint8_t reg, uint8_t v) { return wr(addr, reg, &v, 1); }

static bool rd(uint8_t addr, uint8_t reg, uint8_t* d, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(addr, uint8_t(n)) != n) return false;
  for (size_t i = 0; i < n; i++) d[i] = uint8_t(Wire.read());
  return true;
}

// ---- PCA9685 --------------------------------------------------------------------------------

static uint64_t gPcaInit = 0;  // bit (addr - 0x40): board initialised since boot

bool pcaInit(uint8_t addr) {
  uint64_t bit = uint64_t(1) << (addr - 0x40);
  if (gPcaInit & bit) return true;
  bool ok = wr1(addr, 0x00, 0x10)      // MODE1: sleep, so the prescaler can be written
         && wr1(addr, 0xFE, 121)       // PRESCALE: 25 MHz / (4096 * 50 Hz) - 1
         && wr1(addr, 0x01, 0x04)      // MODE2: totem-pole outputs
         && wr1(addr, 0x00, 0x00);     // wake
  if (!ok) return false;
  delay(5);                            // oscillator start-up
  ok = wr1(addr, 0x00, 0xA0);          // MODE1: restart, auto-increment
  if (ok) gPcaInit |= bit;
  return ok;
}

void pcaSetUs(uint8_t addr, uint8_t channel, uint16_t us) {
  uint16_t t = pcaTicks(us);
  uint8_t d[4] = {0, 0, uint8_t(t & 0xFF), uint8_t(t >> 8)};
  wr(addr, uint8_t(0x06 + 4 * channel), d, 4);
}

void pcaOff(uint8_t addr, uint8_t channel) {
  uint8_t d[4] = {0, 0, 0, 0x10};  // OFF_H bit 4 = full off
  wr(addr, uint8_t(0x06 + 4 * channel), d, 4);
}

// ---- BME280 ---------------------------------------------------------------------------------

static Bme280Calib gCal[2];  // indexed by address: 0x76 -> 0, 0x77 -> 1

bool bmeBegin(uint8_t addr, bool& hasHum) {
  uint8_t id = 0;
  if (!rd(addr, 0xD0, &id, 1) || (id != 0x60 && id != 0x58)) return false;
  hasHum = id == 0x60;
  uint8_t a[26], h[7] = {0};
  if (!rd(addr, 0x88, a, 26)) return false;
  if (hasHum && !rd(addr, 0xE1, h, 7)) return false;
  gCal[addr & 1] = bme280ParseCalib(a, h);
  return wr1(addr, 0xF5, 0x00);  // config: no filter, no standby (forced mode ignores it)
}

bool bmeTrigger(uint8_t addr) {
  return wr1(addr, 0xF2, 0x01)    // humidity x1 (needs ctrl_meas written after it)
      && wr1(addr, 0xF4, 0x25);   // temperature x1, pressure x1, forced mode: one measurement, then sleep
}

bool bmeRead(uint8_t addr, bool hasHum, float& tC, float& hPct, float& pHpa) {
  uint8_t d[8];
  if (!rd(addr, 0xF7, d, 8)) return false;
  int32_t adcP, adcT, adcH;
  bme280ParseRaw(d, adcP, adcT, adcH);
  if (adcT == 0x80000) return false;  // measurement had not finished: the register still holds its reset value
  const Bme280Calib& c = gCal[addr & 1];
  int32_t tFine;
  tC = bme280Temp(c, adcT, tFine);
  pHpa = adcP == 0x80000 ? 0 : bme280Press(c, adcP, tFine);
  hPct = hasHum ? bme280Hum(c, adcH, tFine) : 0;
  return true;
}

// ---- DS18B20 --------------------------------------------------------------------------------

void* dsBegin(int pin) { return new OneWire(uint8_t(pin)); }
void dsEnd(void* h) { delete static_cast<OneWire*>(h); }

bool dsConvert(void* h) {
  OneWire* ow = static_cast<OneWire*>(h);
  if (!ow->reset()) return false;  // nothing pulled the line low: no sensor, or no pull-up resistor
  ow->skip();
  ow->write(0x44, 0);              // convert T, no parasite power
  return true;
}

bool dsRead(void* h, float& tC) {
  OneWire* ow = static_cast<OneWire*>(h);
  if (!ow->reset()) return false;
  ow->skip();
  ow->write(0xBE);                 // read scratchpad
  uint8_t s[9];
  for (int i = 0; i < 9; i++) s[i] = ow->read();
  return ds18b20Decode(s, tC);
}

}  // namespace drv
}  // namespace espwf
