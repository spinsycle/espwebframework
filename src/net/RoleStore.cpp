#include "core/Features.h"
#if ESPWF_HAS_ROLE

#include "RoleStore.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "config/ConfigRules.h"
#include "hal/Hal.h"

namespace espwf {
namespace RoleStore {

static const char* kFile = "/config/role.json";
static const char* kTmp = "/config/role.json.tmp";
static const char* kBak = "/config/role.json.bak";

static bool setStr(JsonVariantConst v, char* dst, unsigned cap, bool& bad) {
  if (v.isNull()) return false;
  if (!v.is<const char*>()) { bad = true; return false; }
  const char* s = v.as<const char*>();
  if (strlen(s) >= cap) { bad = true; return false; }
  strcpy(dst, s);
  return true;
}

RoleCfgError applyJson(JsonVariantConst src, RoleConfig& cfg, int* badLink) {
  if (badLink) *badLink = -1;
  if (!src.is<JsonObjectConst>()) return RoleCfgError::Format;
  bool bad = false;
  JsonVariantConst r = src["role"];
  if (!r.isNull()) {
    const char* s = r.is<const char*>() ? r.as<const char*>() : "";
    if (!strcmp(s, "master")) cfg.role = Role::Master;
    else if (!strcmp(s, "slave")) cfg.role = Role::Slave;
    else return RoleCfgError::Role;
  }
  setStr(src["host"], cfg.host, kRoleHostCap, bad);
  if (bad) return RoleCfgError::Host;
  if (!src["port"].isNull()) {
    if (!src["port"].is<int>()) return RoleCfgError::Port;
    int p = src["port"].as<int>();
    if (p < 1 || p > 65535) return RoleCfgError::Port;
    cfg.port = uint16_t(p);
  }
  setStr(src["password"], cfg.pass, kRolePassCap, bad);
  if (bad) return RoleCfgError::Password;

  JsonVariantConst ls = src["links"];
  if (!ls.isNull()) {
    if (!ls.is<JsonArrayConst>()) return RoleCfgError::Link;
    JsonArrayConst a = ls.as<JsonArrayConst>();
    if (a.size() > kMaxLinks) return RoleCfgError::Link;
    RoleConfig t;  // build the new list aside, so a bad entry leaves the old one alone
    int idx = 0;
    for (JsonVariantConst e : a) {
      LinkRule& l = t.links[idx];
      bool b = !e.is<JsonObjectConst>();
      if (!b) {
        bool bi = false, br = false;
        b = !setStr(e["input"], l.input, kIdCap, bi) || bi || !setStr(e["remote"], l.remote, kIdCap, br) || br ||
            !parseLinkTrig(e["trigger"].is<const char*>() ? e["trigger"].as<const char*>() : nullptr, l.trig) ||
            !parseLinkAct(e["action"].is<const char*>() ? e["action"].as<const char*>() : nullptr, l.act);
        if (!b && !e["value"].isNull()) {
          if (!e["value"].is<int>()) b = true; else l.value = int16_t(e["value"].as<int>());
        }
      }
      if (b) { if (badLink) *badLink = idx; return RoleCfgError::Link; }
      idx++;
    }
    t.nLinks = uint8_t(idx);
    memcpy(cfg.links, t.links, sizeof cfg.links);
    cfg.nLinks = t.nLinks;
  }
  return RoleCfgError::Ok;
}

void toJson(const RoleConfig& c, JsonObject out, bool includeSecrets) {
  out["schema_version"] = kConfigSchema;
  out["role"] = c.role == Role::Slave ? "slave" : "master";
  out["host"] = c.host;
  out["port"] = c.port;
  if (includeSecrets) out["password"] = c.pass;
  else out["password_set"] = c.pass[0] != 0;
  JsonArray a = out["links"].to<JsonArray>();
  for (size_t i = 0; i < c.nLinks; i++) {
    const LinkRule& l = c.links[i];
    JsonObject o = a.add<JsonObject>();
    o["input"] = l.input;
    o["trigger"] = linkTrigName(l.trig);
    o["remote"] = l.remote;
    o["action"] = linkActName(l.act);
    if (l.act == LinkAct::Set) o["value"] = l.value;
  }
}

static bool readFile(const char* path, RoleConfig& cfg) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return false;
  JsonVariantConst sv = doc["schema_version"];
  if (!sv.isNull() && (!sv.is<long>() || configSchemaCheck(sv.as<long>()) != SchemaCheck::Ok)) return false;
  RoleConfig t = cfg;
  if (applyJson(doc.as<JsonVariantConst>(), t) != RoleCfgError::Ok) return false;
  if (validateRoleConfig(t) != RoleCfgError::Ok) return false;
  cfg = t;
  return true;
}

bool load(RoleConfig& cfg) { return readFile(kFile, cfg) || readFile(kTmp, cfg) || readFile(kBak, cfg); }

bool save(const RoleConfig& cfg) {
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

}  // namespace RoleStore
}  // namespace espwf

#endif  // ESPWF_HAS_ROLE
