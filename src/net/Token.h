#pragma once
// The session token a request carries: "Authorization: Bearer <token>" (scripts) or the "sid" cookie (the browser).
#include <ESPAsyncWebServer.h>

#include "security/AuthCore.h"

namespace espwf {

inline bool tokenFrom(AsyncWebServerRequest* r, char* out) {  // out holds kTokenChars + 1
  out[0] = 0;
  const AsyncWebHeader* h = r->getHeader("Authorization");
  if (h) {
    String v = h->value();
    if (v.startsWith("Bearer ") && v.length() == 7 + kTokenChars) {
      v.substring(7).toCharArray(out, kTokenChars + 1);
      return true;
    }
  }
  h = r->getHeader("Cookie");
  if (h) {
    String v = h->value();
    int i = v.indexOf("sid=");
    while (i > 0 && v[i - 1] != ' ' && v[i - 1] != ';') i = v.indexOf("sid=", i + 1);  // "sid=" must start a cookie
    if (i >= 0 && v.length() >= size_t(i) + 4 + kTokenChars) {
      v.substring(i + 4, i + 4 + kTokenChars).toCharArray(out, kTokenChars + 1);
      return true;
    }
  }
  return false;
}

}  // namespace espwf
