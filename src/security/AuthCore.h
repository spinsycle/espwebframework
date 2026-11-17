#pragma once
// The login logic: salted password hash, sessions, and the lock-out after repeated wrong passwords.
// Pure C++ (no Arduino): time and random bytes are passed in, so the host tests are deterministic.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Sha256.h"

namespace espwf {

constexpr const char* kDefaultPassword = "espframework";  // until the first change; the UI forces that change
constexpr size_t kMinPassword = 8;
constexpr size_t kMaxPassword = 32;
constexpr size_t kSaltBytes = 16;
constexpr size_t kTokenBytes = 16;
constexpr size_t kTokenChars = kTokenBytes * 2;  // hex
constexpr size_t kMaxSessions = 3;
constexpr uint16_t kHashRounds = 500;            // each round is one SHA-256; about 0.2 s on an ESP8266
constexpr uint32_t kSessionIdleMs = 3600UL * 1000;
constexpr uint32_t kLockMs = 30UL * 1000;
constexpr uint8_t kMaxFails = 5;

enum class AuthErr : uint8_t { Ok = 0, Wrong, Locked, Policy, SameAsDefault };

inline const char* authErrCode(AuthErr e) {
  switch (e) {
    case AuthErr::Ok: return "";
    case AuthErr::Wrong: return "bad_credentials";
    case AuthErr::Locked: return "locked";
    case AuthErr::Policy: return "weak_password";
    case AuthErr::SameAsDefault: return "default_password";
  }
  return "";
}

// Equal bytes in the same time whatever they hold (a wrong guess must not be faster to reject than a close one).
inline bool constTimeEq(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= uint8_t(a[i] ^ b[i]);
  return d == 0;
}

inline void toHex(const uint8_t* in, size_t n, char* out) {
  static const char* h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = h[in[i] >> 4]; out[2 * i + 1] = h[in[i] & 15]; }
  out[2 * n] = 0;
}

inline bool fromHex(const char* s, uint8_t* out, size_t n) {
  if (strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      char c = s[2 * i + k];
      int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c + 10 - 'A' : -1;
      if (d < 0) return false;
      v = v * 16 + d;
    }
    out[i] = uint8_t(v);
  }
  return true;
}

// 8-32 printable ASCII characters, and not the factory password.
inline AuthErr passwordPolicy(const char* pw) {
  size_t n = strnlen(pw, kMaxPassword + 1);
  if (n < kMinPassword || n > kMaxPassword) return AuthErr::Policy;
  for (size_t i = 0; i < n; i++)
    if (uint8_t(pw[i]) < 0x20 || uint8_t(pw[i]) > 0x7E) return AuthErr::Policy;
  if (!strcmp(pw, kDefaultPassword)) return AuthErr::SameAsDefault;
  return AuthErr::Ok;
}

// h0 = SHA256(salt || pw); h(n+1) = SHA256(h(n) || salt || pw), `rounds` times.
inline void hashPassword(const char* pw, const uint8_t salt[kSaltBytes], uint16_t rounds, uint8_t out[32]) {
  size_t pl = strnlen(pw, kMaxPassword + 1);
  if (pl > kMaxPassword) pl = kMaxPassword;
  Sha256 s;
  s.update(salt, kSaltBytes);
  s.update(reinterpret_cast<const uint8_t*>(pw), pl);
  s.final(out);
  for (uint16_t i = 0; i < rounds; i++) {
    s.update(out, 32);
    s.update(salt, kSaltBytes);
    s.update(reinterpret_cast<const uint8_t*>(pw), pl);
    s.final(out);
  }
}

class AuthCore {
 public:
  // ---- the stored password

  bool hasCustomPassword() const { return custom_; }
  bool mustChange() const { return !custom_; }  // the factory password is in use

  // For loading: the salt, hash and rounds exactly as they were saved.
  void loadStored(const uint8_t salt[kSaltBytes], const uint8_t hash[32], uint16_t rounds) {
    memcpy(salt_, salt, kSaltBytes);
    memcpy(hash_, hash, 32);
    rounds_ = rounds;
    custom_ = true;
  }
  void getStored(uint8_t salt[kSaltBytes], uint8_t hash[32], uint16_t& rounds) const {
    memcpy(salt, salt_, kSaltBytes);
    memcpy(hash, hash_, 32);
    rounds = rounds_;
  }

  AuthErr setPassword(const char* pw, const uint8_t salt[kSaltBytes]) {
    AuthErr e = passwordPolicy(pw);
    if (e != AuthErr::Ok) return e;
    memcpy(salt_, salt, kSaltBytes);
    rounds_ = kHashRounds;
    hashPassword(pw, salt_, rounds_, hash_);
    custom_ = true;
    return AuthErr::Ok;
  }

