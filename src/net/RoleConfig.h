#pragma once
// Board role (master or slave), the slave's button -> remote relay links, and the pure logic around them.
// Pure C++ (no Arduino) so it is unit-tested natively.
//
// A master is any board running as before: it owns the relays and answers the REST API.
// A slave has buttons (input devices) and sends commands to a master's REST API when they are pressed.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "MqttConfig.h"  // host validity check
#include "devices/DeviceTypes.h"
#include "devices/OpQueue.h"

namespace espwf {

enum class Role : uint8_t { Master = 0, Slave = 1 };
enum class LinkTrig : uint8_t { Press = 0, Long = 1, Release = 2 };
enum class LinkAct : uint8_t { Toggle = 0, On = 1, Off = 2, Set = 3 };

constexpr size_t kMaxLinks = 8;
constexpr unsigned kRoleHostCap = 65;
constexpr unsigned kRolePassCap = 65;
constexpr uint32_t kRemoteMaxAgeMs = 10000;  // a command older than this is dropped, never sent late
constexpr size_t kRemoteQueue = 5;           // usable depth 4

inline const char* linkTrigName(LinkTrig t) { return t == LinkTrig::Press ? "press" : t == LinkTrig::Long ? "long" : "release"; }
inline const char* linkActName(LinkAct a) { return a == LinkAct::Toggle ? "toggle" : a == LinkAct::On ? "on" : a == LinkAct::Off ? "off" : "set"; }
inline bool parseLinkTrig(const char* s, LinkTrig& o) {
  if (!s) return false;
  if (!strcmp(s, "press")) { o = LinkTrig::Press; return true; }
  if (!strcmp(s, "long")) { o = LinkTrig::Long; return true; }
  if (!strcmp(s, "release")) { o = LinkTrig::Release; return true; }
  return false;
}
inline bool parseLinkAct(const char* s, LinkAct& o) {
  if (!s) return false;
  if (!strcmp(s, "toggle")) { o = LinkAct::Toggle; return true; }
  if (!strcmp(s, "on")) { o = LinkAct::On; return true; }
  if (!strcmp(s, "off")) { o = LinkAct::Off; return true; }
  if (!strcmp(s, "set")) { o = LinkAct::Set; return true; }
  return false;
}

struct LinkRule {
  char input[kIdCap] = {0};   // local input device (the button)
  LinkTrig trig = LinkTrig::Press;
  char remote[kIdCap] = {0};  // device id on the master (the relay)
  LinkAct act = LinkAct::Toggle;
  int16_t value = 0;          // act == Set: PWM percent, motor speed or servo angle
};

struct RoleConfig {
  Role role = Role::Master;
  char host[kRoleHostCap] = {0};  // the master: an IP address or a DNS name (a .local name does not resolve on the ESP8266)
  uint16_t port = 80;
  char pass[kRolePassCap] = {0};  // the master's admin password
  LinkRule links[kMaxLinks];
  uint8_t nLinks = 0;
};

enum class RoleCfgError : uint8_t { Ok = 0, Role, Host, Port, Password, Link, Format };

inline const char* roleCfgErrorField(RoleCfgError e) {
  switch (e) {
    case RoleCfgError::Ok: return "";
    case RoleCfgError::Role: return "role";
    case RoleCfgError::Host: return "host";
    case RoleCfgError::Port: return "port";
    case RoleCfgError::Password: return "password";
    case RoleCfgError::Link: return "links";
    default: return "format";
  }
}

// On error, *badLink (if given) is the index of the offending link, or -1 when it is not a link problem.
inline RoleCfgError validateRoleConfig(const RoleConfig& c, int* badLink = nullptr) {
  using namespace mqttdetail;
  if (badLink) *badLink = -1;
  if (uint8_t(c.role) > uint8_t(Role::Slave)) return RoleCfgError::Role;
  if (!terminated(c.host, kRoleHostCap)) return RoleCfgError::Host;
  if (!terminated(c.pass, kRolePassCap)) return RoleCfgError::Password;
  if ((c.role == Role::Slave || c.host[0]) && !validHost(c.host)) return RoleCfgError::Host;
  if (c.port < 1) return RoleCfgError::Port;
  if (c.role == Role::Slave && c.pass[0] == 0) return RoleCfgError::Password;
  if (c.nLinks > kMaxLinks) return RoleCfgError::Link;
  for (size_t i = 0; i < c.nLinks; i++) {
    const LinkRule& l = c.links[i];
    bool bad = !terminated(l.input, kIdCap) || !detail::validDeviceId(l.input) || !terminated(l.remote, kIdCap) ||
               !detail::validDeviceId(l.remote) || uint8_t(l.trig) > uint8_t(LinkTrig::Release) ||
               uint8_t(l.act) > uint8_t(LinkAct::Set) ||
               (l.act == LinkAct::Set && (l.value < -100 || l.value > 180));
    for (size_t j = 0; j < i && !bad; j++)  // the same button and trigger twice is ambiguous
      bad = !strcmp(c.links[j].input, l.input) && c.links[j].trig == l.trig;
    if (bad) { if (badLink) *badLink = int(i); return RoleCfgError::Link; }
  }
  return RoleCfgError::Ok;
}

// ---- button events ----------------------------------------------------------------------------

enum : uint8_t { kEvPress = 1, kEvLong = 2, kEvRelease = 4 };

struct LinkState {
  bool primed = false;
  bool active = false;
  uint32_t longs = 0;
};

// Compares the button's current reading with the last one. The first reading only primes the state, so a
// button already held at boot does not fire. Returns a mask of kEv* bits.
inline uint8_t linkEvents(LinkState& s, bool active, uint32_t longs) {
  uint8_t ev = 0;
  if (s.primed) {
    if (!s.active && active) ev |= kEvPress;
    if (s.active && !active) ev |= kEvRelease;
    if (longs != s.longs) ev |= kEvLong;
  }
  s.primed = true;
  s.active = active;
  s.longs = longs;
  return ev;
}

inline bool linkFires(const LinkRule& l, uint8_t ev) {
  uint8_t bit = l.trig == LinkTrig::Press ? kEvPress : l.trig == LinkTrig::Long ? kEvLong : kEvRelease;
  return (ev & bit) != 0;
}

// ---- remote command queue ---------------------------------------------------------------------

struct RemoteCmd {
  char id[kIdCap] = {0};
  LinkAct act = LinkAct::Toggle;
  int16_t value = 0;
  uint32_t born = 0;  // millis() when the button was pressed
};

// A toggle always queues (two presses are two toggles). On / off / set replace an older waiting on / off / set
// for the same remote device: only the latest matters. False if the queue is full.
inline bool remoteEnqueue(OpQueue<RemoteCmd, kRemoteQueue>& q, const RemoteCmd& c) {
  if (c.act != LinkAct::Toggle) {
    bool merged = q.replaceNewestIf(c, [&c](const RemoteCmd& w) { return w.act != LinkAct::Toggle && !strcmp(w.id, c.id); });
    if (merged) return true;
  }
  return q.push(c);
}

inline bool remoteExpired(const RemoteCmd& c, uint32_t now) { return uint32_t(now - c.born) > kRemoteMaxAgeMs; }

// The JSON body for POST /api/v1/command on the master. False if it does not fit.
inline bool remoteBody(char* out, size_t cap, const RemoteCmd& c) {
  int n;
  switch (c.act) {
    case LinkAct::Toggle: n = snprintf(out, cap, "{\"id\":\"%s\",\"action\":\"toggle\"}", c.id); break;
    case LinkAct::On: n = snprintf(out, cap, "{\"id\":\"%s\",\"action\":\"set\",\"value\":true}", c.id); break;
    case LinkAct::Off: n = snprintf(out, cap, "{\"id\":\"%s\",\"action\":\"set\",\"value\":false}", c.id); break;
    default: n = snprintf(out, cap, "{\"id\":\"%s\",\"action\":\"set\",\"value\":%d}", c.id, int(c.value)); break;
  }
  return n > 0 && size_t(n) < cap;
}

// What to do with the master's answer to a command.
enum class CmdReply : uint8_t { Done, Retry, Relogin, Rejected, Locked, DefaultPassword };
inline CmdReply classifyCmdReply(int status, bool passwordChangeRequired) {
  if (status >= 200 && status < 300) return CmdReply::Done;
  if (status == 401) return CmdReply::Relogin;
  if (status == 403) return passwordChangeRequired ? CmdReply::DefaultPassword : CmdReply::Rejected;
  if (status == 429) return CmdReply::Locked;
  if (status == 503 || status < 0) return CmdReply::Retry;  // master busy, or no usable answer
  return CmdReply::Rejected;                                // 400 / 404 / 409: retrying cannot help
}

// Delay before the next try after a failure: 1 s, 2, 4, 8, 16, then 30 s.
inline uint32_t roleBackoffMs(unsigned attempt) {
  uint32_t ms = 1000u << (attempt > 5 ? 5 : attempt);
  return ms > 30000u ? 30000u : ms;
}

}  // namespace espwf
