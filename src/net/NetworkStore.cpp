#include "NetworkStore.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "hal/Hal.h"

namespace espwf {
namespace NetworkStore {

static const char* kFile = "/config/network.json";
static const char* kTmp = "/config/network.json.tmp";

static bool setStr(JsonVariantConst v, char* dst, unsigned cap, bool& bad) {
  if (v.isNull()) return false;
  if (!v.is<const char*>()) { bad = true; return false; }
  const char* s = v.as<const char*>();
  if (strlen(s) >= cap) { bad = true; return false; }
  strcpy(dst, s);
  return true;
}

static bool setIp(JsonVariantConst v, uint8_t dst[4], bool& bad) {
  if (v.isNull()) return false;
  uint8_t t[4];
  if (!v.is<const char*>() || !parseIPv4(v.as<const char*>(), t)) { bad = true; return false; }
  memcpy(dst, t, 4);
  return true;
}

NetCfgError applyJson(JsonVariantConst src, NetworkConfig& cfg) {
  if (!src.is<JsonObjectConst>()) return NetCfgError::Format;
  bool bad;

  JsonVariantConst m = src["mode"];
  if (!m.isNull()) {
    if (!m.is<const char*>() || !parseNetMode(m.as<const char*>(), cfg.mode)) return NetCfgError::Mode;
  }
  bad = false; setStr(src["hostname"], cfg.hostname, kHostCap, bad);
  if (bad) return NetCfgError::Hostname;

  JsonVariantConst ap = src["ap"];
  bad = false; setStr(ap["ssid"], cfg.apSsid, kSsidCap, bad);
  if (bad) return NetCfgError::ApSsid;
  bad = false; setStr(ap["password"], cfg.apPass, kPassCap, bad);
  if (bad) return NetCfgError::ApPass;
  if (!ap["channel"].isNull()) {
    if (!ap["channel"].is<int>()) return NetCfgError::ApChannel;
    int ch = ap["channel"].as<int>();
    if (ch < 1 || ch > 13) return NetCfgError::ApChannel;
    cfg.apChannel = uint8_t(ch);
  }
  bad = false; setIp(ap["ip"], cfg.apIp, bad);
  if (bad) return NetCfgError::ApIp;

  JsonVariantConst sta = src["sta"];
  bad = false; setStr(sta["ssid"], cfg.staSsid, kSsidCap, bad);
  if (bad) return NetCfgError::StaSsid;
  bad = false; setStr(sta["password"], cfg.staPass, kPassCap, bad);
  if (bad) return NetCfgError::StaPass;
  if (!sta["static"].isNull()) {
    if (!sta["static"].is<bool>()) return NetCfgError::Format;
    cfg.staStatic = sta["static"].as<bool>();
  }
  bad = false; setIp(sta["ip"], cfg.staIp, bad);
  if (bad) return NetCfgError::StaIp;
  bad = false; setIp(sta["gateway"], cfg.staGateway, bad);
  if (bad) return NetCfgError::StaGateway;
  bad = false; setIp(sta["mask"], cfg.staMask, bad);
  if (bad) return NetCfgError::StaMask;
  return NetCfgError::Ok;
}

static void ipStr(const uint8_t ip[4], char* out) {
  snprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

void toJson(const NetworkConfig& c, JsonObject out, bool includeSecrets) {
  char b[16];
  out["schema_version"] = 1;
  out["mode"] = netModeName(c.mode);
  out["hostname"] = c.hostname;
  JsonObject ap = out["ap"].to<JsonObject>();
  ap["ssid"] = c.apSsid;
  ap["channel"] = c.apChannel;
  ipStr(c.apIp, b); ap["ip"] = b;
  JsonObject sta = out["sta"].to<JsonObject>();
  sta["ssid"] = c.staSsid;
  sta["static"] = c.staStatic;
  ipStr(c.staIp, b); sta["ip"] = b;
  ipStr(c.staGateway, b); sta["gateway"] = b;
  ipStr(c.staMask, b); sta["mask"] = b;
  if (includeSecrets) {
    ap["password"] = c.apPass;
    sta["password"] = c.staPass;
  } else {
    ap["password_set"] = c.apPass[0] != 0;
    sta["password_set"] = c.staPass[0] != 0;
  }
}

static bool readFile(const char* path, NetworkConfig& cfg) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return false;
  NetworkConfig t = cfg;
  if (applyJson(doc.as<JsonVariantConst>(), t) != NetCfgError::Ok) return false;
  if (validateNetworkConfig(t) != NetCfgError::Ok) return false;
  cfg = t;
  return true;
}

bool load(NetworkConfig& cfg) {
  // A tmp file only survives if power failed between remove() and rename() in save().
  return readFile(kFile, cfg) || readFile(kTmp, cfg);
}

bool save(const NetworkConfig& cfg) {
  hal::FsWrite g;
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  JsonDocument doc;
  toJson(cfg, doc.to<JsonObject>(), true);
  File f = LittleFS.open(kTmp, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) { LittleFS.remove(kTmp); return false; }
  LittleFS.remove(kFile);
  return LittleFS.rename(kTmp, kFile);
}

}  // namespace NetworkStore
}  // namespace espwf
