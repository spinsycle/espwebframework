#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "security/AuthCore.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

#define EQ(a, b) TEST_ASSERT_EQUAL(int(a), int(b))

static void hex(const uint8_t* d, char* out) { toHex(d, 32, out); }

static void sha(const char* s, char* outHex) {
  Sha256 h;
  uint8_t d[32];
  h.update(reinterpret_cast<const uint8_t*>(s), strlen(s));
  h.final(d);
  hex(d, outHex);
}

// ---- SHA-256 against the published test vectors

void test_sha256_vectors() {
  char o[65];
  sha("", o);
  TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", o);
  sha("abc", o);
  TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", o);
  sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", o);
  TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", o);
  sha("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", o);
  TEST_ASSERT_EQUAL_STRING("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", o);
}

void test_sha256_million_a() {
  Sha256 h;
  uint8_t block[1000], d[32];
  memset(block, 'a', sizeof block);
  for (int i = 0; i < 1000; i++) h.update(block, sizeof block);
  h.final(d);
  char o[65];
  hex(d, o);
  TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", o);
}

void test_sha256_split_updates_agree() {
  uint8_t data[200];
  for (size_t i = 0; i < sizeof data; i++) data[i] = uint8_t(i * 7 + 3);
  for (size_t n : {size_t(0), size_t(1), size_t(55), size_t(56), size_t(57), size_t(63), size_t(64), size_t(65), size_t(119), size_t(120), size_t(200)}) {
    Sha256 a;
    uint8_t da[32], db[32];
    a.update(data, n);
    a.final(da);
    for (size_t cut : {size_t(1), size_t(7), size_t(31), size_t(64)}) {
      Sha256 b;
      for (size_t pos = 0; pos < n; pos += cut) b.update(data + pos, (n - pos < cut) ? n - pos : cut);
      b.final(db);
      TEST_ASSERT_EQUAL_MEMORY(da, db, 32);                                // however the bytes arrive, the same digest
    }
  }
}

void test_sha256_reusable() {
  Sha256 h;
  uint8_t d1[32], d2[32];
  h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
  h.final(d1);
  h.update(reinterpret_cast<const uint8_t*>("abc"), 3);                     // final() resets it
  h.final(d2);
  TEST_ASSERT_EQUAL_MEMORY(d1, d2, 32);
}

// ---- hex, comparison, policy

void test_hex() {
  uint8_t in[4] = {0x00, 0xAB, 0x0F, 0xFF}, out[4];
  char h[9];
  toHex(in, 4, h);
  TEST_ASSERT_EQUAL_STRING("00ab0fff", h);
  TEST_ASSERT_TRUE(fromHex(h, out, 4));
  TEST_ASSERT_EQUAL_MEMORY(in, out, 4);
  TEST_ASSERT_TRUE(fromHex("00AB0FFF", out, 4));                            // capitals are accepted
  TEST_ASSERT_EQUAL_MEMORY(in, out, 4);
  TEST_ASSERT_FALSE(fromHex("00ab0ff", out, 4));                            // wrong length
  TEST_ASSERT_FALSE(fromHex("00ab0fffff", out, 4));
  TEST_ASSERT_FALSE(fromHex("00ab0fgf", out, 4));                           // not hex
  TEST_ASSERT_FALSE(fromHex("00ab0f f", out, 4));
  TEST_ASSERT_FALSE(fromHex("", out, 4));
}

void test_const_time_eq() {
  uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4}, c[4] = {1, 2, 3, 5}, d[4] = {0, 2, 3, 4};
  TEST_ASSERT_TRUE(constTimeEq(a, b, 4));
  TEST_ASSERT_FALSE(constTimeEq(a, c, 4));
  TEST_ASSERT_FALSE(constTimeEq(a, d, 4));                                  // a difference in the first byte counts too
  TEST_ASSERT_TRUE(constTimeEq(a, c, 3));
  TEST_ASSERT_TRUE(constTimeEq(a, c, 0));
}

