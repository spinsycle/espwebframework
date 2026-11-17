#include "Ota.h"

#include <Arduino.h>
#if defined(ESP8266)
#include <Updater.h>
#else
#include <Update.h>
#endif

namespace espwf {
namespace Ota {

static bool gBusy = false;

bool busy() { return gBusy; }

bool begin(size_t expected, const char* md5) {
  if (gBusy) return false;
  if (md5 && *md5 && strlen(md5) == 32) Update.setMD5(md5);  // set after begin() on some cores: do both orders
#if defined(ESP8266)
  Update.runAsync(true);
  bool ok = Update.begin(expected);
#else
  bool ok = Update.begin(expected, U_FLASH);
#endif
  if (md5 && *md5 && strlen(md5) == 32) Update.setMD5(md5);
  if (!ok) return false;
  gBusy = true;
  Serial.printf("[ota] update started, up to %u bytes\n", unsigned(expected));
  return true;
}

bool write(const uint8_t* data, size_t len) {
  if (!gBusy) return false;
  // Update.write takes a non-const pointer on both cores
  return Update.write(const_cast<uint8_t*>(data), len) == len;
}

bool end() {
  if (!gBusy) return false;
  gBusy = false;
  bool ok = Update.end(true);  // true: the image may be shorter than the upload (multipart framing)
  Serial.printf("[ota] update %s\n", ok ? "written and verified" : "FAILED");
  return ok;
}

void abort() {
  if (!gBusy) return;
  gBusy = false;
  Update.end(false);
  Serial.println("[ota] update aborted");
}

int errorCode() { return int(Update.getError()); }

const char* errorText() {
  switch (Update.getError()) {
    case UPDATE_ERROR_OK: return "none";
    case UPDATE_ERROR_WRITE: return "flash write failed";
    case UPDATE_ERROR_ERASE: return "flash erase failed";
    case UPDATE_ERROR_READ: return "flash read failed";
    case UPDATE_ERROR_SPACE: return "not enough room for this image";
    case UPDATE_ERROR_SIZE: return "bad size";
    case UPDATE_ERROR_STREAM: return "stream error";
    case UPDATE_ERROR_MD5: return "checksum does not match";
    case UPDATE_ERROR_MAGIC_BYTE: return "this is not a firmware image";
    default: return "update failed";
  }
}

}  // namespace Ota
}  // namespace espwf
