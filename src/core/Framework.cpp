#include "Framework.h"

#include "core/Version.h"
#include "hal/Hal.h"
#include <LittleFS.h>

#include "config/ConfigRules.h"
#include "net/NetworkStore.h"

using namespace espwf;

void FrameworkController::setDeviceName(const char* name) {
  if (name && *name) snprintf(deviceName_, sizeof deviceName_, "%s", name);
}

bool FrameworkController::begin() {
  Serial.begin(115200);
  char v[16];
  versionString(v, sizeof v);
  Serial.printf("\n[espwf] %s framework %s\n", hal::chipName(), v);

  if (!hal::fsBegin()) Serial.println("[espwf] LittleFS mount failed");

  devices_.begin();  // before Wi-Fi: relays reach their boot state as early as possible

  NetworkConfig cfg;
  setNetworkDefaults(cfg, hal::chipId());
  if (NetworkStore::load(cfg)) {
    Serial.println("[espwf] network config loaded");
  } else {
    Serial.println("[espwf] no valid network config, using defaults");
  }
  net_.begin(cfg);
  http_.begin(*this, net_, devices_, ws_);
  Serial.printf("[espwf] ready, AP '%s'\n", cfg.apSsid);
  return true;
}

void FrameworkController::requestFactoryReset() { resetPending_ = true; }

void FrameworkController::loop() {
  if (resetPending_) {
    resetPending_ = false;
    devices_.halt();  // nothing may write a config file after this point
    for (const char* f : espwf::kConfigFiles) LittleFS.remove(f);
    Serial.println("[espwf] factory reset: saved config deleted, restarting");
    restart();
  }
  devices_.loop();
  if (devices_.restartRequested() && !restartPending_) restart();
  ws_.loop();
  net_.loop();
  if (restartPending_ && int32_t(millis() - restartAt_) >= 0) hal::restart();
}

void FrameworkController::restart() {
  restartAt_ = millis() + 400;
  restartPending_ = true;
}

void FrameworkController::getSystemInfo(JsonObject out) const {
  char v[16];
  char id[8];
  versionString(v, sizeof v);
  snprintf(id, sizeof id, "%06x", (unsigned)(hal::chipId() & 0xFFFFFF));
  JsonObject dev = out["device"].to<JsonObject>();
  dev["id"] = id;
  dev["name"] = deviceName_;
  out["framework_version"] = v;
  out["chip"] = hal::chipName();
  out["cpu_mhz"] = ESP.getCpuFreqMHz();
  out["flash_size"] = hal::flashSize();
  out["sdk"] = ESP.getSdkVersion();
  out["build"] = __DATE__ " " __TIME__;
}

void FrameworkController::getStatus(JsonObject out) const {
  char reason[40];
  hal::resetReason(reason, sizeof reason);
  uint32_t freeH = hal::freeHeap(), maxB = hal::maxFreeBlock();
  out["uptime_s"] = millis() / 1000;  // wraps at ~49 days, fine for P1
  out["reset_reason"] = reason;
  JsonObject heap = out["heap"].to<JsonObject>();
  heap["free"] = freeH;
  heap["max_block"] = maxB;
  heap["fragmentation_pct"] = freeH ? 100 - uint32_t(uint64_t(maxB) * 100 / freeH) : 0;
  JsonObject fs = out["filesystem"].to<JsonObject>();
  fs["used"] = hal::fsUsed();
  fs["total"] = hal::fsTotal();
  out["ws_clients"] = ws_.clientCount();
  net_.status(out["network"].to<JsonObject>());
}
