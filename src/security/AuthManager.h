#pragma once
// Firmware side of the login: loads and saves the password hash (/config/security.json), takes randomness
// and time from the chip, and wraps AuthCore. Used only from the web server's task, so it needs no locking.
#include "AuthCore.h"

namespace espwf {

class AuthManager {
 public:
  void begin();  // reads the saved password; none saved = the factory password, which must be changed

  // A correct password returns Ok and writes the session token (kTokenChars + NUL) to `token`.
  AuthErr login(const char* password, char* token);
  bool valid(const char* token);  // renews the session
  bool alive(const char* token) const;  // does not
  void logout(const char* token);

  // Checks the current password, then saves the new one. Every session except `keepToken` ends.
  AuthErr changePassword(const char* current, const char* next, const char* keepToken);

  bool mustChange() const { return core_.mustChange(); }
  uint32_t lockedForMs() const;
  size_t sessions() const { return core_.sessionCount(); }

 private:
  bool save() const;
  AuthCore core_;
};

}  // namespace espwf