void test_password_policy() {
  EQ(AuthErr::Policy, passwordPolicy(""));
  EQ(AuthErr::Policy, passwordPolicy("1234567"));                           // seven characters
  EQ(AuthErr::Ok, passwordPolicy("12345678"));
  EQ(AuthErr::Ok, passwordPolicy("abcdefghijklmnopqrstuvwxyz012345"));       // thirty-two
  EQ(AuthErr::Policy, passwordPolicy("abcdefghijklmnopqrstuvwxyz0123456"));  // thirty-three
  EQ(AuthErr::Ok, passwordPolicy("with spaces ok"));
  EQ(AuthErr::Ok, passwordPolicy("~~~~~~~~"));                              // 0x7E is the last printable
  EQ(AuthErr::Policy, passwordPolicy("tab\there1"));
  EQ(AuthErr::Policy, passwordPolicy("del\x7f" "here1"));
  EQ(AuthErr::Policy, passwordPolicy("caf\xc3\xa9" "pass1"));               // not ASCII
  EQ(AuthErr::SameAsDefault, passwordPolicy(kDefaultPassword));             // the factory password is not a choice
  EQ(AuthErr::Ok, passwordPolicy("espframework1"));
}

// ---- the password hash

void test_hash_matches_the_definition() {
  uint8_t salt[kSaltBytes];
  for (size_t i = 0; i < kSaltBytes; i++) salt[i] = uint8_t(i);
  uint8_t got[32], want[32];
  Sha256 s;
  s.update(salt, kSaltBytes);
  s.update(reinterpret_cast<const uint8_t*>("secret99"), 8);
  s.final(want);
  hashPassword("secret99", salt, 0, got);
  TEST_ASSERT_EQUAL_MEMORY(want, got, 32);                                  // no extra rounds: one hash of salt || password
  for (int r = 0; r < 3; r++) {
    s.update(want, 32);
    s.update(salt, kSaltBytes);
    s.update(reinterpret_cast<const uint8_t*>("secret99"), 8);
    s.final(want);
  }
  hashPassword("secret99", salt, 3, got);
  TEST_ASSERT_EQUAL_MEMORY(want, got, 32);                                  // each round feeds the previous hash back in
}

void test_hash_differs() {
  uint8_t s1[kSaltBytes] = {1}, s2[kSaltBytes] = {2};
  uint8_t a[32], b[32], c[32], d[32], e[32];
  hashPassword("secret99", s1, 10, a);
  hashPassword("secret99", s1, 10, b);
  hashPassword("secret99", s2, 10, c);
  hashPassword("secret98", s1, 10, d);
  hashPassword("secret99", s1, 11, e);
  TEST_ASSERT_EQUAL_MEMORY(a, b, 32);
  TEST_ASSERT_FALSE(constTimeEq(a, c, 32));                                 // another salt, another hash
  TEST_ASSERT_FALSE(constTimeEq(a, d, 32));                                 // another password
  TEST_ASSERT_FALSE(constTimeEq(a, e, 32));                                 // another round count
}

// ---- passwords in the core

static const uint8_t kSalt[kSaltBytes] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
static const uint8_t kRnd1[kTokenBytes] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
static uint8_t rnd(uint8_t seed, uint8_t* out = nullptr) {
  static uint8_t buf[kTokenBytes];
  uint8_t* p = out ? out : buf;
  for (size_t i = 0; i < kTokenBytes; i++) p[i] = uint8_t(seed + i * 5);
  return seed;
}

void test_default_password_until_changed() {
  AuthCore a;
  TEST_ASSERT_TRUE(a.mustChange());
  TEST_ASSERT_FALSE(a.hasCustomPassword());
  TEST_ASSERT_TRUE(a.passwordOk("espframework"));
  TEST_ASSERT_FALSE(a.passwordOk("espframeworK"));
  TEST_ASSERT_FALSE(a.passwordOk("espframewor"));                           // a prefix is not the password
  TEST_ASSERT_FALSE(a.passwordOk("espframework "));
  TEST_ASSERT_FALSE(a.passwordOk(""));
}

