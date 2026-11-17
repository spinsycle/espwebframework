#include "DeviceStore.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "config/ConfigRules.h"

namespace espwf {
namespace DeviceStore {

static const char* kFile = "/config/devices.json";
static const char* kTmp = "/config/devices.json.tmp";
static const char* kBak = "/config/devices.json.bak";
static const char* kNew = "/config/devices.json.new";

// Reads an optional integer field in [lo, hi]. Returns false (and sets bad) on a wrong type or range.
static bool getInt(JsonVariantConst v, long lo, long hi, long& out, bool& bad) {
  if (v.isNull()) return false;
  if (!v.is<long>() || v.is<bool>()) { bad = true; return false; }
  long x = v.as<long>();
  if (x < lo || x > hi) { bad = true; return false; }
  out = x;
  return true;
}

DevErr applyJson(JsonVariantConst src, DeviceConfig& cfg) {
  if (!src.is<JsonObjectConst>()) return DevErr::Format;
  bool bad;
  long n;

  JsonVariantConst v = src["id"];
  if (!v.isNull()) {
    if (!v.is<const char*>() || strlen(v.as<const char*>()) >= kIdCap) return DevErr::Id;
    strcpy(cfg.id, v.as<const char*>());
  }
  v = src["name"];
  if (!v.isNull()) {
    if (!v.is<const char*>() || strlen(v.as<const char*>()) >= kNameCap) return DevErr::Name;
    strcpy(cfg.name, v.as<const char*>());
  }
  v = src["type"];
  if (!v.isNull()) {
    if (!v.is<const char*>() || !parseDeviceType(v.as<const char*>(), cfg.type)) return DevErr::Type;
  }
  bad = false;
  if (getInt(src["pin"], -1, 127, n, bad)) cfg.pin = int8_t(n);
  if (bad) return DevErr::Pin;
  v = src["invert"];
  if (!v.isNull()) { if (!v.is<bool>()) return DevErr::Format; cfg.invert = v.as<bool>(); }
  v = src["pullup"];
  if (!v.isNull()) { if (!v.is<bool>()) return DevErr::Format; cfg.pullup = v.as<bool>(); }
  bad = false;
  if (getInt(src["debounce_ms"], 0, 65535, n, bad)) cfg.debounceMs = uint16_t(n);
  if (bad) return DevErr::Debounce;
  bad = false;
  if (getInt(src["freq_hz"], 0, 65535, n, bad)) cfg.freqHz = uint16_t(n);
  if (bad) return DevErr::Freq;
  bad = false;
  bool hasInitial = getInt(src["initial"], 0, 255, n, bad);
  if (hasInitial) cfg.initial = uint8_t(n);
  if (bad) return DevErr::Initial;

  // P4 fields. Ranges here only guard the storage type; validateDevice applies the real limits.
  bad = false;
  if (getInt(src["pin2"], -1, 127, n, bad)) cfg.pin2 = int8_t(n);
  if (bad) return DevErr::Pin;
  bad = false;
  if (getInt(src["pin3"], -1, 127, n, bad)) cfg.pin3 = int8_t(n);
  if (bad) return DevErr::Pin;
  v = src["model"];
  if (!v.isNull()) {
    if (!v.is<const char*>() || !parseModel(cfg.type, v.as<const char*>(), cfg.model)) return DevErr::Model;
  }
  bad = false;
  if (getInt(src["i2c_addr"], 0, 255, n, bad)) cfg.i2cAddr = uint8_t(n);
  if (bad) return DevErr::I2cAddr;
  bad = false;
  if (getInt(src["channel"], 0, 255, n, bad)) cfg.channel = uint8_t(n);
  if (bad) return DevErr::Channel;
  bad = false;
  if (getInt(src["min_us"], 0, 65535, n, bad)) cfg.minUs = uint16_t(n);
  if (bad) return DevErr::Pulse;
  bad = false;
  if (getInt(src["max_us"], 0, 65535, n, bad)) cfg.maxUs = uint16_t(n);
  if (bad) return DevErr::Pulse;
  bad = false;
  if (getInt(src["interval_s"], 0, 65535, n, bad)) cfg.intervalS = uint16_t(n);
  if (bad) return DevErr::Interval;
  bad = false;
  if (getInt(src["long_ms"], 0, 65535, n, bad)) cfg.longMs = uint16_t(n);
  if (bad) return DevErr::LongMs;
  bad = false;
  if (getInt(src["baud"], 0, 1000000, n, bad)) cfg.baud = uint32_t(n);
  if (bad) return DevErr::Baud;

  if (cfg.type == DeviceType::Servo && !hasInitial) cfg.initial = 90;  // centred, not slammed to an end stop
  if (isI2cDevice(cfg) && cfg.i2cAddr == 0) cfg.i2cAddr = defaultI2cAddr(cfg);

  if (!cfg.name[0]) strcpy(cfg.name, cfg.id);  // the label defaults to the id
  return DevErr::Ok;
}

void toJson(const DeviceConfig& c, JsonObject out) {
  out["id"] = c.id;
  out["name"] = c.name;
  out["type"] = deviceTypeName(c.type);
  out["pin"] = c.pin;
  out["invert"] = c.invert;
  switch (c.type) {
    case DeviceType::Input:
      out["pullup"] = c.pullup;
      out["debounce_ms"] = c.debounceMs;
      out["long_ms"] = c.longMs;
      break;
    case DeviceType::Relay: out["initial"] = c.initial; break;
    case DeviceType::Pwm: out["freq_hz"] = c.freqHz; out["initial"] = c.initial; break;
    case DeviceType::Motor:
      out["pin2"] = c.pin2;
      if (c.pin3 >= 0) out["pin3"] = c.pin3;
      out["freq_hz"] = c.freqHz;
      break;
    case DeviceType::Servo:
      out["model"] = modelName(c.type, c.model);
      if (c.model == 1) { out["i2c_addr"] = c.i2cAddr; out["channel"] = c.channel; }
      out["min_us"] = c.minUs;
      out["max_us"] = c.maxUs;
      out["initial"] = c.initial;
      break;
    case DeviceType::Sensor:
      out["model"] = modelName(c.type, c.model);
      if (c.model == 1) out["i2c_addr"] = c.i2cAddr;
      out["interval_s"] = c.intervalS;
      break;
    case DeviceType::Serial:
      out["pin2"] = c.pin2;
      out["baud"] = c.baud;
      break;
  }
}

bool schemaOk(JsonVariantConst doc) {
  JsonVariantConst v = doc["schema_version"];
  if (v.isNull()) return configSchemaCheck(-1) == SchemaCheck::Ok;
  if (!v.is<long>() || v.is<bool>()) return false;
  return configSchemaCheck(v.as<long>()) == SchemaCheck::Ok;
}

// Reads one file; false if it is missing, unparsable or from a newer framework.
static bool readList(const char* path, DeviceConfig* out, size_t cap, Chip chip, size_t& n) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e || !schemaOk(doc)) { Serial.printf("[dev] %s unreadable or too new\n", path); return false; }
  n = 0;
  for (JsonVariantConst item : doc["devices"].as<JsonArrayConst>()) {
    if (n >= cap) break;
    DeviceConfig c;
    DevErr err = applyJson(item, c);
    if (err == DevErr::Ok) err = validateDevice(c, chip, out, n);
    if (err != DevErr::Ok) {
      Serial.printf("[dev] skipped saved device '%s': %s\n", c.id, devErrCode(err));
      continue;
    }
    out[n++] = c;
  }
  return true;
}

