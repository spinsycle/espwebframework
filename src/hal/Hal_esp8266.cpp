#ifdef ESP8266
#include "Hal.h"

#include <Arduino.h>
#include <LittleFS.h>

namespace espwf {
namespace hal {

const char* chipName() { return "ESP8266"; }
uint32_t chipId() { return ESP.getChipId(); }
uint32_t freeHeap() { return ESP.getFreeHeap(); }
uint32_t maxFreeBlock() { return ESP.getMaxFreeBlockSize(); }
uint32_t flashSize() { return ESP.getFlashChipRealSize(); }

void resetReason(char* out, size_t n) {
  String s = ESP.getResetReason();
  strlcpy(out, s.c_str(), n);
}

void restart() { ESP.restart(); }

bool fsBegin() { return LittleFS.begin(); }  // auto-format is on by default

size_t fsTotal() {
  FSInfo i;
  return LittleFS.info(i) ? i.totalBytes : 0;
}

size_t fsUsed() {
  FSInfo i;
  return LittleFS.info(i) ? i.usedBytes : 0;
}

CriticalSection::CriticalSection() {}   // single-threaded: callbacks only run when loop() yields
CriticalSection::~CriticalSection() {}

void pwmAttach(int pin, uint32_t freqHz, uint8_t) {
  analogWriteRange(1000);  // so duty is permille
  analogWriteFreq(freqHz);
  pinMode(pin, OUTPUT);
}

void pwmWrite(int pin, uint8_t, uint16_t permille) { analogWrite(pin, permille); }

void pwmDetach(int pin, uint8_t) {
  analogWrite(pin, 0);
  digitalWrite(pin, LOW);
}

}  // namespace hal
}  // namespace espwf
#endif  // ESP8266
