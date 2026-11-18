#pragma once
// The last few things that happened to the board (a failsafe fired, a login failed, it restarted), newest kept.
// Fixed size, no heap. Pure C++, unit-tested natively; core/EventLog.h wraps one in a lock for the whole firmware.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace espwf {

constexpr size_t kSysEventCap = 12;
constexpr size_t kEventKindCap = 14;  // 13 chars + NUL
constexpr size_t kEventTextCap = 32;  // 31 chars + NUL

struct SysEvent {
  uint32_t upS = 0;     // seconds since boot
  uint32_t epoch = 0;   // wall-clock seconds, 0 = the clock was not set yet
  char kind[kEventKindCap] = {0};
  char text[kEventTextCap] = {0};
};

class EventRing {
 public:
  void add(uint32_t upS, uint32_t epoch, const char* kind, const char* text) {
    SysEvent& e = e_[(head_ + count_) % kSysEventCap];
    if (count_ == kSysEventCap) head_ = uint8_t((head_ + 1) % kSysEventCap);  // full: the oldest goes
    else count_++;
    e.upS = upS;
    e.epoch = epoch;
    copy(e.kind, kind, kEventKindCap);
    copy(e.text, text, kEventTextCap);
    total_++;
  }
  size_t size() const { return count_; }
  uint32_t total() const { return total_; }  // events since boot, including those that scrolled out
  // 0 = the newest.
  bool newest(size_t i, SysEvent& out) const {
    if (i >= count_) return false;
    out = e_[(head_ + count_ - 1 - i) % kSysEventCap];
    return true;
  }

 private:
  static void copy(char* dst, const char* src, size_t cap) {
    size_t n = src ? strlen(src) : 0;
    if (n > cap - 1) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
  }
  SysEvent e_[kSysEventCap];
  uint8_t head_ = 0, count_ = 0;
  uint32_t total_ = 0;
};

}  // namespace espwf