void test_set_password() {
  AuthCore a;
  EQ(AuthErr::Policy, a.setPassword("short", kSalt));
  EQ(AuthErr::SameAsDefault, a.setPassword("espframework", kSalt));
  TEST_ASSERT_TRUE(a.mustChange());                                         // a refused change leaves everything as it was
  TEST_ASSERT_TRUE(a.passwordOk("espframework"));
  EQ(AuthErr::Ok, a.setPassword("workshop-2026", kSalt));
  TEST_ASSERT_FALSE(a.mustChange());
  TEST_ASSERT_TRUE(a.hasCustomPassword());
  TEST_ASSERT_TRUE(a.passwordOk("workshop-2026"));
  TEST_ASSERT_FALSE(a.passwordOk("espframework"));                          // the factory password stops working
  TEST_ASSERT_FALSE(a.passwordOk("workshop-2027"));
  TEST_ASSERT_FALSE(a.passwordOk("workshop-202"));
  TEST_ASSERT_FALSE(a.passwordOk("Workshop-2026"));
}

void test_stored_round_trip() {
  AuthCore a;
  a.setPassword("workshop-2026", kSalt);
  uint8_t salt[kSaltBytes], hash[32];
  uint16_t rounds;
  a.getStored(salt, hash, rounds);
  EQ(kHashRounds, rounds);
  TEST_ASSERT_EQUAL_MEMORY(kSalt, salt, kSaltBytes);
  AuthCore b;
  b.loadStored(salt, hash, rounds);                                         // what the file holds after a restart
  TEST_ASSERT_TRUE(b.passwordOk("workshop-2026"));
  TEST_ASSERT_FALSE(b.passwordOk("workshop-2027"));
  TEST_ASSERT_FALSE(b.mustChange());
}

// ---- sessions

void test_login_and_session() {
  AuthCore a;
  char tok[kTokenChars + 1];
  rnd(1);
  uint8_t r[kTokenBytes];
  rnd(1, r);
  EQ(AuthErr::Ok, a.login("espframework", 1000, r, tok));
  EQ(kTokenChars, strlen(tok));
  char want[kTokenChars + 1];
  toHex(r, kTokenBytes, want);
  TEST_ASSERT_EQUAL_STRING(want, tok);
  TEST_ASSERT_TRUE(a.valid(tok, 2000));
  TEST_ASSERT_FALSE(a.valid("00000000000000000000000000000000", 2000));
  TEST_ASSERT_FALSE(a.valid("", 2000));
  TEST_ASSERT_FALSE(a.valid(nullptr, 2000));
  TEST_ASSERT_FALSE(a.valid("not a token", 2000));
  char bad[kTokenChars + 1];
  strcpy(bad, tok);
  bad[kTokenChars - 1] = bad[kTokenChars - 1] == '0' ? '1' : '0';          // one digit off
  TEST_ASSERT_FALSE(a.valid(bad, 2000));
  bad[0] = bad[0] == '0' ? '1' : '0';
  strcpy(bad, tok);
  bad[0] = bad[0] == '0' ? '1' : '0';                                       // the first digit off
  TEST_ASSERT_FALSE(a.valid(bad, 2000));
  EQ(1, a.sessionCount());
}

void test_wrong_password_makes_no_session() {
  AuthCore a;
  char tok[kTokenChars + 1] = "unchanged";
  uint8_t r[kTokenBytes];
  rnd(2, r);
  EQ(AuthErr::Wrong, a.login("nope", 0, r, tok));
  TEST_ASSERT_EQUAL_STRING("unchanged", tok);
  EQ(0, a.sessionCount());
}

void test_session_idle_expiry() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(3, r);
  a.login("espframework", 0, r, tok);
  TEST_ASSERT_TRUE(a.valid(tok, kSessionIdleMs));                           // exactly an hour is still fine, and renews it
  TEST_ASSERT_TRUE(a.valid(tok, kSessionIdleMs + kSessionIdleMs));          // renewed an hour ago
  TEST_ASSERT_FALSE(a.valid(tok, 2 * kSessionIdleMs + kSessionIdleMs + 1)); // an hour and a millisecond idle: gone
  TEST_ASSERT_FALSE(a.valid(tok, 2 * kSessionIdleMs + kSessionIdleMs + 2)); // and stays gone
  EQ(0, a.sessionCount());
}

