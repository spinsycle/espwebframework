#include "DeviceRegistry.h"

#include <Arduino.h>
#include <math.h>
#include <new>

#include "DeviceStore.h"
#include "dashboard/DashboardRules.h"
#include "Drivers.h"
#include "hal/Hal.h"

namespace espwf {

#if defined(ESP8266)
static constexpr Chip kChip = Chip::Esp8266;
#else
static constexpr Chip kChip = Chip::Esp32;
#endif

static int roundInt(double v) { return int(v < 0 ? v - 0.5 : v + 0.5); }
static float round1(float v) { return roundf(v * 10.0f) / 10.0f; }

// ---- request side (any task) ----------------------------------------------------------------

size_t DeviceRegistry::count() const {
  hal::CriticalSection cs;
  return n_;
}

bool DeviceRegistry::view(size_t i, DeviceView& out) const {
  hal::CriticalSection cs;
  if (i >= n_) return false;
  const Runtime& r = rt_[i];
  out.cfg = cfg_[i];
  out.on = r.on;
  out.duty = r.duty;
  out.edges = r.edges;
  out.val = r.val;
  out.longs = r.longs;
  out.ok = r.ok;
  out.hasHum = r.hasHum;
  out.temp = r.temp;
  out.hum = r.hum;
  out.press = r.press;
  if (cfg_[i].type == DeviceType::Serial) {
    out.edges = serialLines_;
    memcpy(out.text, serialLast_, kTextCap);
  }
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

bool DeviceRegistry::config(const char* id, DeviceConfig& out) const {
  hal::CriticalSection cs;
  int i = find(id);
  if (i < 0) return false;
  out = cfg_[i];
  return true;
}

DevErr DeviceRegistry::requestUpdate(const DeviceConfig& c) {
  {
    hal::CriticalSection cs;
    int i = find(c.id);
    if (i < 0) return DevErr::NotFound;
    if (c.type != cfg_[i].type) return DevErr::Type;  // a different kind of device is a remove + add
    DevErr e = validateDevice(c, kChip, cfg_, n_, i);
    if (e != DevErr::Ok) return e;
  }
  Op op;
  op.kind = Op::Update;
  op.cfg = c;
  return push(op) ? DevErr::Ok : DevErr::Full;
}

ImportResult DeviceRegistry::requestImport(const DeviceConfig* list, size_t n) {
  ImportResult r;
  if (n > kMaxDevices) { r.err = DevErr::Full; r.index = kMaxDevices; return r; }
  for (size_t i = 0; i < n; i++) {  // each device is checked against the ones before it, as if added one by one
    DevErr e = validateDevice(list[i], kChip, list, i);
    if (e != DevErr::Ok) { r.err = e; r.index = i; return r; }
  }
  DeviceConfig* copy = new (std::nothrow) DeviceConfig[n ? n : 1];
  if (!copy) { r.err = DevErr::Full; return r; }
  for (size_t i = 0; i < n; i++) copy[i] = list[i];
  hal::CriticalSection cs;
  if (importList_) { delete[] copy; r.err = DevErr::Full; return r; }  // one import at a time
  importList_ = copy;
  importN_ = n;
  return r;
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

ScanErr DeviceRegistry::requestScan() {
  hal::CriticalSection cs;
  if (scanning_) return ScanErr::Busy;
  if (!i2cPinsFree(kChip, cfg_, n_)) return ScanErr::PinsInUse;  // a button on the bus pins would be disturbed
  scanReq_ = true;
  scanning_ = true;
  return ScanErr::Ok;
}

void DeviceRegistry::describeScan(JsonObject out) const {
  uint8_t found[16];
  uint8_t nf;
  bool scanning, done;
  {
    hal::CriticalSection cs;
    nf = nFound_;
    memcpy(found, found_, sizeof found);
    scanning = scanning_;
    done = scanDone_;
  }
  out["scanning"] = scanning;
  out["done"] = done;
  out["sda"] = i2cSda(kChip);
  out["scl"] = i2cScl(kChip);
  JsonArray arr = out["found"].to<JsonArray>();
  for (uint8_t i = 0; i < nf; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["addr"] = found[i];
    o["name"] = i2cKnownName(found[i]);
  }
}

void DeviceRegistry::describeState(const DeviceView& v, JsonObject st) {
  switch (v.cfg.type) {
    case DeviceType::Relay: st["on"] = v.on; break;
    case DeviceType::Input:
      st["active"] = v.on;
      st["edges"] = v.edges;
      if (v.cfg.longMs) st["long_presses"] = v.longs;
      break;
    case DeviceType::Pwm: st["duty"] = v.duty; break;
    case DeviceType::Motor: st["speed"] = v.val; break;
    case DeviceType::Servo: st["angle"] = v.val; break;
    case DeviceType::Sensor:
      st["ok"] = v.ok;
      if (v.ok) {
        st["temp_c"] = v.temp;
        if (v.cfg.model == 1) {
          st["pressure_hpa"] = v.press;
          if (v.hasHum) st["humidity"] = v.hum;
        }
      }
      break;
    case DeviceType::Serial:
      st["rx_lines"] = v.edges;
      st["last"] = v.text;
      break;
  }
}

void DeviceRegistry::describe(const DeviceView& v, JsonObject out) {
  DeviceStore::toJson(v.cfg, out);
  describeState(v, out["state"].to<JsonObject>());
  JsonArray props = out["props"].to<JsonArray>();  // what a dashboard component may bind to
  PropAccess pa[5];
  size_t npa = deviceProps(v.cfg, pa);
  for (size_t k = 0; k < npa; k++) {
    JsonObject o = props.add<JsonObject>();
    o["name"] = propName(pa[k].prop);
    o["write"] = pa[k].write;
  }
  int8_t p[5];
  size_t np = pinsOf(v.cfg, kChip, p);
  for (size_t k = 0; k < np; k++)
    if (pinBootSensitive(kChip, p[k])) { out["boot_sensitive"] = true; break; }
}

// ---- loop side (only writer) ----------------------------------------------------------------

void DeviceRegistry::begin() {
  n_ = DeviceStore::load(cfg_, kMaxDevices, kChip);
  for (size_t i = 0; i < n_; i++) startHw(i);
  Serial.printf("[dev] %u device(s) loaded\n", unsigned(n_));
}

uint8_t DeviceRegistry::allocSlot() {
  uint8_t slot = 0;
  while (slot < 8 && (pwmSlots_ & (1u << slot))) slot++;  // validateDevice caps PWM devices at 8
  pwmSlots_ |= uint8_t(1u << slot);
  return slot;
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
      r.longDone = true;                           // a button held at boot is not a long press
      break;
    case DeviceType::Pwm:
      r.slot = allocSlot();
      r.duty = c.initial;
      hal::pwmAttach(c.pin, c.freqHz, r.slot);
      hal::pwmWrite(c.pin, r.slot, uint16_t(pwmPermille(r.duty, c.invert)) * 10);
      break;
    case DeviceType::Motor:  // always starts stopped
      digitalWrite(c.pin2, LOW);
      pinMode(c.pin2, OUTPUT);
      if (c.pin3 >= 0) { digitalWrite(c.pin3, LOW); pinMode(c.pin3, OUTPUT); }
      r.slot = allocSlot();
      hal::pwmAttach(c.pin, c.freqHz, r.slot);
      driveMotor(i);
      break;
    case DeviceType::Servo:
      r.val = c.initial;
      if (c.model == 0) {
        r.slot = allocSlot();
        hal::pwmAttach(c.pin, 50, r.slot);
      } else {
        drv::i2cStart(kChip);
        if (!drv::pcaInit(c.i2cAddr)) Serial.printf("[dev] '%s': PCA9685 at 0x%02X did not answer\n", c.id, c.i2cAddr);
      }
      driveServo(i);
      break;
    case DeviceType::Sensor:
      r.next = millis();
      if (c.model == 0) r.h = drv::dsBegin(c.pin);
      else drv::i2cStart(kChip);
      break;
    case DeviceType::Serial:
      hal::uartBegin(c.pin, c.pin2, c.baud);
      serialOn_ = true;
      rxLen_ = 0;
      serialLast_[0] = 0;
      serialLines_ = 0;
      break;
  }
}

void DeviceRegistry::stopHw(size_t i) {
  const DeviceConfig& c = cfg_[i];
  Runtime& r = rt_[i];
  switch (c.type) {
    case DeviceType::Relay: digitalWrite(c.pin, relayPinLevel(false, c.invert)); break;  // leave it off
    case DeviceType::Pwm:
      hal::pwmDetach(c.pin, r.slot);
      pwmSlots_ &= uint8_t(~(1u << r.slot));
      break;
    case DeviceType::Motor:
      hal::pwmDetach(c.pin, r.slot);
      pwmSlots_ &= uint8_t(~(1u << r.slot));
      digitalWrite(c.pin2, LOW);
      if (c.pin3 >= 0) digitalWrite(c.pin3, LOW);
      break;
    case DeviceType::Servo:
      if (c.model == 0) {
        hal::pwmDetach(c.pin, r.slot);
        pwmSlots_ &= uint8_t(~(1u << r.slot));
      } else {
        drv::pcaOff(c.i2cAddr, c.channel);
      }
      break;
    case DeviceType::Sensor:
      if (r.h) { drv::dsEnd(r.h); r.h = nullptr; }
      break;
    case DeviceType::Serial:
      hal::uartEnd();
      serialOn_ = false;
      break;
    default: break;
  }
}

void DeviceRegistry::driveRelay(size_t i) { digitalWrite(cfg_[i].pin, relayPinLevel(rt_[i].on, cfg_[i].invert)); }
void DeviceRegistry::drivePwm(size_t i) {
  hal::pwmWrite(cfg_[i].pin, rt_[i].slot, uint16_t(pwmPermille(rt_[i].duty, cfg_[i].invert)) * 10);
}

void DeviceRegistry::driveMotor(size_t i) {
  const DeviceConfig& c = cfg_[i];
  MotorDrive d = motorDrive(rt_[i].val, c.invert, c.pin3 >= 0);
  hal::pwmWrite(c.pin, rt_[i].slot, 0);  // drop the enable while the direction changes
  digitalWrite(c.pin2, d.dir1);
  if (c.pin3 >= 0) digitalWrite(c.pin3, d.dir2);
  hal::pwmWrite(c.pin, rt_[i].slot, uint16_t(d.duty) * 100);
}

void DeviceRegistry::driveServo(size_t i) {
  const DeviceConfig& c = cfg_[i];
  uint16_t us = servoPulseUs(rt_[i].val, c.minUs, c.maxUs);
  if (c.model == 0) hal::pwmWrite(c.pin, rt_[i].slot, servoDuty10k(us));
  else drv::pcaSetUs(c.i2cAddr, c.channel, us);
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

void DeviceRegistry::doUpdate(const DeviceConfig& c) {
  int idx = find(c.id);
  if (idx < 0) return;
  size_t i = size_t(idx);
  DevErr e = validateDevice(c, kChip, cfg_, n_, int(i));  // re-check: the list may have changed while queued
  if (e != DevErr::Ok) {
    Serial.printf("[dev] edit '%s' dropped: %s\n", c.id, devErrCode(e));
    return;
  }
  stopHw(i);  // the old pins and PWM slot are released before the new settings claim theirs
  {
    hal::CriticalSection cs;
    cfg_[i] = c;
  }
  startHw(i);  // runtime state restarts: a relay returns to its boot value
  {
    hal::CriticalSection cs;
    structVer_ = structVer_ + 1;
  }
  bool saved = DeviceStore::save(cfg_, n_);
  Serial.printf("[dev] edited '%s' (saved=%d)\n", c.id, saved);
}

void DeviceRegistry::doImport() {
  DeviceConfig* list;
  size_t n;
  {
    hal::CriticalSection cs;
    list = importList_;
    n = importN_;
  }
  bool ok = DeviceStore::saveNew(list, n) && DeviceStore::commitNew();
  Serial.printf("[dev] import of %u device(s): %s\n", unsigned(n), ok ? "saved, restarting" : "FAILED, nothing changed");
  {
    hal::CriticalSection cs;
    importList_ = nullptr;
  }
  delete[] list;
  if (ok) restart_ = true;
}

void DeviceRegistry::doCommand(const char* id, const Command& cmd) {
  int idx = find(id);
  if (idx < 0) return;
  size_t i = size_t(idx);
  if (checkCommand(cfg_[i].type, cmd) != CmdErr::Ok) return;  // the device may have been replaced since queueing
  switch (cfg_[i].type) {
    case DeviceType::Relay: {
      bool on = cmd.action == CmdAction::Toggle ? !rt_[i].on : cmd.value != 0;
      {
        hal::CriticalSection cs;
        rt_[i].on = on;
        stateVer_ = stateVer_ + 1;
      }
      driveRelay(i);
      break;
    }
    case DeviceType::Pwm: {
      uint8_t duty = uint8_t(cmd.value + 0.5);
      {
        hal::CriticalSection cs;
        rt_[i].duty = duty;
        stateVer_ = stateVer_ + 1;
      }
      drivePwm(i);
      break;
    }
    case DeviceType::Motor:
    case DeviceType::Servo: {
      int v = cmd.action == CmdAction::Stop ? 0 : roundInt(cmd.value);
      {
        hal::CriticalSection cs;
        rt_[i].val = int16_t(v);
        stateVer_ = stateVer_ + 1;
      }
      if (cfg_[i].type == DeviceType::Motor) driveMotor(i);
      else driveServo(i);
      break;
    }
    case DeviceType::Serial: {
      size_t len = strnlen(cmd.text, kTextCap);
      hal::uartWrite(cmd.text, len);
      hal::uartWrite("\n", 1);
      break;
    }
    default: break;  // Input and Sensor are refused by checkCommand
  }
}

void DeviceRegistry::pollInputs() {
  uint32_t now = millis();
  for (size_t i = 0; i < n_; i++) {
    if (cfg_[i].type != DeviceType::Input) continue;
    Runtime& r = rt_[i];
    bool active = inputActive(digitalRead(cfg_[i].pin) != LOW, cfg_[i].invert);
    if (r.deb.update(active, now, cfg_[i].debounceMs)) {
      {
        hal::CriticalSection cs;
        r.on = r.deb.stable;
        r.edges++;
        stateVer_ = stateVer_ + 1;
      }
      if (r.on) { r.pressedAt = now; r.longDone = false; }
      Serial.printf("[dev] '%s' %s\n", cfg_[i].id, r.on ? "active" : "inactive");
    }
    if (cfg_[i].longMs && r.on && !r.longDone && uint32_t(now - r.pressedAt) >= cfg_[i].longMs) {
      r.longDone = true;  // once per press
      hal::CriticalSection cs;
      r.longs++;
      stateVer_ = stateVer_ + 1;
    }
  }
}

void DeviceRegistry::sensorFail(size_t i, uint32_t now) {
  Runtime& r = rt_[i];
  r.phase = 0;
  r.ready = false;  // an I2C chip that lost power needs its calibration read again
  r.next = now + uint32_t(cfg_[i].intervalS) * 1000u;
  if (r.fails < 255) r.fails++;
  if (r.fails >= 3 && r.ok) {  // three misses in a row: stop showing the old number
    hal::CriticalSection cs;
    r.ok = false;
    stateVer_ = stateVer_ + 1;
  }
}

void DeviceRegistry::sensorSet(size_t i, float t, float h, float p, uint32_t now) {
  Runtime& r = rt_[i];
  t = round1(t); h = round1(h); p = round1(p);
  bool changed = !r.ok || t != r.temp || h != r.hum || p != r.press;
  r.fails = 0;
  r.phase = 0;
  r.next = now + uint32_t(cfg_[i].intervalS) * 1000u;
  if (!changed) return;
  hal::CriticalSection cs;
  r.ok = true;
  r.temp = t; r.hum = h; r.press = p;
  stateVer_ = stateVer_ + 1;
}

void DeviceRegistry::pollSensors() {
  uint32_t now = millis();
  for (size_t i = 0; i < n_; i++) {
    if (cfg_[i].type != DeviceType::Sensor) continue;
    const DeviceConfig& c = cfg_[i];
    Runtime& r = rt_[i];
    bool bme = c.model == 1;
    if (r.phase == 0) {
      if (int32_t(now - r.next) < 0) continue;
      bool started;
      if (bme) {
        if (!r.ready) r.ready = drv::bmeBegin(c.i2cAddr, r.hasHum);
        started = r.ready && drv::bmeTrigger(c.i2cAddr);
      } else {
        started = drv::dsConvert(r.h);
      }
      if (started) { r.phase = 1; r.t0 = now; }
      else sensorFail(i, now);
    } else if (uint32_t(now - r.t0) >= (bme ? 20u : 800u)) {  // BME280 x1 oversampling ~10 ms, DS18B20 12-bit 750 ms
      float t = 0, h = 0, p = 0;
      bool ok = bme ? drv::bmeRead(c.i2cAddr, r.hasHum, t, h, p) : drv::dsRead(r.h, t);
      if (ok) sensorSet(i, t, h, p, now);
      else sensorFail(i, now);
    }
  }
}

void DeviceRegistry::serialEmit() {
  if (!rxLen_) return;
  rxBuf_[rxLen_] = 0;
  {
    hal::CriticalSection cs;
    memcpy(serialLast_, rxBuf_, rxLen_ + 1u);
    serialLines_++;
    stateVer_ = stateVer_ + 1;
  }
  rxLen_ = 0;
}

void DeviceRegistry::pollSerial() {
  if (!serialOn_) return;
  for (int budget = 64; budget > 0; budget--) {  // bounded, so a chatty device cannot starve loop()
    int ch = hal::uartRead();
    if (ch < 0) break;
    if (ch == '\n' || ch == '\r') serialEmit();
    else if (ch >= 32 && ch < 127) {              // printable ASCII only: the text goes out as JSON
      rxBuf_[rxLen_++] = char(ch);
      if (rxLen_ >= kTextCap - 1) serialEmit();  // an overlong line is cut into pieces
    }
  }
}

void DeviceRegistry::doScan() {
  drv::i2cStart(kChip);
  uint8_t found[16];
  uint8_t nf = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    if (drv::i2cProbe(a) && nf < sizeof found) found[nf++] = a;
    yield();
  }
  hal::CriticalSection cs;
  memcpy(found_, found, sizeof found);
  nFound_ = nf;
  scanDone_ = true;
  scanning_ = false;
  scanReq_ = false;
}

void DeviceRegistry::loop() {
  if (restart_) return;  // an import has been committed: nothing may save over it before the restart
  Op op;
  while (pop(op)) {
    if (op.kind == Op::Add) doAdd(op.cfg);
    else if (op.kind == Op::Remove) doRemove(op.id);
    else if (op.kind == Op::Update) doUpdate(op.cfg);
    else doCommand(op.id, op.cmd);
  }
  if (importList_) doImport();
  if (scanReq_) doScan();
  pollInputs();
  pollSensors();
  pollSerial();
}

}  // namespace espwf
