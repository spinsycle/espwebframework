#pragma once
// The firmware-wide system event log. Safe to call from loop() and from web callbacks.
#include <ArduinoJson.h>

#include "core/EventRing.h"

namespace espwf {

// kind: a short code ("boot", "failsafe", "login_failed"...). text: one line, cut to fit.
void logEvent(const char* kind, const char* text);
void logEventf(const char* kind, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

size_t eventCount();
uint32_t eventTotal();
bool eventNewest(size_t i, SysEvent& out);  // 0 = newest
void eventToJson(const SysEvent& e, JsonObject out);

}  // namespace espwf
