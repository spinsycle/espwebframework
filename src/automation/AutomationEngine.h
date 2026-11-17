#pragma once
// Firmware side of the automation engine: loads and saves the rules, connects AutomationCore to the real
// devices, clock and Wi-Fi, and exposes request*() calls for the HTTP API.
// Same rule as the device registry: request*() only validate and queue (safe from the async server's task);
// begin() and loop() do all the work and are the only writers.
#include <ArduinoJson.h>

#include "AutomationCore.h"
#include "devices/DeviceRegistry.h"
#include "net/NetworkManager.h"

namespace espwf {

class AutomationEngine {
 public:
  void begin(DeviceRegistry& devices, NetworkManager& net);
  void loop();
  void halt() { halted_ = true; }  // no more saving or running (ahead of a restart)

  // Each validates against the live devices and queues one change; Busy if the previous one is still waiting.
  AutoErr requestAdd(const Rule& r);
  AutoErr requestReplace(const Rule& r);
  AutoErr requestRemove(const char* id);
  AutoErr requestEnable(const char* id, bool on);
  AutoErr requestRun(const char* id);
  AutoErr requestSettings(const AutoSettings& s);

  size_t count() const;
  bool ruleAt(size_t i, Rule& out) const;
  AutoSettings settings() const;
  bool clockSynced() const;

  void describeStatusHead(JsonObject out) const;  // clock, switches, totals
  bool describeRule(size_t i, JsonObject out) const;  // one rule's status
  size_t eventCount() const;
  bool describeEvent(size_t i, JsonObject out) const;  // newest first

 private:
  struct Pending {
    enum Kind : uint8_t { Add, Replace, Remove, Enable, Run, Settings } kind = Add;
    Rule rule;
    char id[kIdCap] = {0};
    bool flag = false;
    AutoSettings settings;
  };
  struct Env;

  AutoErr queue(Pending* p);
  void apply(const Pending& p);

  AutomationCore core_;
  DeviceRegistry* devices_ = nullptr;
  NetworkManager* net_ = nullptr;
  Pending* pend_ = nullptr;
  bool halted_ = false;
};

}  // namespace espwf