void test_session_expiry_across_clock_wrap() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(4, r);
  uint32_t t0 = 0xFFFFFF00u;
  a.login("espframework", t0, r, tok);
  TEST_ASSERT_TRUE(a.valid(tok, t0 + 1000));                                // millis() wrapped in between
  TEST_ASSERT_TRUE(a.valid(tok, t0 + 1000 + kSessionIdleMs));
  TEST_ASSERT_FALSE(a.valid(tok, t0 + 1000 + 2 * kSessionIdleMs + 1));
}

void test_session_limit_evicts_the_idlest() {
  AuthCore a;
  char t[4][kTokenChars + 1];
  for (int i = 0; i < 3; i++) { uint8_t r[kTokenBytes]; rnd(uint8_t(10 + i), r); a.login("espframework", uint32_t(1000 * (i + 1)), r, t[i]); }
  EQ(3, a.sessionCount());
  TEST_ASSERT_TRUE(a.valid(t[0], 5000));                                    // the first is used again, so the second is the idlest
  uint8_t r[kTokenBytes];
  rnd(20, r);
  a.login("espframework", 6000, r, t[3]);
  EQ(3, a.sessionCount());
  TEST_ASSERT_TRUE(a.valid(t[0], 7000));
  TEST_ASSERT_FALSE(a.valid(t[1], 7000));                                   // pushed out
  TEST_ASSERT_TRUE(a.valid(t[2], 7000));
  TEST_ASSERT_TRUE(a.valid(t[3], 7000));
}

void test_logout() {
  AuthCore a;
  char t1[kTokenChars + 1], t2[kTokenChars + 1];
  uint8_t r1[kTokenBytes], r2[kTokenBytes];
  rnd(30, r1); rnd(31, r2);
  a.login("espframework", 0, r1, t1);
  a.login("espframework", 0, r2, t2);
  a.logout(t1);
  TEST_ASSERT_FALSE(a.valid(t1, 10));
  TEST_ASSERT_TRUE(a.valid(t2, 10));                                        // only that one
  a.logout("garbage");
  a.logout(nullptr);
  TEST_ASSERT_TRUE(a.valid(t2, 20));
  EQ(1, a.sessionCount());
}

void test_password_change_ends_other_sessions() {
  AuthCore a;
  char t1[kTokenChars + 1], t2[kTokenChars + 1];
  uint8_t r1[kTokenBytes], r2[kTokenBytes];
  rnd(40, r1); rnd(41, r2);
  a.login("espframework", 0, r1, t1);
  a.login("espframework", 0, r2, t2);
  a.endAllExcept(t1);
  TEST_ASSERT_TRUE(a.valid(t1, 10));
  TEST_ASSERT_FALSE(a.valid(t2, 10));
  a.endAllExcept(nullptr);
  EQ(0, a.sessionCount());
}

// ---- lock-out

void test_lockout_after_five_wrong() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(50, r);
  for (int i = 0; i < 4; i++) EQ(AuthErr::Wrong, a.login("bad-pass", 1000 + i, r, tok));
  EQ(0, a.lockedForMs(1010));                                               // four misses do not lock
  EQ(AuthErr::Wrong, a.login("bad-pass", 1004, r, tok));                    // the fifth does
  EQ(kLockMs - 1, a.lockedForMs(1005));
  EQ(AuthErr::Locked, a.login("espframework", 1006, r, tok));               // even the right password waits
  EQ(0, a.sessionCount());
  EQ(AuthErr::Locked, a.login("bad-pass", 1007, r, tok));
  EQ(1, a.lockedForMs(1004 + kLockMs - 1));
  EQ(0, a.lockedForMs(1004 + kLockMs));
  EQ(AuthErr::Ok, a.login("espframework", 1004 + kLockMs, r, tok));        // and then it is open again
}

void test_a_good_login_clears_the_miss_count() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(51, r);
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 4; i++) EQ(AuthErr::Wrong, a.login("bad-pass", 100 + uint32_t(round), r, tok));
    EQ(AuthErr::Ok, a.login("espframework", 100 + uint32_t(round), r, tok));  // four misses, then a hit: the count starts over
  }
  EQ(0, a.lockedForMs(200));
}

