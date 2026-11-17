#include "AutomationEngine.h"

#include <Arduino.h>
#include <new>

#include "AutomationStore.h"
#include "hal/Hal.h"

namespace espwf {

static bool lookup(void* ctx, const char* id, DeviceConfig& out) {
  return static_cast<DeviceRegistry*>(ctx)->config(id, out);
}

// What the rules see of the outside world.
struct AutomationEngine::Env : AutoEnv {
  DeviceRegistry& dev;
  NetworkManager& net;
  int16_t offsetMin;
  Env(DeviceRegistry& d, NetworkManager& n, int16_t off) : dev(d), net(n), offsetMin(off) {}
  bool read(const char* id, Prop p, double& v) override { return dev.readProp(id, p, v); }
  CmdErr command(const char* id, const Command& c) override { return dev.requestCommand(id, c); }
  bool deviceType(const char* id, DeviceType& t) override {
    DeviceConfig c;
    if (!dev.config(id, c)) return false;
    t = c.type;
    return true;
  }
  bool clock(ClockInfo& ci) override { return localClock(hal::epochUtc(), offsetMin, ci); }
  bool wifiUp() override { return net.staConnected(); }
  void event(const char* rule, EvKind k, const char* text) override {
    if (k != EvKind::Fired) Serial.printf("[auto] %s %s %s\n", rule, evKindName(k), text);
  }
};

void AutomationEngine::begin(DeviceRegistry& devices, NetworkManager& net) {
  devices_ = &devices;
  net_ = &net;
  size_t n = AutomationStore::load(core_);
  hal::clockBegin();
  Serial.printf("[auto] %u rule(s) loaded, engine %s\n", unsigned(n), core_.settings.enabled ? "on" : "off");
}

// ---- request side (any task) ----------------------------------------------------------------------

AutoErr AutomationEngine::queue(Pending* p) {
  hal::CriticalSection cs;
  if (pend_) { delete p; return AutoErr::Busy; }  // one change at a time: the caller retries
  pend_ = p;
  return AutoErr::Ok;
}

AutoErr AutomationEngine::requestAdd(const Rule& r) {
  AutoErr e = validateRule(r, lookup, devices_);
  if (e != AutoErr::Ok) return e;
  {
    hal::CriticalSection cs;
    if (core_.find(r.id) >= 0) return AutoErr::Duplicate;
    if (core_.count() >= kMaxRules) return AutoErr::TooMany;
  }
  Pending* p = new (std::nothrow) Pending;
  if (!p) return AutoErr::TooMany;
  p->kind = Pending::Add;
  p->rule = r;
  return queue(p);
}

AutoErr AutomationEngine::requestReplace(const Rule& r) {
  AutoErr e = validateRule(r, lookup, devices_);
  if (e != AutoErr::Ok) return e;
  {
    hal::CriticalSection cs;
    if (core_.find(r.id) < 0) return AutoErr::NotFound;
  }
  Pending* p = new (std::nothrow) Pending;
  if (!p) return AutoErr::TooMany;
  p->kind = Pending::Replace;
  p->rule = r;
  return queue(p);
}

AutoErr AutomationEngine::requestRemove(const char* id) {
  {
    hal::CriticalSection cs;
    if (core_.find(id) < 0) return AutoErr::NotFound;
  }
  Pending* p = new (std::nothrow) Pending;
  if (!p) return AutoErr::TooMany;
  p->kind = Pending::Remove;
  copyStr(p->id, id, sizeof p->id);
  return queue(p);
}

AutoErr AutomationEngine::requestEnable(const char* id, bool on) {
  {
    hal::CriticalSection cs;
    if (core_.find(id) < 0) return AutoErr::NotFound;
  }
  Pending* p = new (std::nothrow) Pending;
  if (!p) return AutoErr::TooMany;
  p->kind = Pending::Enable;
  copyStr(p->id, id, sizeof p->id);
  p->flag = on;
  return queue(p);
}

AutoErr AutomationEngine::requestRun(const char* id) {
  {
    hal::CriticalSection cs;
    if (core_.find(id) < 0) return AutoErr::NotFound;
  }
  Pending* p = new (std::nothrow) Pending;
  if (!p) return AutoErr::TooMany;
  p->kind = Pending::Run;
  copyStr(p->id, id, sizeof p->id);
  return queue(p);
}

AutoErr AutomationEngine::requestSettings(const AutoSettings& s) {
  AutoErr e = validateSettings(s, lookup, devices_);
  if (e != AutoErr::Ok) return e;
  Pending* p = new (std::nothrow) Pending;
  if (!p) return AutoErr::TooMany;
  p->kind = Pending::Settings;
  p->settings = s;
  return queue(p);
}

size_t AutomationEngine::count() const {
  hal::CriticalSection cs;
  return core_.count();
}

bool AutomationEngine::ruleAt(size_t i, Rule& out) const {
  hal::CriticalSection cs;
  if (i >= core_.count()) return false;
  out = core_.rule(i);
  return true;
}

AutoSettings AutomationEngine::settings() const {
  hal::CriticalSection cs;
  return core_.settings;
}

bool AutomationEngine::clockSynced() const {
  ClockInfo ci;
  return localClock(hal::epochUtc(), settings().utcOffsetMin, ci);
}

// ---- loop side ------------------------------------------------------------------------------------

void AutomationEngine::apply(const Pending& p) {
  bool changed = false;
  switch (p.kind) {
    case Pending::Add: {
      hal::CriticalSection cs;
      changed = core_.add(p.rule) == AutoErr::Ok;
      break;
    }
    case Pending::Replace: {
      hal::CriticalSection cs;
      changed = core_.replace(p.rule) == AutoErr::Ok;
      break;
    }
    case Pending::Remove: {
      hal::CriticalSection cs;
      changed = core_.remove(p.id);
      break;
    }
    case Pending::Enable: {
      hal::CriticalSection cs;
      changed = core_.setEnabled(p.id, p.flag);
      break;
    }
    case Pending::Run: core_.requestRun(p.id); break;
    case Pending::Settings: {
      hal::CriticalSection cs;
      core_.settings = p.settings;
      changed = true;
      break;
    }
  }
  if (changed) {
    bool saved = AutomationStore::save(core_.settings, core_);
    Serial.printf("[auto] change applied (saved=%d)\n", saved);
  }
}

void AutomationEngine::loop() {
  if (halted_) return;
  Pending* p = nullptr;
  {
    hal::CriticalSection cs;
    p = pend_;
    pend_ = nullptr;
  }
  if (p) { apply(*p); delete p; }
  if (!devices_) return;
  Env env(*devices_, *net_, core_.settings.utcOffsetMin);
  core_.tick(env, millis());
}

// ---- status ---------------------------------------------------------------------------------------

void AutomationEngine::describeStatusHead(JsonObject out) const {
  AutoSettings s = settings();
  out["enabled"] = s.enabled;
  JsonObject clk = out["clock"].to<JsonObject>();
  ClockInfo ci;
  bool ok = localClock(hal::epochUtc(), s.utcOffsetMin, ci);
  clk["synced"] = ok;
  clk["utc_offset_min"] = s.utcOffsetMin;
  if (ok) {
    char b[6];
    snprintf(b, sizeof b, "%02u:%02u", unsigned(ci.minuteOfDay / 60), unsigned(ci.minuteOfDay % 60));
    clk["local"] = b;
    clk["weekday"] = ci.weekday;
  }
  out["rules"] = count();
  out["max_rules"] = kMaxRules;
  out["firings_this_second"] = core_.firingsThisSecond();
}

bool AutomationEngine::describeRule(size_t i, JsonObject out) const {
  Rule r;
  RuleState s;
  {
    hal::CriticalSection cs;  // copy only; the JSON is built outside
    if (i >= core_.count()) return false;
    r = core_.rule(i);
    s = core_.state(i);
  }
  out["id"] = r.id;
  out["name"] = r.name;
  out["enabled"] = r.enabled;
  out["trigger"] = trigName(r.trig.type);
  out["fired"] = s.fired;
  out["failed"] = s.failed;
  out["unconfirmed"] = s.unconfirmed;
  out["last"] = lastName(s.last);
  if (s.everFired) out["fired_ago_s"] = (millis() - s.lastFire) / 1000;
  return true;
}

size_t AutomationEngine::eventCount() const {
  hal::CriticalSection cs;
  return core_.eventCount();
}

bool AutomationEngine::describeEvent(size_t i, JsonObject out) const {
  EventRec e;
  {
    hal::CriticalSection cs;
    if (i >= core_.eventCount()) return false;
    e = core_.eventAt(i);
  }
  out["ago_s"] = (millis() - e.ms) / 1000;
  out["rule"] = e.rule;
  out["kind"] = evKindName(e.kind);
  if (e.text[0]) out["text"] = e.text;
  return true;
}

}  // namespace espwf
