#include <unity.h>

#include "devices/OpQueue.h"
#include "net/RateLimiter.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

#define EQ(a, b) TEST_ASSERT_EQUAL(int(a), int(b))

struct Item { int key; int val; };
static bool sameKey(const Item& a, int key) { return a.key == key; }

// ---- OpQueue

void test_queue_fifo_and_capacity() {
  OpQueue<int, 4> q;  // usable depth 3
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_TRUE(q.push(1));
  TEST_ASSERT_TRUE(q.push(2));
  TEST_ASSERT_TRUE(q.push(3));
  TEST_ASSERT_FALSE(q.push(4));  // full
  EQ(3, q.size());
  int v = 0;
  TEST_ASSERT_TRUE(q.pop(v)); EQ(1, v);
  TEST_ASSERT_TRUE(q.pop(v)); EQ(2, v);
  TEST_ASSERT_TRUE(q.push(5));   // wraps around the end of the array
  TEST_ASSERT_TRUE(q.push(6));
  TEST_ASSERT_TRUE(q.pop(v)); EQ(3, v);
  TEST_ASSERT_TRUE(q.pop(v)); EQ(5, v);
  TEST_ASSERT_TRUE(q.pop(v)); EQ(6, v);
  TEST_ASSERT_FALSE(q.pop(v));
  TEST_ASSERT_TRUE(q.empty());
}

void test_queue_replace_newest_matching() {
  OpQueue<Item, 8> q;
  q.push({1, 10});
  q.push({2, 20});
  q.push({1, 11});
  Item n = {1, 99};
  TEST_ASSERT_TRUE(q.replaceNewestIf(n, [](const Item& i) { return sameKey(i, 1); }));
  EQ(3, q.size());  // replaced, not added
  Item o;
  q.pop(o); EQ(10, o.val);   // the older matching item is untouched
  q.pop(o); EQ(20, o.val);
  q.pop(o); EQ(99, o.val);   // the newest one took the new value, order kept
}

void test_queue_replace_no_match_and_when_full() {
  OpQueue<Item, 3> q;  // usable depth 2
  TEST_ASSERT_FALSE(q.replaceNewestIf({1, 1}, [](const Item& i) { return sameKey(i, 1); }));  // empty
  q.push({1, 10});
  q.push({2, 20});
  TEST_ASSERT_FALSE(q.push({3, 30}));  // full
  TEST_ASSERT_FALSE(q.replaceNewestIf({9, 9}, [](const Item& i) { return sameKey(i, 9); }));
  TEST_ASSERT_TRUE(q.replaceNewestIf({1, 77}, [](const Item& i) { return sameKey(i, 1); }));  // still works when full
  Item o;
  q.pop(o); EQ(77, o.val);
}

void test_queue_replace_after_wrap() {
  OpQueue<Item, 4> q;
  Item o;
  q.push({1, 1}); q.push({2, 2}); q.pop(o); q.pop(o);   // head/tail now mid-array
  q.push({3, 3}); q.push({4, 4}); q.push({3, 5});       // wraps
  TEST_ASSERT_TRUE(q.replaceNewestIf({3, 50}, [](const Item& i) { return sameKey(i, 3); }));
  q.pop(o); EQ(3, o.val);
  q.pop(o); EQ(4, o.val);
  q.pop(o); EQ(50, o.val);
}

void test_queue_size_after_wrap_non_power_of_two() {
  OpQueue<int, 3> q;  // 3 is not a power of two, so the unsigned wrap shows up in size()
  int v;
  q.push(1); q.push(2); q.pop(v); q.pop(v);
  q.push(3); q.push(4);
  EQ(2, q.size());
}

// ---- RateLimiter

void test_limiter_window() {
  RateLimiter r;
  for (int i = 0; i < 3; i++) TEST_ASSERT_TRUE(r.allow(1000, 3, 1000));
  TEST_ASSERT_FALSE(r.allow(1500, 3, 1000));  // 4th inside the window
  TEST_ASSERT_FALSE(r.allow(1999, 3, 1000));
  TEST_ASSERT_TRUE(r.allow(2000, 3, 1000));   // new window
  TEST_ASSERT_TRUE(r.allow(2001, 3, 1000));
  TEST_ASSERT_TRUE(r.allow(2002, 3, 1000));
  TEST_ASSERT_FALSE(r.allow(2003, 3, 1000));
}

void test_limiter_wraparound() {
  RateLimiter r;
  TEST_ASSERT_TRUE(r.allow(0xFFFFFF00u, 1, 1000));
  TEST_ASSERT_FALSE(r.allow(0xFFFFFFFFu, 1, 1000));
  TEST_ASSERT_TRUE(r.allow(0x400u, 1, 1000));  // millis() wrapped, >1000 ms later
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_queue_fifo_and_capacity);
  RUN_TEST(test_queue_replace_newest_matching);
  RUN_TEST(test_queue_replace_no_match_and_when_full);
  RUN_TEST(test_queue_replace_after_wrap);
  RUN_TEST(test_queue_size_after_wrap_non_power_of_two);
  RUN_TEST(test_limiter_window);
  RUN_TEST(test_limiter_wraparound);
  return UNITY_END();
}
