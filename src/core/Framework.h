#pragma once
// Public entry point. An app does:
//   FrameworkController Framework;
//   void setup() { Framework.setDeviceName("My Device"); Framework.begin(); }
//   void loop()  { Framework.loop(); }
#include <Arduino.h>
#include <ArduinoJson.h>

#include "net/HttpApi.h"
#include "net/NetworkManager.h"

class FrameworkController {
 public:
  void setDeviceName(const char* name);
  const char* deviceName() const { return deviceName_; }

  bool begin();   // call once from setup(); also starts Serial at 115200
  void loop();    // call every pass of loop(); never blocks

  void getSystemInfo(JsonObject out) const;  // static facts (/api/v1/system)
  void getStatus(JsonObject out) const;      // live numbers (/api/v1/status)
  void restart();                            // deferred ~400 ms so HTTP replies can flush
  // factoryReset() arrives with the config system (P5).

  espwf::NetworkManager& network() { return net_; }

 private:
  char deviceName_[33] = "ESP Device";
  espwf::NetworkManager net_;
  espwf::HttpApi http_;
  uint32_t restartAt_ = 0;
  bool restartPending_ = false;
};
