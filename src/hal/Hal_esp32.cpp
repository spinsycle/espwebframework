#ifdef ESP32
#include "Hal.h"

#include <Arduino.h>
#include <time.h>
#include <LittleFS.h>
#include <esp_system.h>

namespace espwf {
namespace hal {

const char* chipName() { return "ESP32"; }

uint32_t chipId() {
  // Efuse MAC is little-endian in the 64-bit value: octet i = byte i.
  uint64_t m = ESP.getEfuseMac();
  return (uint32_t((m >> 24) & 0xFF) << 16) | (uint32_t((m >> 32) & 0xFF) << 8) | uint32_t((m >> 40) & 0xFF);
}

uint32_t freeHeap() { return ESP.getFreeHeap(); }
uint32_t maxFreeBlock() { return ESP.getMaxAllocHeap(); }
uint32_t flashSize() { return ESP.getFlashChipSize(); }

void resetReason(char* out, size_t n) {
  const char* s;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: s = "Power on"; break;
    case ESP_RST_SW: s = "Software restart"; break;
    case ESP_RST_PANIC: s = "Exception"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: s = "Watchdog"; break;
    case ESP_RST_BROWNOUT: s = "Brownout"; break;
    case ESP_RST_DEEPSLEEP: s = "Deep-sleep wake"; break;
    case ESP_RST_EXT: s = "External reset"; break;
    default: s = "Other"; break;
  }
  strlcpy(out, s, n);
}

void restart() { ESP.restart(); }

static volatile int gFsWriters = 0;
bool fsBusy() { return gFsWriters > 0; }
FsWrite::FsWrite() { gFsWriters = gFsWriters + 1; }
FsWrite::~FsWrite() { gFsWriters = gFsWriters - 1; }

bool fsBegin() { return LittleFS.begin(true); }  // true = format on first use
size_t fsTotal() { return LittleFS.totalBytes(); }
size_t fsUsed() { return LittleFS.usedBytes(); }

static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
CriticalSection::CriticalSection() { portENTER_CRITICAL(&gMux); }
CriticalSection::~CriticalSection() { portEXIT_CRITICAL(&gMux); }

// LEDC: a device in slot n uses channel 2n, so its timer (channel/2 % 4, per speed group) is its own and
// devices can have different frequencies. Resolution is the finest the frequency allows, up to 14 bits.
static uint8_t gBits[8];

static inline uint8_t ledcChannel(uint8_t slot) { return uint8_t(slot * 2); }

void pwmAttach(int pin, uint32_t freqHz, uint8_t slot) {
  uint8_t bits = 14;
  while (bits > 8 && (uint64_t(freqHz) << bits) > 80000000ULL) bits--;
  gBits[slot] = bits;
  ledcSetup(ledcChannel(slot), freqHz, bits);
  ledcAttachPin(pin, ledcChannel(slot));
}

void pwmWrite(int, uint8_t slot, uint16_t duty10k) {
  ledcWrite(ledcChannel(slot), uint32_t(duty10k) * ((1u << gBits[slot]) - 1) / 10000);
}

void pwmDetach(int pin, uint8_t slot) {
  ledcWrite(ledcChannel(slot), 0);
  ledcDetachPin(pin);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

bool uartBegin(int rxPin, int txPin, uint32_t baud) {
  Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
  return true;
}
void uartEnd() { Serial2.end(); }
int uartRead() { return Serial2.available() ? Serial2.read() : -1; }
void uartWrite(const char* s, size_t n) { Serial2.write(reinterpret_cast<const uint8_t*>(s), n); }

void clockBegin() { configTime(0, 0, "pool.ntp.org", "time.nist.gov"); }  // the offset is applied by the automation engine
uint32_t epochUtc() { return uint32_t(time(nullptr)); }

void randomBytes(uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i += 4) {
    uint32_t r = esp_random();
    for (size_t k = 0; k < 4 && i + k < n; k++) out[i + k] = uint8_t(r >> (8 * k));
  }
}

}  // namespace hal
}  // namespace espwf
#endif  // ESP32
