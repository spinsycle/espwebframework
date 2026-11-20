#include "NetworkManager.h"

#include <Arduino.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#else
#include <ESPmDNS.h>
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
  mdnsTick(false);  // drop the responder before the radio restarts; loop() brings it back up
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
      staticApplied_ = true;
    } else if (staticApplied_) {
      WiFi.config(IPAddress(), IPAddress(), IPAddress());  // back to DHCP after a static run
      staticApplied_ = false;
    }  // plain DHCP is the default: calling config() with zeros every time can stall the DHCP client
    // Radio power-save makes the board miss the router's DHCP reply (minutes to get an IP) and adds
    // latency to every request. This is a mains-powered server, so keep the radio awake.
#if defined(ESP8266)
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
#else
    WiFi.setSleep(false);
#endif
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg_.staSsid, cfg_.staPass);
  }
}

NetCfgError NetworkManager::requestApply(const NetworkConfig& cfg) {
  NetCfgError e = validateNetworkConfig(cfg);
  if (e != NetCfgError::Ok) return e;
  pending_ = cfg;
  applyAt_ = millis() + kApplyDelayMs;
  Serial.printf("[net] %lu apply queued (in %lu ms)\n", (unsigned long)millis(), (unsigned long)kApplyDelayMs);
  hasPending_ = true;
  return NetCfgError::Ok;
}

bool NetworkManager::staConnected() const { return WiFi.status() == WL_CONNECTED; }

void NetworkManager::mdnsTick(bool staUp) {
  bool want = staUp && cfg_.mdnsEnabled && wantsSta();
  if (want && !mdnsUp_) {
    if (MDNS.begin(cfg_.mdnsName)) {
      MDNS.addService("http", "tcp", 80);
      mdnsUp_ = true;
      Serial.printf("[net] mDNS up: %s.local\n", cfg_.mdnsName);
    }  // else: retried on the next loop() pass
  } else if (!want && mdnsUp_) {
    MDNS.end();
    mdnsUp_ = false;
    Serial.println("[net] mDNS down");
  }
#if defined(ESP8266)
  if (mdnsUp_) MDNS.update();
#endif
}

void NetworkManager::loop() {
  uint32_t now = millis();

  if (hasPending_ && int32_t(now - applyAt_) >= 0) {
    cfg_ = pending_;
    hasPending_ = false;
    Serial.printf("[net] %lu applying mode=%s sta='%s'\n", (unsigned long)now, netModeName(cfg_.mode), cfg_.staSsid);
    bool saved = NetworkStore::save(cfg_);
    Serial.printf("[net] config saved=%d\n", saved);
    apply();
    Serial.printf("[net] %lu applied\n", (unsigned long)millis());
    return;
  }

  if (!wantsSta()) return;
  bool conn = staConnected();
  if (conn != lastConn_) {
    lastConn_ = conn;
    Serial.printf("[net] %lu station %s\n", (unsigned long)now, conn ? "connected" : "disconnected");
  }
  mdnsTick(conn);
  if (conn) {
    staLostSince_ = 0;
    if (recoveryAp_ && cfg_.mode == NetMode::STA) {  // link is back: drop the recovery AP
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      recoveryAp_ = false;
    }
    return;
  }
  if (staLostSince_ == 0) staLostSince_ = now ? now : 1;
  static uint32_t lastDbg = 0;
  if (now - lastDbg >= 10000) {  // join progress, until it connects
    lastDbg = now;
    Serial.printf("[net] %lu joining: wifi status %d, ip %s\n", (unsigned long)now, (int)WiFi.status(),
                  WiFi.localIP().toString().c_str());
  }
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
  JsonObject md = out["mdns"].to<JsonObject>();
  md["enabled"] = cfg_.mdnsEnabled;
  md["active"] = mdnsUp_;
  if (mdnsUp_) {
    char n[sizeof cfg_.mdnsName + 6];
    snprintf(n, sizeof n, "%s.local", cfg_.mdnsName);
    md["name"] = n;
  }
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
  if (wantsSta()) {
    sta["ssid"] = cfg_.staSsid;
    if (!conn) {
      wl_status_t s = WiFi.status();
      sta["reason"] = s == WL_NO_SSID_AVAIL ? "network not found (2.4 GHz? exact name?)"
#if defined(ESP8266)
                    : s == WL_WRONG_PASSWORD ? "wrong password"
#endif
                    : s == WL_CONNECT_FAILED ? "join failed"
                    : "not connected";
      sta["code"] = (int)s;  // raw wl_status_t: 0 idle, 1 no SSID, 4 connect failed, 6/7 disconnected
    }
  }
  if (conn) {
    sta["ip"] = WiFi.localIP().toString();
    sta["rssi"] = WiFi.RSSI();
  }
}

}  // namespace espwf
