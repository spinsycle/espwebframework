#pragma once
// Build-time feature switches. The ESP8266 has about 13 KB of free heap, so what does not fit is compiled out.
//
// ESPWF_HAS_ROLE: master/slave (a slave sends its button presses to another board's REST API). ESP32 family only.
// An ESP8266 board always behaves as a master and has no /api/v1/role route.
#if !defined(ESP8266)
#define ESPWF_HAS_ROLE 1
#else
#define ESPWF_HAS_ROLE 0
#endif
