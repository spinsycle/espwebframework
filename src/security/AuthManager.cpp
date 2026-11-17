#include "AuthManager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "hal/Hal.h"

namespace espwf {

static const char* kFile = "/config/security.json";
static const char* kTmp = "/config/security.json.tmp";

void AuthManager::begin() {
  File f = LittleFS.open(kFile, "r");
  if (!f) f = LittleFS.open(kTmp, "r");  // only survives a power cut between remove() and rename()
  if (!f) { Serial.println("[auth] no saved password: the factory password is active and must be changed"); return; }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  uint8_t salt[kSaltBytes], hash[32];
  if (e || !doc["salt"].is<const char*>() || !doc["hash"].is<const char*>() || !doc["rounds"].is<int>() ||
      !fromHex(doc["salt"].as<const char*>(), salt, kSaltBytes) || !fromHex(doc["hash"].as<const char*>(), hash, 32) ||
      doc["rounds"].as<int>() < 1 || doc["rounds"].as<int>() > 60000) {
    Serial.println("[auth] saved password unreadable: falling back to the factory password");
    return;
  }
  core_.loadStored(salt, hash, uint16_t(doc["rounds"].as<int>()));
  Serial.println("[auth] password loaded");
}

bool AuthManager::save() const {
  uint8_t salt[kSaltBytes], hash[32];
  uint16_t rounds;
  core_.getStored(salt, hash, rounds);
  char sh[2 * kSaltBytes + 1], hh[65];
  toHex(salt, kSaltBytes, sh);
  toHex(hash, 32, hh);
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  File f = LittleFS.open(kTmp, "w");
  if (!f) return false;
  f.printf("{\"schema_version\":1,\"salt\":\"%s\",\"hash\":\"%s\",\"rounds\":%u}", sh, hh, unsigned(rounds));
  f.close();
  LittleFS.remove(kFile);
  return LittleFS.rename(kTmp, kFile);
}

AuthErr AuthManager::login(const char* password, char* token) {
  uint8_t rnd[kTokenBytes];
  hal::randomBytes(rnd, sizeof rnd);
  AuthErr e = core_.login(password, millis(), rnd, token);
  if (e == AuthErr::Wrong) Serial.println("[auth] wrong password");
  else if (e == AuthErr::Locked) Serial.println("[auth] login refused: locked out");
  else Serial.println("[auth] login ok");
  return e;
}

bool AuthManager::valid(const char* token) { return core_.valid(token, millis()); }
bool AuthManager::alive(const char* token) const { return core_.alive(token, millis()); }
void AuthManager::logout(const char* token) { core_.logout(token); }
uint32_t AuthManager::lockedForMs() const { return core_.lockedForMs(millis()); }

AuthErr AuthManager::changePassword(const char* current, const char* next, const char* keepToken) {
  if (core_.lockedForMs(millis())) return AuthErr::Locked;
  if (!core_.passwordOk(current)) return AuthErr::Wrong;
  uint8_t salt[kSaltBytes];
  hal::randomBytes(salt, sizeof salt);
  AuthErr e = core_.setPassword(next, salt);
  if (e != AuthErr::Ok) return e;
  bool saved = save();
  core_.endAllExcept(keepToken);
  Serial.printf("[auth] password changed (saved=%d)\n", saved);
  return AuthErr::Ok;
}

}  // namespace espwf
