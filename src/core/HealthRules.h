#pragma once
// What the reset reason string means. Pure C++, unit-tested natively. Reasons differ per chip, so this matches words.
#include <ctype.h>
#include <string.h>

namespace espwf {

inline bool reasonHas(const char* reason, const char* word) {
  if (!reason || !word || !*word) return false;
  size_t n = strlen(word);
  for (const char* p = reason; *p; p++) {
    size_t k = 0;
    while (k < n && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)word[k])) k++;
    if (k == n) return true;
  }
  return false;
}

// The watchdog bit the board: the main loop (or a task) did not come back in time.
inline bool isWatchdogReason(const char* reason) { return reasonHas(reason, "watchdog") || reasonHas(reason, "wdt"); }

// Anything that is the firmware's fault rather than power-up, a button, or a deliberate restart.
inline bool isFaultReason(const char* reason) {
  return isWatchdogReason(reason) || reasonHas(reason, "exception") || reasonHas(reason, "panic") || reasonHas(reason, "brownout");
}

}  // namespace espwf
