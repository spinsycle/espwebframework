#ifdef ESP32
#include "Hal.h"

#include <Arduino.h>
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

bool fsBegin() { return LittleFS.begin(true); }  // true = format on first use
size_t fsTotal() { return LittleFS.totalBytes(); }
size_t fsUsed() { return LittleFS.usedBytes(); }

static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
CriticalSection::CriticalSection() { portENTER_CRITICAL(&gMux); }
CriticalSection::~CriticalSection() { portEXIT_CRITICAL(&gMux); }

// 10-bit LEDC so duty maps from permille. arduino-esp32 2.x API (platform espressif32 6.9.0).
void pwmAttach(int pin, uint32_t freqHz, uint8_t slot) {
  ledcSetup(slot, freqHz, 10);
  ledcAttachPin(pin, slot);
}

void pwmWrite(int, uint8_t slot, uint16_t permille) { ledcWrite(slot, uint32_t(permille) * 1023 / 1000); }

void pwmDetach(int pin, uint8_t slot) {
  ledcWrite(slot, 0);
  ledcDetachPin(pin);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

}  // namespace hal
}  // namespace espwf
#endif  // ESP32
