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

  // Sequences and timers: same pattern. Controls (start / stop / pause / resume) are queued like any change.
  AutoErr requestSeqAdd(const SeqDef& q);
  AutoErr requestSeqReplace(const SeqDef& q);
  AutoErr requestSeqRemove(const char* id);
  AutoErr requestSeqCtl(AutomationCore::Ctl op, const char* id);
  AutoErr requestTimerAdd(const TimerDef& t);
  AutoErr requestTimerReplace(const TimerDef& t);
  AutoErr requestTimerRemove(const char* id);
  AutoErr requestTimerCtl(AutomationCore::Ctl op, const char* id);

  size_t count() const;
  bool ruleAt(size_t i, Rule& out) const;
  AutoSettings settings() const;
  bool clockSynced() const;
  bool hasSeq(const char* id) const;
  bool hasTimer(const char* id) const;
  size_t seqCount() const;
  bool seqAt(size_t i, SeqDef& out) const;  // out is ~1.5 KB: keep it on the heap
  size_t timerCount() const;
  bool describeSeq(size_t i, JsonObject out) const;
  bool describeTimer(size_t i, JsonObject out) const;

  void describeStatusHead(JsonObject out) const;  // clock, switches, totals
  bool describeRule(size_t i, JsonObject out) const;  // one rule's status
  size_t eventCount() const;
  bool describeEvent(size_t i, JsonObject out) const;  // newest first

 private:
  struct Pending {
    enum Kind : uint8_t { Add, Replace, Remove, Enable, Run, Settings, SeqAdd, SeqReplace, SeqRemove, SeqCtl, TimerAdd, TimerReplace, TimerRemove, TimerCtl } kind = Add;
    Rule rule;
    char id[kIdCap] = {0};
    bool flag = false;
    AutoSettings settings;
    SeqDef* seq = nullptr;  // only sequence changes carry one (it is large)
    TimerDef timer;
    AutomationCore::Ctl ctl = AutomationCore::Ctl::SeqStart;
    ~Pending() { delete seq; }
  };
  struct Env;

  AutoRefs refs() const;
  AutoErr queue(Pending* p);
  void apply(const Pending& p);

  AutomationCore core_;
  DeviceRegistry* devices_ = nullptr;
  NetworkManager* net_ = nullptr;
  Pending* pend_ = nullptr;
  bool halted_ = false;
};

}  // namespace espwf
