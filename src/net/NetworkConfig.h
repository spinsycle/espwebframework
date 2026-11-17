#pragma once
// Network settings + validation. Pure C++ (no Arduino) so it is unit-tested natively.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace espwf {

enum class NetMode : uint8_t { AP = 0, STA = 1, AP_STA = 2 };

inline const char* netModeName(NetMode m) {
  switch (m) {
    case NetMode::AP: return "ap";
    case NetMode::STA: return "sta";
    default: return "ap_sta";
  }
}

inline bool parseNetMode(const char* s, NetMode& out) {
  if (!s) return false;
  if (!strcmp(s, "ap")) { out = NetMode::AP; return true; }
  if (!strcmp(s, "sta")) { out = NetMode::STA; return true; }
  if (!strcmp(s, "ap_sta")) { out = NetMode::AP_STA; return true; }
  return false;
}

// "a.b.c.d" -> 4 octets; rejects anything else.
inline bool parseIPv4(const char* s, uint8_t out[4]) {
  if (!s) return false;
  const char* p = s;
  for (int i = 0; i < 4; i++) {
    if (*p < '0' || *p > '9') return false;
    unsigned v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + unsigned(*p - '0');
      if (++digits > 3 || v > 255) return false;
      p++;
    }
    out[i] = uint8_t(v);
    if (i < 3) {
      if (*p != '.') return false;
      p++;
    }
  }
  return *p == 0;
}

constexpr unsigned kSsidCap = 33;   // 32 chars + NUL
constexpr unsigned kPassCap = 64;   // 63 chars + NUL
constexpr unsigned kHostCap = 33;

struct NetworkConfig {
  NetMode mode = NetMode::AP;
  char hostname[kHostCap] = {0};
  char apSsid[kSsidCap] = {0};
  char apPass[kPassCap] = {0};
  uint8_t apChannel = 1;
  uint8_t apIp[4] = {192, 168, 4, 1};
  char staSsid[kSsidCap] = {0};
  char staPass[kPassCap] = {0};
  bool staStatic = false;
  uint8_t staIp[4] = {0, 0, 0, 0};
  uint8_t staGateway[4] = {0, 0, 0, 0};
  uint8_t staMask[4] = {255, 255, 255, 0};
};

enum class NetCfgError : uint8_t {
  Ok = 0, Mode, Hostname, ApSsid, ApPass, ApChannel, ApIp,
  StaSsid, StaPass, StaIp, StaGateway, StaMask, Format
};

inline const char* netCfgErrorField(NetCfgError e) {
  switch (e) {
    case NetCfgError::Ok: return "";
    case NetCfgError::Mode: return "mode";
    case NetCfgError::Hostname: return "hostname";
    case NetCfgError::ApSsid: return "ap.ssid";
    case NetCfgError::ApPass: return "ap.password";
    case NetCfgError::ApChannel: return "ap.channel";
    case NetCfgError::ApIp: return "ap.ip";
    case NetCfgError::StaSsid: return "sta.ssid";
    case NetCfgError::StaPass: return "sta.password";
    case NetCfgError::StaIp: return "sta.ip";
    case NetCfgError::StaGateway: return "sta.gateway";
    case NetCfgError::StaMask: return "sta.mask";
    default: return "format";
  }
}

// Factory defaults. chipId makes SSID/hostname unique per board.
inline void setNetworkDefaults(NetworkConfig& c, uint32_t chipId) {
  c = NetworkConfig();
  snprintf(c.hostname, sizeof c.hostname, "espwf-%06x", (unsigned)(chipId & 0xFFFFFF));
  snprintf(c.apSsid, sizeof c.apSsid, "ESPWF-%06X", (unsigned)(chipId & 0xFFFFFF));
  // Documented default; P8 forces a change on first login.
  snprintf(c.apPass, sizeof c.apPass, "%s", "espframework");
}

namespace detail {
inline bool terminated(const char* s, unsigned cap) { return memchr(s, 0, cap) != nullptr; }
inline bool usableHostIp(const uint8_t ip[4]) {
  return ip[0] >= 1 && ip[0] <= 223 && ip[0] != 127 && ip[3] != 0 && ip[3] != 255;
}
inline bool validMask(const uint8_t m[4]) {
  uint32_t v = (uint32_t(m[0]) << 24) | (uint32_t(m[1]) << 16) | (uint32_t(m[2]) << 8) | m[3];
  if (v == 0 || v > 0xFFFFFFFCu) return false;  // /1 .. /30
  uint32_t inv = ~v;
  return (inv & (inv + 1)) == 0;                 // ones must be contiguous from the top
}
inline bool validHostname(const char* h) {
  size_t n = strlen(h);
  if (n < 1 || n > 32 || h[0] == '-' || h[n - 1] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    char c = h[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}
inline bool validPass(const char* p, bool allowEmpty) {
  size_t n = strlen(p);
  return (allowEmpty && n == 0) || (n >= 8 && n <= 63);
}
}  // namespace detail

inline NetCfgError validateNetworkConfig(const NetworkConfig& c) {
  using namespace detail;
  if (uint8_t(c.mode) > uint8_t(NetMode::AP_STA)) return NetCfgError::Mode;
  if (!terminated(c.hostname, kHostCap) || !validHostname(c.hostname)) return NetCfgError::Hostname;
  // The AP block is always valid: it doubles as the recovery access point.
  if (!terminated(c.apSsid, kSsidCap) || strlen(c.apSsid) < 1) return NetCfgError::ApSsid;
  if (!terminated(c.apPass, kPassCap) || !validPass(c.apPass, false)) return NetCfgError::ApPass;
  if (c.apChannel < 1 || c.apChannel > 13) return NetCfgError::ApChannel;
  if (!usableHostIp(c.apIp)) return NetCfgError::ApIp;
  if (!terminated(c.staSsid, kSsidCap)) return NetCfgError::StaSsid;
  if (!terminated(c.staPass, kPassCap) || !validPass(c.staPass, true)) return NetCfgError::StaPass;
  if (c.mode != NetMode::AP) {
    if (strlen(c.staSsid) < 1) return NetCfgError::StaSsid;
    if (c.staStatic) {
      if (!usableHostIp(c.staIp)) return NetCfgError::StaIp;
      if (!usableHostIp(c.staGateway)) return NetCfgError::StaGateway;
      if (!validMask(c.staMask)) return NetCfgError::StaMask;
    }
  }
  return NetCfgError::Ok;
}

}  // namespace espwf
