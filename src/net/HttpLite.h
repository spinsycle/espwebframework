#pragma once
// Just enough HTTP/1.1 for one board to call another board's REST API: build a request, read the status code,
// pull one string out of a small JSON reply. Pure C++ (no Arduino) so it is unit-tested natively.
#include <stdio.h>
#include <string.h>

namespace espwf {

// "<METHOD> <path> HTTP/1.1" + headers + body. Returns the length, or 0 if it does not fit in `cap`.
// bearer / body may be null. Connection: close, so the other side ends each exchange itself.
inline size_t httpBuildRequest(char* out, size_t cap, const char* method, const char* path, const char* host,
                               const char* bearer, const char* body) {
  size_t bl = body ? strlen(body) : 0;
  int n = snprintf(out, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method, path, host);
  if (n < 0 || size_t(n) >= cap) return 0;
  size_t o = size_t(n);
  if (bearer) {
    n = snprintf(out + o, cap - o, "Authorization: Bearer %s\r\n", bearer);
    if (n < 0 || size_t(n) >= cap - o) return 0;
    o += size_t(n);
  }
  if (body) {
    n = snprintf(out + o, cap - o, "Content-Type: application/json\r\nContent-Length: %u\r\n", unsigned(bl));
    if (n < 0 || size_t(n) >= cap - o) return 0;
    o += size_t(n);
  }
  if (cap - o < 3 + bl) return 0;
  memcpy(out + o, "\r\n", 2);
  o += 2;
  if (bl) { memcpy(out + o, body, bl); o += bl; }
  out[o] = 0;
  return o;
}

// "HTTP/1.1 200 OK" -> 200. -1 until a whole status line has arrived or if it is not one.
inline int httpStatus(const char* buf, size_t len) {
  if (len < 12 || strncmp(buf, "HTTP/1.", 7) != 0 || (buf[7] != '0' && buf[7] != '1') || buf[8] != ' ') return -1;
  int v = 0;
  for (int i = 9; i < 12; i++) {
    if (buf[i] < '0' || buf[i] > '9') return -1;
    v = v * 10 + (buf[i] - '0');
  }
  return v;
}

// Finds  "key":"value"  (a space after the colon is fine) anywhere in buf. Only plain values: it stops at the
// closing quote and refuses a backslash. False if absent, unfinished or longer than cap-1.
inline bool httpJsonString(const char* buf, size_t len, const char* key, char* out, size_t cap) {
  char pat[24];
  int pn = snprintf(pat, sizeof pat, "\"%s\":", key);
  if (pn < 0 || size_t(pn) >= sizeof pat) return false;
  for (size_t i = 0; i + size_t(pn) <= len; i++) {
    if (memcmp(buf + i, pat, size_t(pn)) != 0) continue;
    size_t j = i + size_t(pn);
    while (j < len && buf[j] == ' ') j++;
    if (j >= len || buf[j] != '"') return false;
    j++;
    size_t k = 0;
    while (j < len && buf[j] != '"') {
      if (buf[j] == '\\' || k + 1 >= cap) return false;
      out[k++] = buf[j++];
    }
    if (j >= len) return false;  // the closing quote has not arrived yet
    out[k] = 0;
    return true;
  }
  return false;
}

}  // namespace espwf
