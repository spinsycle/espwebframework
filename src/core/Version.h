#pragma once
// Framework version. Bump with CHANGELOG.md and library.json together.
namespace espwf {
constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;
constexpr int kVersionPatch = 1;

// Writes "major.minor.patch" into out; returns false if it does not fit.
inline bool versionString(char* out, unsigned size) {
  if (!out || size < 6) return false;
  unsigned n = 0;
  auto put = [&](int v) {
    char tmp[6]; int t = 0;
    do { tmp[t++] = char('0' + v % 10); v /= 10; } while (v && t < 5);
    while (t) { if (n + 1 >= size) return false; out[n++] = tmp[--t]; }
    return true;
  };
  if (!put(kVersionMajor)) return false;
  if (n + 1 >= size) return false;
  out[n++] = '.';
  if (!put(kVersionMinor)) return false;
  if (n + 1 >= size) return false;
  out[n++] = '.';
  if (!put(kVersionPatch)) return false;
  out[n] = 0;
  return true;
}
}  // namespace espwf