void test_lockout_misses_restart_after_it_ends() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(52, r);
  for (int i = 0; i < 5; i++) a.login("bad-pass", 0, r, tok);
  uint32_t later = kLockMs + 10;
  for (int i = 0; i < 4; i++) EQ(AuthErr::Wrong, a.login("bad-pass", later, r, tok));
  EQ(0, a.lockedForMs(later + 1));                                          // a fresh count of four, not five
  EQ(AuthErr::Wrong, a.login("bad-pass", later, r, tok));
  TEST_ASSERT_TRUE(a.lockedForMs(later + 1) > 0);
}

void test_lockout_across_clock_wrap() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(53, r);
  uint32_t t0 = 0xFFFFFFF0u;
  for (int i = 0; i < 5; i++) a.login("bad-pass", t0, r, tok);
  TEST_ASSERT_TRUE(a.lockedForMs(t0 + 100) > 0);                            // the clock wrapped inside the lock-out
  EQ(0, a.lockedForMs(t0 + kLockMs));
}

void test_new_password_works_for_login() {
  AuthCore a;
  a.setPassword("workshop-2026", kSalt);
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(60, r);
  EQ(AuthErr::Wrong, a.login("espframework", 0, r, tok));
  EQ(AuthErr::Ok, a.login("workshop-2026", 10, r, tok));
  TEST_ASSERT_TRUE(a.valid(tok, 20));
}

void test_error_codes() {
  TEST_ASSERT_EQUAL_STRING("", authErrCode(AuthErr::Ok));
  TEST_ASSERT_EQUAL_STRING("bad_credentials", authErrCode(AuthErr::Wrong));
  TEST_ASSERT_EQUAL_STRING("locked", authErrCode(AuthErr::Locked));
  TEST_ASSERT_EQUAL_STRING("weak_password", authErrCode(AuthErr::Policy));
  TEST_ASSERT_EQUAL_STRING("default_password", authErrCode(AuthErr::SameAsDefault));
}

void test_alive_does_not_renew() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(70, r);
  a.login("espframework", 0, r, tok);
  TEST_ASSERT_TRUE(a.alive(tok, 1000));
  TEST_ASSERT_TRUE(a.alive(tok, kSessionIdleMs));                           // an hour idle is still alive
  TEST_ASSERT_FALSE(a.alive(tok, kSessionIdleMs + 1));                      // one millisecond more is not
  TEST_ASSERT_FALSE(a.alive(tok, kSessionIdleMs + 1));                      // and checking never revived it
  TEST_ASSERT_TRUE(a.valid(tok, kSessionIdleMs));                           // valid() is the one that renews
  TEST_ASSERT_TRUE(a.alive(tok, kSessionIdleMs + 1000));
  TEST_ASSERT_FALSE(a.alive("", 1));
  TEST_ASSERT_FALSE(a.alive(nullptr, 1));
  TEST_ASSERT_FALSE(a.alive("00000000000000000000000000000000", 1));
  a.logout(tok);
  TEST_ASSERT_FALSE(a.alive(tok, kSessionIdleMs + 1000));
}

void test_alive_checks_the_whole_token() {
  AuthCore a;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(71, r);
  a.login("espframework", 0, r, tok);
  for (size_t i = 0; i < kTokenChars; i++) {                                // a change in any one digit makes it a different token
    char bad[kTokenChars + 1];
    strcpy(bad, tok);
    bad[i] = bad[i] == 'a' ? 'b' : 'a';
    TEST_ASSERT_FALSE(a.alive(bad, 10));
    TEST_ASSERT_FALSE(a.valid(bad, 10));
  }
  TEST_ASSERT_TRUE(a.alive(tok, 10));
}

