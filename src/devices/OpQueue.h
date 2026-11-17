#pragma once
// Fixed-size FIFO between the async server (producer) and loop() (consumer). Pure C++, no locking:
// the caller wraps push/pop/replaceIf in a critical section. One slot stays empty, so usable depth is N-1.
#include <stddef.h>

namespace espwf {

template <typename T, size_t N>
class OpQueue {
 public:
  bool push(const T& v) {
    size_t next = (head_ + 1) % N;
    if (next == tail_) return false;
    items_[head_] = v;
    head_ = next;
    return true;
  }

  bool pop(T& out) {
    if (tail_ == head_) return false;
    out = items_[tail_];
    tail_ = (tail_ + 1) % N;
    return true;
  }

  // Overwrites the newest queued item for which same(item) is true. Used to coalesce a flood of
  // slider values to the same device into the latest one, so a drag never fills the queue.
  template <typename Pred>
  bool replaceNewestIf(const T& v, Pred same) {
    size_t i = head_;
    while (i != tail_) {
      i = (i + N - 1) % N;
      if (same(items_[i])) { items_[i] = v; return true; }
    }
    return false;
  }

  size_t size() const { return (head_ + N - tail_) % N; }
  bool empty() const { return head_ == tail_; }

 private:
  T items_[N];
  size_t head_ = 0, tail_ = 0;
};

}  // namespace espwf