  bool passwordOk(const char* pw) const {
    if (!custom_) {
      size_t n = strlen(kDefaultPassword);
      return strlen(pw) == n && constTimeEq(reinterpret_cast<const uint8_t*>(pw), reinterpret_cast<const uint8_t*>(kDefaultPassword), n);
    }
    uint8_t h[32];
    hashPassword(pw, salt_, rounds_, h);
    return constTimeEq(h, hash_, 32);
  }

  // ---- sessions and lock-out

  // 0 if a login attempt is allowed now, otherwise how many ms of lock-out are left.
  uint32_t lockedForMs(uint32_t now) const {
    if (!locked_) return 0;
    int32_t left = int32_t(lockUntil_ - now);
    return left > 0 ? uint32_t(left) : 0;
  }

  // A correct password makes a session and writes its token (kTokenChars + NUL) to `token`.
  AuthErr login(const char* pw, uint32_t now, const uint8_t rnd[kTokenBytes], char* token) {
    if (lockedForMs(now)) return AuthErr::Locked;
    locked_ = false;
    if (!passwordOk(pw)) {
      if (++fails_ >= kMaxFails) {
        fails_ = 0;
        locked_ = true;
        lockUntil_ = now + kLockMs;
      }
      return AuthErr::Wrong;
    }
    fails_ = 0;
    size_t slot = 0;  // a free slot, else the one idle the longest
    bool found = false;
    for (size_t i = 0; i < kMaxSessions; i++)
      if (!s_[i].used) { slot = i; found = true; break; }
    if (!found)
      for (size_t i = 1; i < kMaxSessions; i++)
        if (int32_t(s_[slot].last - s_[i].last) > 0) slot = i;
    s_[slot].used = true;
    s_[slot].last = now;
    memcpy(s_[slot].tok, rnd, kTokenBytes);
    toHex(rnd, kTokenBytes, token);
    return AuthErr::Ok;
  }

  // Is this token a live session? A good one is renewed; one idle for an hour is forgotten.
  bool valid(const char* token, uint32_t now) {
    uint8_t t[kTokenBytes];
    if (!token || !fromHex(token, t, kTokenBytes)) return false;
    bool ok = false;
    for (size_t i = 0; i < kMaxSessions; i++) {  // every slot is compared, so the time does not show which one matched
      if (!s_[i].used) continue;
      bool match = constTimeEq(t, s_[i].tok, kTokenBytes);
      if (int32_t(now - s_[i].last) > int32_t(kSessionIdleMs)) { s_[i].used = false; continue; }
      if (match) { s_[i].last = now; ok = true; }
    }
    return ok;
  }

  // Is it still a live session? Unlike valid() this does not renew it (the WebSocket checks this every few seconds).
  bool alive(const char* token, uint32_t now) const {
    uint8_t t[kTokenBytes];
    if (!token || !fromHex(token, t, kTokenBytes)) return false;
    bool ok = false;
    for (size_t i = 0; i < kMaxSessions; i++) {
      if (!s_[i].used) continue;
      bool match = constTimeEq(t, s_[i].tok, kTokenBytes);
      if (match && int32_t(now - s_[i].last) <= int32_t(kSessionIdleMs)) ok = true;
    }
    return ok;
  }

  void logout(const char* token) {
    uint8_t t[kTokenBytes];
    if (!token || !fromHex(token, t, kTokenBytes)) return;
    for (size_t i = 0; i < kMaxSessions; i++)
      if (s_[i].used && constTimeEq(t, s_[i].tok, kTokenBytes)) s_[i].used = false;
  }

  // After a password change every other session ends.
  void endAllExcept(const char* keep) {
    uint8_t t[kTokenBytes];
    bool have = keep && fromHex(keep, t, kTokenBytes);
    for (size_t i = 0; i < kMaxSessions; i++)
      if (s_[i].used && !(have && constTimeEq(t, s_[i].tok, kTokenBytes))) s_[i].used = false;
  }

  size_t sessionCount() const {
    size_t n = 0;
    for (size_t i = 0; i < kMaxSessions; i++) n += s_[i].used ? 1 : 0;
    return n;
  }

 private:
  struct Sess {
    bool used = false;
    uint32_t last = 0;
    uint8_t tok[kTokenBytes] = {0};
  };
  Sess s_[kMaxSessions];
  uint8_t salt_[kSaltBytes] = {0};
  uint8_t hash_[32] = {0};
  uint16_t rounds_ = 0;
  bool custom_ = false;
  uint8_t fails_ = 0;
  bool locked_ = false;
  uint32_t lockUntil_ = 0;
};

}  // namespace espwf
