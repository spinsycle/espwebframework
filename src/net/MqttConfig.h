#pragma once
// MQTT settings, topic building and command-payload parsing. Pure C++ (no Arduino) so it is unit-tested natively.
//
// Topics (base = the configured base topic, id = a device id):
//   <base>/status          retained "online" / "offline" (the last will)
//   <base>/<id>/state      retained JSON, published on every change of that device
//   <base>/<id>/set        command in: on|off|toggle|stop|<number>, or the line to send to a serial device
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices/DeviceTypes.h"

namespace espwf {

constexpr unsigned kMqttHostCap = 65;   // 64 chars + NUL
constexpr unsigned kMqttUserCap = 33;
constexpr unsigned kMqttPassCap = 65;
constexpr unsigned kMqttBaseCap = 41;   // 40 chars + NUL
constexpr unsigned kMqttTopicCap = kMqttBaseCap + kIdCap + 8;

struct MqttConfig {
  bool enabled = false;
  char host[kMqttHostCap] = {0};        // IP address or a DNS name (a .local name does not resolve on the ESP8266)
  uint16_t port = 1883;
  char user[kMqttUserCap] = {0};
  char pass[kMqttPassCap] = {0};
  char base[kMqttBaseCap] = {0};
  uint16_t keepAlive = 30;              // seconds
};

enum class MqttCfgError : uint8_t { Ok = 0, Host, Port, User, Pass, Base, KeepAlive, Format };

inline const char* mqttCfgErrorField(MqttCfgError e) {
  switch (e) {
    case MqttCfgError::Ok: return "";
    case MqttCfgError::Host: return "host";
    case MqttCfgError::Port: return "port";
    case MqttCfgError::User: return "user";
    case MqttCfgError::Pass: return "password";
    case MqttCfgError::Base: return "base";
    case MqttCfgError::KeepAlive: return "keepalive";
    default: return "format";
  }
}

namespace mqttdetail {
inline bool terminated(const char* s, unsigned cap) { return memchr(s, 0, cap) != nullptr; }
inline bool validHost(const char* h) {
  size_t n = strlen(h);
  if (n < 1 || n > 64) return false;
  for (size_t i = 0; i < n; i++) {
    char c = h[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}
// 1-40 of letters, digits, '-', '_' and '/'; no leading, trailing or doubled '/'. No wildcards, so it is a literal topic prefix.
inline bool validBase(const char* b) {
  size_t n = strlen(b);
  if (n < 1 || n > 40 || b[0] == '/' || b[n - 1] == '/') return false;
  for (size_t i = 0; i < n; i++) {
    char c = b[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '/';
    if (!ok || (c == '/' && b[i + 1] == '/')) return false;
  }
  return true;
}
}  // namespace mqttdetail

// Factory defaults: off, and a base topic equal to the board's hostname so two boards never collide.
inline void setMqttDefaults(MqttConfig& c, const char* hostname) {
  c = MqttConfig();
  snprintf(c.base, sizeof c.base, "%s", hostname ? hostname : "");
}

inline MqttCfgError validateMqttConfig(const MqttConfig& c) {
  using namespace mqttdetail;
  if (!terminated(c.host, kMqttHostCap)) return MqttCfgError::Host;
  if ((c.enabled || c.host[0]) && !validHost(c.host)) return MqttCfgError::Host;
  if (c.port < 1) return MqttCfgError::Port;
  if (!terminated(c.user, kMqttUserCap)) return MqttCfgError::User;
  if (!terminated(c.pass, kMqttPassCap)) return MqttCfgError::Pass;
  if (!terminated(c.base, kMqttBaseCap) || !validBase(c.base)) return MqttCfgError::Base;
  if (c.keepAlive < 5 || c.keepAlive > 600) return MqttCfgError::KeepAlive;
  return MqttCfgError::Ok;
}

// "<base>/<id>/<leaf>", or "<base>/<leaf>" when id is null. False if it does not fit.
inline bool mqttTopic(char* out, size_t cap, const char* base, const char* id, const char* leaf) {
  int n = id ? snprintf(out, cap, "%s/%s/%s", base, id, leaf) : snprintf(out, cap, "%s/%s", base, leaf);
  return n > 0 && size_t(n) < cap;
}

// "<base>/<id>/set" -> id. False for any other topic or an id that is not a valid device id.
inline bool mqttParseSetTopic(const char* base, const char* topic, char* idOut) {
  size_t bl = strlen(base);
  if (strncmp(topic, base, bl) != 0 || topic[bl] != '/') return false;
  const char* id = topic + bl + 1;
  const char* slash = strchr(id, '/');
  if (!slash || strcmp(slash, "/set") != 0) return false;
  size_t n = size_t(slash - id);
  if (n < 1 || n >= kIdCap) return false;
  memcpy(idOut, id, n);
  idOut[n] = 0;
  return detail::validDeviceId(idOut);
}

// Payload text -> Command for a device of type t. False when the text means nothing for that device.
// Whether the device accepts the command is still checked by DeviceRegistry::requestCommand.
inline bool mqttParseCommand(DeviceType t, const char* p, size_t len, Command& out) {
  out = Command();
  while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
  while (len && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r' || p[len - 1] == '\n')) len--;
  if (len < 1 || len >= kTextCap) return false;
  if (t == DeviceType::Serial) {
    out.action = CmdAction::Send;
    out.isText = true;
    memcpy(out.text, p, len);
    out.text[len] = 0;
    return true;
  }
  char w[kTextCap];
  for (size_t i = 0; i < len; i++) {
    char c = p[i];
    w[i] = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
  }
  w[len] = 0;
  if (!strcmp(w, "toggle")) { out.action = CmdAction::Toggle; return true; }
  if (!strcmp(w, "stop") || (t == DeviceType::Motor && !strcmp(w, "off"))) { out.action = CmdAction::Stop; return true; }
  bool isOn = !strcmp(w, "on") || !strcmp(w, "true");
  bool isOff = !strcmp(w, "off") || !strcmp(w, "false");
  if (t == DeviceType::Relay) {
    if (!isOn && !isOff) { isOn = !strcmp(w, "1"); isOff = !strcmp(w, "0"); }
    if (!isOn && !isOff) return false;
    out.action = CmdAction::Set; out.hasValue = true; out.isBool = true; out.value = isOn ? 1 : 0;
    return true;
  }
  out.action = CmdAction::Set;
  out.hasValue = true;
  if (t == DeviceType::Pwm && (isOn || isOff)) { out.value = isOn ? 100 : 0; return true; }
  char* end = nullptr;
  double v = strtod(w, &end);
  if (end == w || *end != 0 || !isfinite(v)) return false;
  out.value = v;
  return true;
}

// Reconnect delay after a failed or lost connection: 5 s, 10, 20, 40, then 60 s.
inline uint32_t mqttBackoffMs(unsigned attempt) {
  uint32_t ms = 5000u << (attempt > 4 ? 4 : attempt);
  return ms > 60000u ? 60000u : ms;
}

// What a device's published state is made of. Two equal signatures mean nothing new to publish.
struct MqttSig {
  bool on = false, ok = false, tripped = false;
  uint8_t duty = 0;
  int16_t val = 0;
  uint32_t edges = 0, longs = 0;
  int32_t t10 = 0, h10 = 0, p10 = 0;  // sensor readings in tenths: noise below that is not worth a message
};
inline MqttSig mqttSig(bool on, uint8_t duty, int16_t val, uint32_t edges, uint32_t longs, bool ok, bool tripped,
                       float temp, float hum, float press) {
  MqttSig s;
  s.on = on; s.duty = duty; s.val = val; s.edges = edges; s.longs = longs; s.ok = ok; s.tripped = tripped;
  s.t10 = int32_t(lroundf(temp * 10.0f));
  s.h10 = int32_t(lroundf(hum * 10.0f));
  s.p10 = int32_t(lroundf(press * 10.0f));
  return s;
}
inline bool mqttSigSame(const MqttSig& a, const MqttSig& b) {
  return a.on == b.on && a.ok == b.ok && a.tripped == b.tripped && a.duty == b.duty && a.val == b.val &&
         a.edges == b.edges && a.longs == b.longs && a.t10 == b.t10 && a.h10 == b.h10 && a.p10 == b.p10;
}

}  // namespace espwf
