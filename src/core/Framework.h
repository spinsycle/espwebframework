#pragma once
// Public entry point. An app does:
//   FrameworkController Framework;
//   void setup() { Framework.setDeviceName("My Device"); Framework.begin(); }
//   void loop()  { Framework.loop(); }
#include <Arduino.h>
#include <ArduinoJson.h>

#include "automation/AutomationEngine.h"
#include "devices/DeviceRegistry.h"
#include "net/HttpApi.h"
#include "security/AuthManager.h"
#include "net/MqttManager.h"
#include "net/NetworkManager.h"
#include "core/Features.h"
#if ESPWF_HAS_ROLE
#include "net/RoleManager.h"
#endif
#include "net/WsHub.h"

class FrameworkController {
 public:
  void setDeviceName(const char* name);
  const char* deviceName() const { return deviceName_; }

  bool begin();   // call once from setup(); also starts Serial at 115200
  void loop();    // call every pass of loop(); never blocks

  void getSystemInfo(JsonObject out) const;  // static facts (/api/v1/system)
  void getStatus(JsonObject out) const;      // live numbers (/api/v1/status)
  void restart();                            // deferred ~400 ms so HTTP replies can flush
  void requestWatchdogTest() { wdtTest_ = true; }  // ESP8266 only: hangs the loop on purpose so the watchdog resets the board
  void getHealth(JsonObject out) const;      // what /api/v1/diagnostics adds to the status: loop gaps, heap low-water, reset kind
  void requestFactoryReset();                // deletes every saved config file, then restarts (queued; runs in loop())

  espwf::NetworkManager& network() { return net_; }
  espwf::DeviceRegistry& devices() { return devices_; }

 private:
  char deviceName_[33] = "ESP Device";
  espwf::DeviceRegistry devices_;
  espwf::AutomationEngine automation_;
  espwf::AuthManager auth_;
  espwf::NetworkManager net_;
  espwf::MqttManager mqtt_;
#if ESPWF_HAS_ROLE
  espwf::RoleManager role_;
#endif
  espwf::HttpApi http_;
  espwf::WsHub ws_;
  uint32_t restartAt_ = 0;
  bool restartPending_ = false;
  volatile bool resetPending_ = false;
  volatile bool wdtTest_ = false;
  uint32_t lastLoop_ = 0, loopMaxMs_ = 0, nextHeapSample_ = 0, minHeap_ = 0xFFFFFFFFu;
};
