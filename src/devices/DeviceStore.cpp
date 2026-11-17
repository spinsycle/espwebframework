#include "DeviceStore.h"

#include <Arduino.h>
#include <LittleFS.h>

namespace espwf {
namespace DeviceStore {

static const char* kFile = "/config/devices.json";
static const char* kTmp = "/config/devices.json.tmp";

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
  if (getInt(src["initial"], 0, 255, n, bad)) cfg.initial = uint8_t(n);
  if (bad) return DevErr::Initial;

  if (!cfg.name[0]) strcpy(cfg.name, cfg.id);  // the label defaults to the id
  return DevErr::Ok;
}

void toJson(const DeviceConfig& c, JsonObject out) {
  out["id"] = c.id;
  out["name"] = c.name;
  out["type"] = deviceTypeName(c.type);
  out["pin"] = c.pin;
  out["invert"] = c.invert;
  if (c.type == DeviceType::Input) {
    out["pullup"] = c.pullup;
    out["debounce_ms"] = c.debounceMs;
  }
  if (c.type == DeviceType::Pwm) out["freq_hz"] = c.freqHz;
  if (c.type != DeviceType::Input) out["initial"] = c.initial;
}

size_t load(DeviceConfig* out, size_t cap, Chip chip) {
  File f = LittleFS.open(kFile, "r");
  if (!f) f = LittleFS.open(kTmp, "r");  // only survives a power cut between remove() and rename()
  if (!f) return 0;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { Serial.println("[dev] devices.json unreadable, starting empty"); return 0; }

  size_t n = 0;
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
  return n;
}

bool save(const DeviceConfig* devs, size_t n) {
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  File f = LittleFS.open(kTmp, "w");
  if (!f) return false;
  f.print("{\"schema_version\":1,\"devices\":[");
  for (size_t i = 0; i < n; i++) {  // one device at a time: no big JSON document on the heap
    JsonDocument d;
    toJson(devs[i], d.to<JsonObject>());
    if (i) f.print(',');
    serializeJson(d, f);
  }
  f.print("]}");
  f.close();
  LittleFS.remove(kFile);
  return LittleFS.rename(kTmp, kFile);
}

}  // namespace DeviceStore
}  // namespace espwf
