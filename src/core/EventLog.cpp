#include "core/EventLog.h"

#include <Arduino.h>
#include <stdarg.h>

#include "hal/Hal.h"

namespace espwf {

static EventRing gRing;

static uint32_t wallClock() {
  uint32_t t = hal::epochUtc();
  return t > 1600000000u ? t : 0;  // below that the clock has not been set yet
}

void logEvent(const char* kind, const char* text) {
  uint32_t up = millis() / 1000, ep = wallClock();
  hal::CriticalSection cs;
  gRing.add(up, ep, kind, text);
}

void logEventf(const char* kind, const char* fmt, ...) {
  char buf[kEventTextCap + 16];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  logEvent(kind, buf);
}

size_t eventCount() {
  hal::CriticalSection cs;
  return gRing.size();
}
uint32_t eventTotal() {
  hal::CriticalSection cs;
  return gRing.total();
}
bool eventNewest(size_t i, SysEvent& out) {
  hal::CriticalSection cs;
  return gRing.newest(i, out);
}

void eventToJson(const SysEvent& e, JsonObject out) {
  out["up_s"] = e.upS;
  if (e.epoch) out["time"] = e.epoch;
  out["kind"] = e.kind;
  out["text"] = e.text;
}

}  // namespace espwf