size_t load(DeviceConfig* out, size_t cap, Chip chip) {
  size_t n = 0;
  // tmp only survives a power cut between the two renames in save(); bak is the previous good file.
  if (readList(kFile, out, cap, chip, n) || readList(kTmp, out, cap, chip, n) || readList(kBak, out, cap, chip, n)) return n;
  return 0;
}

static bool writeList(const char* path, const DeviceConfig* devs, size_t n) {
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.printf("{\"schema_version\":%d,\"devices\":[", kConfigSchema);
  for (size_t i = 0; i < n; i++) {  // one device at a time: no big JSON document on the heap
    JsonDocument d;
    toJson(devs[i], d.to<JsonObject>());
    if (i) f.print(',');
    serializeJson(d, f);
  }
  f.print("]}");
  f.close();
  return true;
}

// tmp -> file, keeping the file it replaces as .bak.
static bool commit(const char* from) {
  LittleFS.remove(kBak);
  if (LittleFS.exists(kFile)) LittleFS.rename(kFile, kBak);
  return LittleFS.rename(from, kFile);
}

bool save(const DeviceConfig* devs, size_t n) { return writeList(kTmp, devs, n) && commit(kTmp); }
bool saveNew(const DeviceConfig* devs, size_t n) { return writeList(kNew, devs, n); }
bool commitNew() { return commit(kNew); }

}  // namespace DeviceStore
}  // namespace espwf
