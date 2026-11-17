#include "DeviceRegistry.h"

#include <Arduino.h>

#include "DeviceStore.h"
#include "hal/Hal.h"

namespace espwf {

#if defined(ESP8266)
static constexpr Chip kChip = Chip::Esp8266;
#else
static constexpr Chip kChip = Chip::Esp32;
#endif

// ---- request side (any task) ----------------------------------------------------------------

size_t DeviceRegistry::count() const {
  hal::CriticalSection cs;
  return n_;
}

bool DeviceRegistry::view(size_t i, DeviceView& out) const {
  hal::CriticalSection cs;
  if (i >= n_) return false;
  out.cfg = cfg_[i];
  out.on = rt_[i].on;
  out.duty = rt_[i].duty;
  out.edges = rt_[i].edges;
  return true;
}

int DeviceRegistry::find(const char* id) const {
  hal::CriticalSection cs;
  for (size_t i = 0; i < n_; i++)
    if (!strcmp(cfg_[i].id, id)) return int(i);
  return -1;
}

bool DeviceRegistry::push(const Op& op) {
  hal::CriticalSection cs;
  if (op.kind == Op::Cmd && op.cmd.action == CmdAction::Set) {
    // A slider drag sends many "set" values for one device: only the latest matters, so it takes the
    // place of the one already waiting instead of filling the queue.
    bool merged = queue_.replaceNewestIf(op, [&op](const Op& q) {
      return q.kind == Op::Cmd && q.cmd.action == CmdAction::Set && !strcmp(q.id, op.id);
    });
    if (merged) return true;
  }
  return queue_.push(op);
}

bool DeviceRegistry::pop(Op& op) {
  hal::CriticalSection cs;
  return queue_.pop(op);
}

DevErr DeviceRegistry::requestAdd(const DeviceConfig& c) {
  {
    hal::CriticalSection cs;
    DevErr e = validateDevice(c, kChip, cfg_, n_);
    if (e != DevErr::Ok) return e;
  }
  Op op;
  op.kind = Op::Add;
  op.cfg = c;
  return push(op) ? DevErr::Ok : DevErr::Full;
}

DevErr DeviceRegistry::requestRemove(const char* id) {
  if (find(id) < 0) return DevErr::NotFound;
  Op op;
  op.kind = Op::Remove;
  strlcpy(op.id, id, sizeof op.id);
  return push(op) ? DevErr::Ok : DevErr::Full;
}

CmdErr DeviceRegistry::requestCommand(const char* id, const Command& cmd) {
  DeviceType t;
  {
    hal::CriticalSection cs;
    int i = find(id);
    if (i < 0) return CmdErr::NotFound;
    t = cfg_[i].type;
  }
  CmdErr e = checkCommand(t, cmd);
  if (e != CmdErr::Ok) return e;
  Op op;
  op.kind = Op::Cmd;
  strlcpy(op.id, id, sizeof op.id);
  op.cmd = cmd;
  return push(op) ? CmdErr::Ok : CmdErr::QueueFull;
}

void DeviceRegistry::describeState(const DeviceView& v, JsonObject st) {
  switch (v.cfg.type) {
    case DeviceType::Relay: st["on"] = v.on; break;
    case DeviceType::Input: st["active"] = v.on; st["edges"] = v.edges; break;
    default: st["duty"] = v.duty; break;
  }
}

void DeviceRegistry::describe(const DeviceView& v, JsonObject out) {
  DeviceStore::toJson(v.cfg, out);
  describeState(v, out["state"].to<JsonObject>());
  if (pinBootSensitive(kChip, v.cfg.pin)) out["boot_sensitive"] = true;
}

// ---- loop side (only writer) ----------------------------------------------------------------

void DeviceRegistry::begin() {
  n_ = DeviceStore::load(cfg_, kMaxDevices, kChip);
  for (size_t i = 0; i < n_; i++) startHw(i);
  Serial.printf("[dev] %u device(s) loaded\n", unsigned(n_));
}

void DeviceRegistry::startHw(size_t i) {
  const DeviceConfig& c = cfg_[i];
  Runtime& r = rt_[i];
  r = Runtime();
  switch (c.type) {
    case DeviceType::Relay:
      r.on = c.initial != 0;
      digitalWrite(c.pin, relayPinLevel(r.on, c.invert));  // level first, so the pin never glitches
      pinMode(c.pin, OUTPUT);
      break;
    case DeviceType::Input:
      pinMode(c.pin, c.pullup ? INPUT_PULLUP : INPUT);
      r.on = inputActive(digitalRead(c.pin) != LOW, c.invert);
      r.deb.update(r.on, millis(), c.debounceMs);  // primes: no phantom edge at boot
      break;
    default: {
      uint8_t slot = 0;
      while (slot < 8 && (pwmSlots_ & (1u << slot))) slot++;  // validateDevice caps pwm devices at 8
      pwmSlots_ |= uint8_t(1u << slot);
      r.slot = slot;
      r.duty = c.initial;
      hal::pwmAttach(c.pin, c.freqHz, slot);
      hal::pwmWrite(c.pin, slot, pwmPermille(r.duty, c.invert));
      break;
    }
  }
}

void DeviceRegistry::stopHw(size_t i) {
  const DeviceConfig& c = cfg_[i];
  switch (c.type) {
    case DeviceType::Relay: digitalWrite(c.pin, relayPinLevel(false, c.invert)); break;  // leave it off
    case DeviceType::Pwm:
      hal::pwmDetach(c.pin, rt_[i].slot);
      pwmSlots_ &= uint8_t(~(1u << rt_[i].slot));
      break;
    default: break;
  }
}

void DeviceRegistry::driveRelay(size_t i) { digitalWrite(cfg_[i].pin, relayPinLevel(rt_[i].on, cfg_[i].invert)); }
void DeviceRegistry::drivePwm(size_t i) {
  hal::pwmWrite(cfg_[i].pin, rt_[i].slot, pwmPermille(rt_[i].duty, cfg_[i].invert));
}

void DeviceRegistry::doAdd(const DeviceConfig& c) {
  DevErr e = validateDevice(c, kChip, cfg_, n_);  // re-check: another add may have queued ahead of this
  if (e != DevErr::Ok) {
    Serial.printf("[dev] add '%s' dropped: %s\n", c.id, devErrCode(e));
    return;
  }
  cfg_[n_] = c;      // readers only look below n_, so fill the slot first, publish after
  startHw(n_);
  {
    hal::CriticalSection cs;
    n_++;
    structVer_ = structVer_ + 1;
  }
  bool saved = DeviceStore::save(cfg_, n_);
  Serial.printf("[dev] added %s '%s' on pin %d (saved=%d)\n", deviceTypeName(c.type), c.id, c.pin, saved);
}

void DeviceRegistry::doRemove(const char* id) {
  int idx = find(id);
  if (idx < 0) return;
  size_t i = size_t(idx);
  stopHw(i);
  {
    hal::CriticalSection cs;
    for (size_t k = i + 1; k < n_; k++) { cfg_[k - 1] = cfg_[k]; rt_[k - 1] = rt_[k]; }
    n_--;
    structVer_ = structVer_ + 1;
  }
  bool saved = DeviceStore::save(cfg_, n_);
  Serial.printf("[dev] removed '%s' (saved=%d)\n", id, saved);
}

void DeviceRegistry::doCommand(const char* id, const Command& cmd) {
  int idx = find(id);
  if (idx < 0) return;
  size_t i = size_t(idx);
  if (checkCommand(cfg_[i].type, cmd) != CmdErr::Ok) return;  // the device may have been replaced since queueing
  if (cfg_[i].type == DeviceType::Relay) {
    bool on = cmd.action == CmdAction::Toggle ? !rt_[i].on : cmd.value != 0;
    {
      hal::CriticalSection cs;
      rt_[i].on = on;
      stateVer_ = stateVer_ + 1;
    }
    driveRelay(i);
  } else {
    uint8_t duty = uint8_t(cmd.value + 0.5);
    {
      hal::CriticalSection cs;
      rt_[i].duty = duty;
      stateVer_ = stateVer_ + 1;
    }
    drivePwm(i);
  }
}

void DeviceRegistry::pollInputs() {
  uint32_t now = millis();
  for (size_t i = 0; i < n_; i++) {
    if (cfg_[i].type != DeviceType::Input) continue;
    bool active = inputActive(digitalRead(cfg_[i].pin) != LOW, cfg_[i].invert);
    if (rt_[i].deb.update(active, now, cfg_[i].debounceMs)) {
      {
        hal::CriticalSection cs;
        rt_[i].on = rt_[i].deb.stable;
        rt_[i].edges++;
        stateVer_ = stateVer_ + 1;
      }
      Serial.printf("[dev] '%s' %s\n", cfg_[i].id, rt_[i].on ? "active" : "inactive");
    }
  }
}

void DeviceRegistry::loop() {
  Op op;
  while (pop(op)) {
    if (op.kind == Op::Add) doAdd(op.cfg);
    else if (op.kind == Op::Remove) doRemove(op.id);
    else doCommand(op.id, op.cmd);
  }
  pollInputs();
}

}  // namespace espwf
