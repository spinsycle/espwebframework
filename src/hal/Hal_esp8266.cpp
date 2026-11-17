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

}  // namespace hal
}  // namespace espwf
#endif  // ESP8266
