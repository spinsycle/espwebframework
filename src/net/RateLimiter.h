#pragma once
// Fixed-window rate limiter, one per WebSocket client. Pure C++ (native-tested).
#include <stdint.h>

namespace espwf {

struct RateLimiter {
  uint32_t windowStart = 0;
  uint16_t count = 0;

  // True if one more event fits in the current window of `windowMs`; false = drop it.
  bool allow(uint32_t now, uint16_t maxPerWindow, uint32_t windowMs) {
    if (uint32_t(now - windowStart) >= windowMs) { windowStart = now; count = 0; }
    if (count >= maxPerWindow) return false;
    count++;
    return true;
  }
};

}  // namespace espwf