void test_the_numbers_are_the_contract() {
  EQ(30000, kLockMs);
  EQ(3600000, kSessionIdleMs);
  EQ(5, kMaxFails);
  EQ(3, kMaxSessions);
  EQ(8, kMinPassword);
  EQ(32, kMaxPassword);
  EQ(500, kHashRounds);
  EQ(16, kSaltBytes);
  EQ(16, kTokenBytes);
  EQ(32, kTokenChars);
  TEST_ASSERT_EQUAL_STRING("espframework", kDefaultPassword);
}

void test_found_by_the_mutation_run() {
  EQ(AuthErr::Policy, passwordPolicy("abc" "defgh"));                    // 0x1F is a control character; 0x20 (space) is the first allowed
  EQ(AuthErr::Ok, passwordPolicy("abc defgh"));
  // a password longer than the limit is hashed as its first 32 characters (the policy keeps such ones out anyway)
  uint8_t h32[32], h40[32];
  const char* long40 = "abcdefghijklmnopqrstuvwxyz0123456789ABCD";           // 40 characters
  char first32[33];
  memcpy(first32, long40, 32);
  first32[32] = 0;
  hashPassword(first32, kSalt, 5, h32);
  hashPassword(long40, kSalt, 5, h40);
  TEST_ASSERT_EQUAL_MEMORY(h32, h40, 32);
  // a stored hash that is right except for its very last byte must not let the password in
  AuthCore a;
  a.setPassword("workshop-2026", kSalt);
  uint8_t salt[kSaltBytes], hash[32];
  uint16_t rounds;
  a.getStored(salt, hash, rounds);
  hash[31] ^= 1;
  AuthCore b;
  b.loadStored(salt, hash, rounds);
  TEST_ASSERT_FALSE(b.passwordOk("workshop-2026"));
  // once a lock-out is over, a later clock wrap must not bring it back
  AuthCore c;
  char tok[kTokenChars + 1];
  uint8_t r[kTokenBytes];
  rnd(80, r);
  for (int i = 0; i < 5; i++) c.login("bad-pass", 0, r, tok);
  EQ(AuthErr::Ok, c.login("espframework", kLockMs + 1000, r, tok));         // the lock ended and a login cleared it
  EQ(0, c.lockedForMs(kLockMs + 0x80000001u));                              // about 24.8 days on, the clock difference wraps: still open
  // a new session's idle time counts from its login, however late in the uptime that is
  AuthCore d;
  uint8_t r2[kTokenBytes];
  rnd(81, r2);
  d.login("espframework", 5000000u, r2, tok);
  TEST_ASSERT_TRUE(d.valid(tok, 5000100u));
  TEST_ASSERT_TRUE(d.alive(tok, 5000200u));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_vectors);
  RUN_TEST(test_sha256_million_a);
  RUN_TEST(test_sha256_split_updates_agree);
  RUN_TEST(test_sha256_reusable);
  RUN_TEST(test_hex);
  RUN_TEST(test_const_time_eq);
  RUN_TEST(test_password_policy);
  RUN_TEST(test_hash_matches_the_definition);
  RUN_TEST(test_hash_differs);
  RUN_TEST(test_default_password_until_changed);
  RUN_TEST(test_set_password);
  RUN_TEST(test_stored_round_trip);
  RUN_TEST(test_login_and_session);
  RUN_TEST(test_wrong_password_makes_no_session);
  RUN_TEST(test_session_idle_expiry);
  RUN_TEST(test_session_expiry_across_clock_wrap);
  RUN_TEST(test_session_limit_evicts_the_idlest);
  RUN_TEST(test_logout);
  RUN_TEST(test_password_change_ends_other_sessions);
  RUN_TEST(test_lockout_after_five_wrong);
  RUN_TEST(test_a_good_login_clears_the_miss_count);
  RUN_TEST(test_lockout_misses_restart_after_it_ends);
  RUN_TEST(test_lockout_across_clock_wrap);
  RUN_TEST(test_new_password_works_for_login);
  RUN_TEST(test_error_codes);
  RUN_TEST(test_the_numbers_are_the_contract);
  RUN_TEST(test_found_by_the_mutation_run);
  RUN_TEST(test_alive_does_not_renew);
  RUN_TEST(test_alive_checks_the_whole_token);
  return UNITY_END();
}
