#include "MqttStore.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "config/ConfigRules.h"
#include "hal/Hal.h"

namespace espwf {
namespace MqttStore {

static const char* kFile = "/config/mqtt.json";
static const char* kTmp = "/config/mqtt.json.tmp";
static const char* kBak = "/config/mqtt.json.bak";

static bool setStr(JsonVariantConst v, char* dst, unsigned cap, bool& bad) {
  if (v.isNull()) return false;
  if (!v.is<const char*>()) { bad = true; return false; }
  const char* s = v.as<const char*>();
  if (strlen(s) >= cap) { bad = true; return false; }
  strcpy(dst, s);
  return true;
}

MqttCfgError applyJson(JsonVariantConst src, MqttConfig& cfg) {
  if (!src.is<JsonObjectConst>()) return MqttCfgError::Format;
  bool bad = false;
  if (!src["enabled"].isNull()) {
    if (!src["enabled"].is<bool>()) return MqttCfgError::Format;
    cfg.enabled = src["enabled"].as<bool>();
  }
  setStr(src["host"], cfg.host, kMqttHostCap, bad);
  if (bad) return MqttCfgError::Host;
  if (!src["port"].isNull()) {
    if (!src["port"].is<int>()) return MqttCfgError::Port;
    int p = src["port"].as<int>();
    if (p < 1 || p > 65535) return MqttCfgError::Port;
    cfg.port = uint16_t(p);
  }
  setStr(src["user"], cfg.user, kMqttUserCap, bad);
  if (bad) return MqttCfgError::User;
  setStr(src["password"], cfg.pass, kMqttPassCap, bad);
  if (bad) return MqttCfgError::Pass;
  setStr(src["base"], cfg.base, kMqttBaseCap, bad);
  if (bad) return MqttCfgError::Base;
  if (!src["keepalive"].isNull()) {
    if (!src["keepalive"].is<int>()) return MqttCfgError::KeepAlive;
    int k = src["keepalive"].as<int>();
    if (k < 5 || k > 600) return MqttCfgError::KeepAlive;
    cfg.keepAlive = uint16_t(k);
  }
  return MqttCfgError::Ok;
}

void toJson(const MqttConfig& c, JsonObject out, bool includeSecrets) {
  out["schema_version"] = kConfigSchema;
  out["enabled"] = c.enabled;
  out["host"] = c.host;
  out["port"] = c.port;
  out["user"] = c.user;
  out["base"] = c.base;
  out["keepalive"] = c.keepAlive;
  if (includeSecrets) out["password"] = c.pass;
  else out["password_set"] = c.pass[0] != 0;
}

static bool readFile(const char* path, MqttConfig& cfg) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return false;
  JsonVariantConst sv = doc["schema_version"];
  if (!sv.isNull() && (!sv.is<long>() || configSchemaCheck(sv.as<long>()) != SchemaCheck::Ok)) return false;
  MqttConfig t = cfg;
  if (applyJson(doc.as<JsonVariantConst>(), t) != MqttCfgError::Ok) return false;
  if (validateMqttConfig(t) != MqttCfgError::Ok) return false;
  cfg = t;
  return true;
}

bool load(MqttConfig& cfg) { return readFile(kFile, cfg) || readFile(kTmp, cfg) || readFile(kBak, cfg); }

bool save(const MqttConfig& cfg) {
  hal::FsWrite g;
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  JsonDocument doc;
  toJson(cfg, doc.to<JsonObject>(), true);
  File f = LittleFS.open(kTmp, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) { LittleFS.remove(kTmp); return false; }
  LittleFS.remove(kBak);
  if (LittleFS.exists(kFile)) LittleFS.rename(kFile, kBak);
  return LittleFS.rename(kTmp, kFile);
}

}  // namespace MqttStore
}  // namespace espwf
