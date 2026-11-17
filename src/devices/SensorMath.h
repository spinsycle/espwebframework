#pragma once
// Pure sensor math (no Arduino): BME280 / BMP280 compensation from the Bosch datasheet (integer
// algorithms, 64-bit pressure), and DS18B20 CRC + scratchpad decoding. Unit-tested natively.
#include <stddef.h>
#include <stdint.h>

namespace espwf {

struct Bme280Calib {
  uint16_t T1; int16_t T2, T3;
  uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
  uint8_t H1; int16_t H2; uint8_t H3; int16_t H4, H5; int8_t H6;
};

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

// a = registers 0x88..0xA1 (26 bytes), h = registers 0xE1..0xE7 (7 bytes; ignored on a BMP280).
inline Bme280Calib bme280ParseCalib(const uint8_t* a, const uint8_t* h) {
  Bme280Calib c;
  c.T1 = le16(a + 0); c.T2 = int16_t(le16(a + 2)); c.T3 = int16_t(le16(a + 4));
  c.P1 = le16(a + 6); c.P2 = int16_t(le16(a + 8)); c.P3 = int16_t(le16(a + 10));
  c.P4 = int16_t(le16(a + 12)); c.P5 = int16_t(le16(a + 14)); c.P6 = int16_t(le16(a + 16));
  c.P7 = int16_t(le16(a + 18)); c.P8 = int16_t(le16(a + 20)); c.P9 = int16_t(le16(a + 22));
  c.H1 = a[25];
  c.H2 = int16_t(le16(h + 0));
  c.H3 = h[2];
  c.H4 = int16_t((int8_t(h[3]) * 16) | (h[4] & 0x0F));
  c.H5 = int16_t((int8_t(h[5]) * 16) | (h[4] >> 4));
  c.H6 = int8_t(h[6]);
  return c;
}

// d = registers 0xF7..0xFE (8 bytes): pressure, temperature (20 bit each), humidity (16 bit).
inline void bme280ParseRaw(const uint8_t* d, int32_t& adcP, int32_t& adcT, int32_t& adcH) {
  adcP = (int32_t(d[0]) << 12) | (int32_t(d[1]) << 4) | (d[2] >> 4);
  adcT = (int32_t(d[3]) << 12) | (int32_t(d[4]) << 4) | (d[5] >> 4);
  adcH = (int32_t(d[6]) << 8) | d[7];
}

// Degrees C. Also returns t_fine, which the pressure and humidity formulas need.
inline float bme280Temp(const Bme280Calib& c, int32_t adcT, int32_t& tFine) {
  int32_t v1 = (((adcT >> 3) - (int32_t(c.T1) << 1)) * int32_t(c.T2)) >> 11;
  int32_t v2 = (((((adcT >> 4) - int32_t(c.T1)) * ((adcT >> 4) - int32_t(c.T1))) >> 12) * int32_t(c.T3)) >> 14;
  tFine = v1 + v2;
  return float((tFine * 5 + 128) >> 8) / 100.0f;
}

// hPa. 0 when the calibration would divide by zero.
inline float bme280Press(const Bme280Calib& c, int32_t adcP, int32_t tFine) {
  int64_t v1 = int64_t(tFine) - 128000;
  int64_t v2 = v1 * v1 * int64_t(c.P6);
  v2 += (v1 * int64_t(c.P5)) << 17;
  v2 += int64_t(c.P4) << 35;
  v1 = ((v1 * v1 * int64_t(c.P3)) >> 8) + ((v1 * int64_t(c.P2)) << 12);
  v1 = (((int64_t(1) << 47) + v1) * int64_t(c.P1)) >> 33;
  if (v1 == 0) return 0;
  int64_t p = 1048576 - adcP;
  p = (((p << 31) - v2) * 3125) / v1;
  v1 = (int64_t(c.P9) * (p >> 13) * (p >> 13)) >> 25;
  v2 = (int64_t(c.P8) * p) >> 19;
  p = ((p + v1 + v2) >> 8) + (int64_t(c.P7) << 4);
  return float(uint32_t(p)) / 256.0f / 100.0f;  // Q24.8 pascal -> hPa
}

// Percent relative humidity, clamped to 0..100.
inline float bme280Hum(const Bme280Calib& c, int32_t adcH, int32_t tFine) {
  int32_t x = tFine - 76800;
  x = (((((adcH << 14) - (int32_t(c.H4) << 20) - (int32_t(c.H5) * x)) + 16384) >> 15) *
       (((((((x * int32_t(c.H6)) >> 10) * (((x * int32_t(c.H3)) >> 11) + 32768)) >> 10) + 2097152) *
         int32_t(c.H2) + 8192) >> 14));
  x = x - (((((x >> 15) * (x >> 15)) >> 7) * int32_t(c.H1)) >> 4);
  if (x < 0) x = 0;
  if (x > 419430400) x = 419430400;
  return float(uint32_t(x) >> 12) / 1024.0f;
}

// Dallas/Maxim CRC-8 (polynomial x^8 + x^5 + x^4 + 1, reflected).
inline uint8_t dallasCrc8(const uint8_t* d, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t b = d[i];
    for (int k = 0; k < 8; k++) {
      uint8_t mix = (crc ^ b) & 1;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      b >>= 1;
    }
  }
  return crc;
}

// 9-byte DS18B20 scratchpad to degrees C. False on a bad CRC or the power-on value 85.0 (a conversion
// that never ran), so a half-finished read never shows as a temperature.
inline bool ds18b20Decode(const uint8_t* s, float& tempC) {
  if (dallasCrc8(s, 8) != s[8]) return false;
  int16_t raw = int16_t(le16(s));
  if (raw == 0x0550) return false;
  tempC = float(raw) / 16.0f;
  return true;
}

}  // namespace espwf
