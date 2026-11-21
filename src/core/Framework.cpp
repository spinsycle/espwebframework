#include "Framework.h"

#include "core/EventLog.h"
#include "core/HealthRules.h"
#include "core/Version.h"
#include "hal/Hal.h"
#include <LittleFS.h>

#include "config/ConfigRules.h"
#include "net/MqttStore.h"
#include "net/NetworkStore.h"
#if ESPWF_HAS_ROLE
#include "net/RoleStore.h"
#endif

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
  {
    char why[40];
    hal::resetReason(why, sizeof why);
    logEvent(isFaultReason(why) ? "boot_fault" : "boot", why);
  }

  NetworkConfig cfg;
  setNetworkDefaults(cfg, hal::chipId());
  if (NetworkStore::load(cfg)) {
    Serial.println("[espwf] network config loaded");
  } else {
    Serial.println("[espwf] no valid network config, using defaults");
  }
  net_.begin(cfg);
  MqttConfig mcfg;
  setMqttDefaults(mcfg, cfg.hostname);
  if (MqttStore::load(mcfg)) Serial.println("[espwf] mqtt config loaded");
  mqtt_.begin(mcfg, devices_, net_);
#if ESPWF_HAS_ROLE
  RoleConfig rcfg;
  if (RoleStore::load(rcfg)) Serial.println("[espwf] role config loaded");
  role_.begin(rcfg, devices_, net_);
#endif
  automation_.begin(devices_, net_);
  auth_.begin();
  #if ESPWF_HAS_ROLE
  http_.begin(*this, net_, devices_, ws_, automation_, auth_, mqtt_, &role_);
#else
  http_.begin(*this, net_, devices_, ws_, automation_, auth_, mqtt_, nullptr);
#endif
  Serial.printf("[espwf] ready, AP '%s'\n", cfg.apSsid);
  return true;
}

void FrameworkController::requestFactoryReset() { resetPending_ = true; }

void FrameworkController::loop() {
  uint32_t t = millis();
  if (lastLoop_ && uint32_t(t - lastLoop_) > loopMaxMs_) loopMaxMs_ = uint32_t(t - lastLoop_);  // the first pass would count setup()
  lastLoop_ = t;
  if (int32_t(t - nextHeapSample_) >= 0) {
    nextHeapSample_ = t + 500;
    uint32_t h = hal::freeHeap();
    if (h < minHeap_) minHeap_ = h;
#ifdef ESPWF_HEAP_LOG  // build with -DESPWF_HEAP_LOG to see the heap on the serial console every 5 s
    static uint32_t lastHeapLog = 0;
    if (t - lastHeapLog >= 5000) {
      lastHeapLog = t;
      Serial.printf("[heap] %lu free %u max_block %u min %u\n", (unsigned long)t, (unsigned)h, (unsigned)hal::maxFreeBlock(), (unsigned)minHeap_);
    }
#endif
  }
#if defined(ESP8266)
  if (wdtTest_) {  // never returns: the software watchdog resets the board after about 3 seconds
    logEvent("watchdog_test", "hanging the loop on purpose");
    Serial.println("[espwf] watchdog test: hanging the loop");
    Serial.flush();
    while (true) { }
  }
#endif
  if (resetPending_) {
    resetPending_ = false;
    devices_.halt();  // nothing may write a config file after this point
    automation_.halt();
    {
      hal::FsWrite g;
      for (const char* f : espwf::kConfigFiles) LittleFS.remove(f);
    }
    Serial.println("[espwf] factory reset: saved config deleted, restarting");
    logEvent("factory_reset", "saved config deleted");
    restart();
  }
  devices_.loop();
  automation_.loop();
  if (devices_.restartRequested() && !restartPending_) restart();
  ws_.loop();
  net_.loop();
  mqtt_.loop();
#if ESPWF_HAS_ROLE
  role_.loop();
#endif
  if (restartPending_ && int32_t(millis() - restartAt_) >= 0) hal::restart();
}

void FrameworkController::restart() {
  logEvent("restart", "requested");
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

void FrameworkController::getHealth(JsonObject out) const {
  char reason[40];
  hal::resetReason(reason, sizeof reason);
  out["watchdog_reset"] = isWatchdogReason(reason);
  out["fault_reset"] = isFaultReason(reason);
  out["loop_max_ms"] = loopMaxMs_;
  out["heap"]["min_free"] = minHeap_ == 0xFFFFFFFFu ? hal::freeHeap() : minHeap_;
  out["events_total"] = eventTotal();
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
  mqtt_.status(out["mqtt"].to<JsonObject>());
#if ESPWF_HAS_ROLE
  role_.status(out["role"].to<JsonObject>());
#endif
}
