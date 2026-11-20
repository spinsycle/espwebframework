#include <unity.h>
#include <string.h>
#include "core/Version.h"

void setUp() {}
void tearDown() {}

void test_version_string() {
  char buf[16];
  TEST_ASSERT_TRUE(espwf::versionString(buf, sizeof buf));
  TEST_ASSERT_EQUAL_STRING("1.0.1", buf);
}

void test_version_string_too_small() {
  char buf[5];
  TEST_ASSERT_FALSE(espwf::versionString(buf, sizeof buf));
  TEST_ASSERT_FALSE(espwf::versionString(nullptr, 16));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_version_string);
  RUN_TEST(test_version_string_too_small);
  return UNITY_END();
}
