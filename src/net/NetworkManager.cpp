#include "NetworkManager.h"

#include <Arduino.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

#include "NetworkStore.h"

namespace espwf {

static IPAddress toIp(const uint8_t a[4]) { return IPAddress(a[0], a[1], a[2], a[3]); }

static void startAp(const NetworkConfig& c) {
  IPAddress ip = toIp(c.apIp);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(c.apSsid, c.apPass, c.apChannel, false, 4);
}

void NetworkManager::begin(const NetworkConfig& cfg) {
  cfg_ = cfg;
  WiFi.persistent(false);  // never let the SDK write credentials to flash behind our back
  apply();
}

void NetworkManager::apply() {
  recoveryAp_ = false;
  staLostSince_ = 0;
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);

  switch (cfg_.mode) {
    case NetMode::AP: WiFi.mode(WIFI_AP); break;
    case NetMode::STA: WiFi.mode(WIFI_STA); break;
    default: WiFi.mode(WIFI_AP_STA); break;
  }
  if (cfg_.mode != NetMode::STA) startAp(cfg_);
  if (wantsSta()) {
#if defined(ESP8266)
    WiFi.hostname(cfg_.hostname);
#else
    WiFi.setHostname(cfg_.hostname);
#endif
    if (cfg_.staStatic) {
      WiFi.config(toIp(cfg_.staIp), toIp(cfg_.staGateway), toIp(cfg_.staMask));
    } else {
      WiFi.config(IPAddress(), IPAddress(), IPAddress());  // back to DHCP
    }
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg_.staSsid, cfg_.staPass);
  }
}

NetCfgError NetworkManager::requestApply(const NetworkConfig& cfg) {
  NetCfgError e = validateNetworkConfig(cfg);
  if (e != NetCfgError::Ok) return e;
  pending_ = cfg;
  applyAt_ = millis() + kApplyDelayMs;
  hasPending_ = true;
  return NetCfgError::Ok;
}

bool NetworkManager::staConnected() const { return WiFi.status() == WL_CONNECTED; }

void NetworkManager::loop() {
  uint32_t now = millis();

  if (hasPending_ && int32_t(now - applyAt_) >= 0) {
    cfg_ = pending_;
    hasPending_ = false;
    NetworkStore::save(cfg_);
    apply();
    return;
  }

  if (!wantsSta()) return;
  if (staConnected()) {
    staLostSince_ = 0;
    if (recoveryAp_ && cfg_.mode == NetMode::STA) {  // link is back: drop the recovery AP
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      recoveryAp_ = false;
    }
    return;
  }
  if (staLostSince_ == 0) staLostSince_ = now ? now : 1;
  if (cfg_.mode == NetMode::STA && !recoveryAp_ && now - staLostSince_ >= kRecoveryMs) {
    WiFi.mode(WIFI_AP_STA);
    startAp(cfg_);
    recoveryAp_ = true;
  }
}

void NetworkManager::status(JsonObject out) const {
  bool conn = staConnected();
  out["mode"] = netModeName(cfg_.mode);
  out["hostname"] = cfg_.hostname;
  out["mac"] = WiFi.macAddress();
  out["recovery_ap"] = recoveryAp_;
  bool apUp = cfg_.mode != NetMode::STA || recoveryAp_;
  JsonObject ap = out["ap"].to<JsonObject>();
  ap["active"] = apUp;
  if (apUp) {
    ap["ssid"] = cfg_.apSsid;
    ap["ip"] = WiFi.softAPIP().toString();
    ap["clients"] = WiFi.softAPgetStationNum();
  }
  JsonObject sta = out["sta"].to<JsonObject>();
  sta["enabled"] = wantsSta();
  sta["connected"] = conn;
  if (wantsSta()) sta["ssid"] = cfg_.staSsid;
  if (conn) {
    sta["ip"] = WiFi.localIP().toString();
    sta["rssi"] = WiFi.RSSI();
  }
}

}  // namespace espwf
