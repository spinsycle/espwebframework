#include <unity.h>
#include "net/HttpLite.h"
#include "net/RoleConfig.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

static RoleConfig slave() {
  RoleConfig c;
  c.role = Role::Slave;
  strcpy(c.host, "192.168.1.50");
  strcpy(c.pass, "secret123");
  return c;
}
static void addLink(RoleConfig& c, const char* in, LinkTrig t, const char* rem, LinkAct a, int v = 0) {
  LinkRule& l = c.links[c.nLinks++];
  strcpy(l.input, in); l.trig = t; strcpy(l.remote, rem); l.act = a; l.value = int16_t(v);
}

// ---- HTTP helpers

void test_build_request() {
  char b[512];
  size_t n = httpBuildRequest(b, sizeof b, "POST", "/api/v1/command", "10.0.0.5", "TOK", "{\"a\":1}");
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_UINT(strlen(b), n);
  TEST_ASSERT_NOT_NULL(strstr(b, "POST /api/v1/command HTTP/1.1\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Host: 10.0.0.5\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Connection: close\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Authorization: Bearer TOK\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Content-Length: 7\r\n"));
  TEST_ASSERT_TRUE(n > 7 && strcmp(b + n - 7 - 4, "\r\n\r\n{\"a\":1}") == 0 || strstr(b, "\r\n\r\n{\"a\":1}") != nullptr);
  size_t m = httpBuildRequest(b, sizeof b, "POST", "/x", "h", nullptr, "{}");
  TEST_ASSERT_TRUE(m > 0);
  TEST_ASSERT_NULL(strstr(b, "Authorization"));
  m = httpBuildRequest(b, sizeof b, "GET", "/x", "h", nullptr, nullptr);
  TEST_ASSERT_TRUE(m > 0);
  TEST_ASSERT_NULL(strstr(b, "Content-Length"));
  TEST_ASSERT_TRUE(strstr(b, "\r\n\r\n") == b + m - 4);   // headers end the message
}

void test_build_request_limits() {
  char b[512];
  size_t full = httpBuildRequest(b, sizeof b, "POST", "/p", "h", "T", "{}");
  TEST_ASSERT_TRUE(full > 0);
  char exact[512];
  TEST_ASSERT_EQUAL_UINT(full, httpBuildRequest(exact, full + 1, "POST", "/p", "h", "T", "{}"));   // exactly fits (NUL included)
  TEST_ASSERT_EQUAL_UINT(0, httpBuildRequest(exact, full, "POST", "/p", "h", "T", "{}"));          // one byte short
  TEST_ASSERT_EQUAL_UINT(0, httpBuildRequest(exact, 20, "POST", "/p", "h", "T", "{}"));
  TEST_ASSERT_EQUAL_UINT(0, httpBuildRequest(exact, 1, "POST", "/p", "h", nullptr, nullptr));
}

void test_status_line() {
  TEST_ASSERT_EQUAL_INT(200, httpStatus("HTTP/1.1 200 OK\r\n", 17));
  TEST_ASSERT_EQUAL_INT(401, httpStatus("HTTP/1.1 401 Unauthorized\r\n", 27));
  TEST_ASSERT_EQUAL_INT(202, httpStatus("HTTP/1.0 202 Accepted", 21));
  TEST_ASSERT_EQUAL_INT(-1, httpStatus("HTTP/1.1 20", 11));          // not all there yet
  TEST_ASSERT_EQUAL_INT(-1, httpStatus("HTTP/2 200 OK", 13));
  TEST_ASSERT_EQUAL_INT(-1, httpStatus("XTTP/1.1 200 OK", 15));
  TEST_ASSERT_EQUAL_INT(-1, httpStatus("HTTP/1.1 2x0 OK", 15));
  TEST_ASSERT_EQUAL_INT(-1, httpStatus("HTTP/1.1  200 OK", 16));
  TEST_ASSERT_EQUAL_INT(-1, httpStatus("", 0));
}

void test_json_string() {
  const char* r = "HTTP/1.1 200 OK\r\nX: y\r\n\r\n{\"ok\":true,\"token\":\"abc123\",\"must_change\":false}";
  char o[16];
  TEST_ASSERT_TRUE(httpJsonString(r, strlen(r), "token", o, sizeof o));
  TEST_ASSERT_EQUAL_STRING("abc123", o);
  const char* sp = "{\"error\": \"not_found\"}";
  TEST_ASSERT_TRUE(httpJsonString(sp, strlen(sp), "error", o, sizeof o));
  TEST_ASSERT_EQUAL_STRING("not_found", o);
  TEST_ASSERT_FALSE(httpJsonString(r, strlen(r), "nope", o, sizeof o));
  const char* cut = "{\"token\":\"abc";                               // closing quote not here yet
  TEST_ASSERT_FALSE(httpJsonString(cut, strlen(cut), "token", o, sizeof o));
  char tiny[4];
  TEST_ASSERT_FALSE(httpJsonString(r, strlen(r), "token", tiny, sizeof tiny));   // "abc123" does not fit in 4
  char fit[7];
  TEST_ASSERT_TRUE(httpJsonString(r, strlen(r), "token", fit, sizeof fit));      // 6 chars + NUL fits exactly
  const char* esc = "{\"token\":\"a\\\"b\"}";
  TEST_ASSERT_FALSE(httpJsonString(esc, strlen(esc), "token", o, sizeof o));    // escapes are refused
  const char* num = "{\"token\":5}";
  TEST_ASSERT_FALSE(httpJsonString(num, strlen(num), "token", o, sizeof o));
  const char* num2 = "{\"token\":5,\"x\":\"y\"}";                     // a number value must not be read as a string
  TEST_ASSERT_FALSE(httpJsonString(num2, strlen(num2), "token", o, sizeof o));
}

// ---- config

void test_master_default_valid() {
  RoleConfig c;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(c)));
}

void test_slave_needs_host_and_password() {
  RoleConfig c = slave();
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(c)));
  c.host[0] = 0;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Host), int(validateRoleConfig(c)));
  c = slave();
  c.pass[0] = 0;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Password), int(validateRoleConfig(c)));
  c = slave();
  strcpy(c.host, "bad host");
  TEST_ASSERT_EQUAL(int(RoleCfgError::Host), int(validateRoleConfig(c)));
  c.role = Role::Master;                      // a stored bad host is rejected even for a master
  TEST_ASSERT_EQUAL(int(RoleCfgError::Host), int(validateRoleConfig(c)));
  c.host[0] = 0;                              // a master needs neither
  c.pass[0] = 0;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(c)));
}

void test_port_and_terminators() {
  RoleConfig c = slave();
  c.port = 0;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Port), int(validateRoleConfig(c)));
  c.port = 8080;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(c)));
  memset(c.host, 'a', sizeof c.host);
  TEST_ASSERT_EQUAL(int(RoleCfgError::Host), int(validateRoleConfig(c)));
  c = slave();
  memset(c.pass, 'a', sizeof c.pass);
  TEST_ASSERT_EQUAL(int(RoleCfgError::Password), int(validateRoleConfig(c)));
  c = slave();
  c.role = Role(7);
  TEST_ASSERT_EQUAL(int(RoleCfgError::Role), int(validateRoleConfig(c)));
}

void test_link_rules() {
  int bad = 99;
  RoleConfig c = slave();
  addLink(c, "btn1", LinkTrig::Press, "valve1", LinkAct::Toggle);
  addLink(c, "btn1", LinkTrig::Long, "valve1", LinkAct::Off);
  addLink(c, "btn2", LinkTrig::Release, "pwm1", LinkAct::Set, 50);
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(c, &bad)));
  TEST_ASSERT_EQUAL_INT(-1, bad);

  RoleConfig other = c;
  addLink(other, "btn2", LinkTrig::Press, "valve2", LinkAct::On);   // a different button may use a trigger another button uses
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(other, &bad)));

  RoleConfig d = c;
  addLink(d, "btn1", LinkTrig::Press, "valve2", LinkAct::On);       // same button + trigger as link 0
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
  TEST_ASSERT_EQUAL_INT(3, bad);

  d = c;
  strcpy(d.links[1].input, "Bad Id");
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
  TEST_ASSERT_EQUAL_INT(1, bad);
  d = c;
  strcpy(d.links[0].remote, "");
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
  TEST_ASSERT_EQUAL_INT(0, bad);
  d = c;
  d.links[2].value = 181;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
  d.links[2].value = 180;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(d, &bad)));
  d.links[2].value = -100;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(d, &bad)));
  d.links[2].value = -101;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
  d = c;
  d.links[0].value = 999;                                           // value only matters for "set"
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(d, &bad)));
  d = c;
  d.links[0].trig = LinkTrig(9);
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
  d = c;
  d.links[0].act = LinkAct(9);
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
  d = c;
  d.nLinks = kMaxLinks + 1;
  TEST_ASSERT_EQUAL(int(RoleCfgError::Link), int(validateRoleConfig(d, &bad)));
}

void test_max_links() {
  RoleConfig c = slave();
  char in[kIdCap];
  for (size_t i = 0; i < kMaxLinks; i++) {
    snprintf(in, sizeof in, "btn%u", unsigned(i));
    addLink(c, in, LinkTrig::Press, "valve1", LinkAct::Toggle);
  }
  TEST_ASSERT_EQUAL(int(RoleCfgError::Ok), int(validateRoleConfig(c)));
}

void test_parse_names() {
  LinkTrig t; LinkAct a;
  TEST_ASSERT_TRUE(parseLinkTrig("press", t));   TEST_ASSERT_EQUAL(int(LinkTrig::Press), int(t));
  TEST_ASSERT_TRUE(parseLinkTrig("long", t));    TEST_ASSERT_EQUAL(int(LinkTrig::Long), int(t));
  TEST_ASSERT_TRUE(parseLinkTrig("release", t)); TEST_ASSERT_EQUAL(int(LinkTrig::Release), int(t));
  TEST_ASSERT_FALSE(parseLinkTrig("tap", t));
  TEST_ASSERT_FALSE(parseLinkTrig(nullptr, t));
  TEST_ASSERT_TRUE(parseLinkAct("toggle", a));   TEST_ASSERT_EQUAL(int(LinkAct::Toggle), int(a));
  TEST_ASSERT_TRUE(parseLinkAct("on", a));       TEST_ASSERT_EQUAL(int(LinkAct::On), int(a));
  TEST_ASSERT_TRUE(parseLinkAct("off", a));      TEST_ASSERT_EQUAL(int(LinkAct::Off), int(a));
  TEST_ASSERT_TRUE(parseLinkAct("set", a));      TEST_ASSERT_EQUAL(int(LinkAct::Set), int(a));
  TEST_ASSERT_FALSE(parseLinkAct("pulse", a));
  TEST_ASSERT_EQUAL_STRING("press", linkTrigName(LinkTrig::Press));
  TEST_ASSERT_EQUAL_STRING("long", linkTrigName(LinkTrig::Long));
  TEST_ASSERT_EQUAL_STRING("release", linkTrigName(LinkTrig::Release));
  TEST_ASSERT_EQUAL_STRING("toggle", linkActName(LinkAct::Toggle));
  TEST_ASSERT_EQUAL_STRING("on", linkActName(LinkAct::On));
  TEST_ASSERT_EQUAL_STRING("off", linkActName(LinkAct::Off));
  TEST_ASSERT_EQUAL_STRING("set", linkActName(LinkAct::Set));
}

// ---- button events

void test_events() {
  LinkState s;
  TEST_ASSERT_EQUAL_UINT8(0, linkEvents(s, true, 0));            // first reading primes: a button held at boot does not fire
  TEST_ASSERT_EQUAL_UINT8(0, linkEvents(s, true, 0));
  TEST_ASSERT_EQUAL_UINT8(kEvRelease, linkEvents(s, false, 0));
  TEST_ASSERT_EQUAL_UINT8(0, linkEvents(s, false, 0));
  TEST_ASSERT_EQUAL_UINT8(kEvPress, linkEvents(s, true, 0));
  TEST_ASSERT_EQUAL_UINT8(kEvLong, linkEvents(s, true, 1));      // long-press counter went up while held
  TEST_ASSERT_EQUAL_UINT8(kEvRelease, linkEvents(s, false, 1));
  TEST_ASSERT_EQUAL_UINT8(kEvPress | kEvLong, linkEvents(s, true, 2));   // both seen between two polls
  LinkState p;
  linkEvents(p, false, 5);
  TEST_ASSERT_EQUAL_UINT8(0, linkEvents(p, false, 5));           // an unchanged count is no event
}

void test_fires() {
  LinkRule l;
  l.trig = LinkTrig::Press;
  TEST_ASSERT_TRUE(linkFires(l, kEvPress));
  TEST_ASSERT_FALSE(linkFires(l, kEvLong));
  TEST_ASSERT_FALSE(linkFires(l, kEvRelease));
  TEST_ASSERT_TRUE(linkFires(l, kEvPress | kEvRelease));
  l.trig = LinkTrig::Long;
  TEST_ASSERT_TRUE(linkFires(l, kEvLong));
  TEST_ASSERT_FALSE(linkFires(l, kEvPress));
  l.trig = LinkTrig::Release;
  TEST_ASSERT_TRUE(linkFires(l, kEvRelease));
  TEST_ASSERT_FALSE(linkFires(l, kEvPress | kEvLong));
}

// ---- queue

static RemoteCmd rc(const char* id, LinkAct a, int v = 0, uint32_t born = 0) {
  RemoteCmd c;
  strcpy(c.id, id); c.act = a; c.value = int16_t(v); c.born = born;
  return c;
}

void test_queue_toggles_stack_and_sets_merge() {
  OpQueue<RemoteCmd, kRemoteQueue> q;
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("a", LinkAct::Toggle)));
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("a", LinkAct::Toggle)));
  TEST_ASSERT_EQUAL_UINT(2, q.size());                         // two presses are two toggles
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("a", LinkAct::On)));
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("a", LinkAct::Off)));   // replaces the waiting "on"
  TEST_ASSERT_EQUAL_UINT(3, q.size());
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("b", LinkAct::On)));    // another device: its own entry
  TEST_ASSERT_EQUAL_UINT(4, q.size());
  OpQueue<RemoteCmd, kRemoteQueue> q2;                          // a toggle never replaces a waiting on / off
  TEST_ASSERT_TRUE(remoteEnqueue(q2, rc("a", LinkAct::On)));
  TEST_ASSERT_TRUE(remoteEnqueue(q2, rc("a", LinkAct::Toggle)));
  TEST_ASSERT_EQUAL_UINT(2, q2.size());
  RemoteCmd o;
  TEST_ASSERT_TRUE(q.pop(o)); TEST_ASSERT_EQUAL(int(LinkAct::Toggle), int(o.act));
  TEST_ASSERT_TRUE(q.pop(o)); TEST_ASSERT_EQUAL(int(LinkAct::Toggle), int(o.act));
  TEST_ASSERT_TRUE(q.pop(o)); TEST_ASSERT_EQUAL(int(LinkAct::Off), int(o.act)); TEST_ASSERT_EQUAL_STRING("a", o.id);
  TEST_ASSERT_TRUE(q.pop(o)); TEST_ASSERT_EQUAL_STRING("b", o.id);
}

void test_queue_full() {
  OpQueue<RemoteCmd, kRemoteQueue> q;
  for (size_t i = 0; i < kRemoteQueue - 1; i++) TEST_ASSERT_TRUE(remoteEnqueue(q, rc("a", LinkAct::Toggle)));
  TEST_ASSERT_FALSE(remoteEnqueue(q, rc("a", LinkAct::Toggle)));
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("a", LinkAct::On)) == false);   // nothing to merge into, still full
}

void test_set_merge_keeps_latest_value() {
  OpQueue<RemoteCmd, kRemoteQueue> q;
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("p", LinkAct::Set, 10)));
  TEST_ASSERT_TRUE(remoteEnqueue(q, rc("p", LinkAct::Set, 70)));
  TEST_ASSERT_EQUAL_UINT(1, q.size());
  RemoteCmd o;
  TEST_ASSERT_TRUE(q.pop(o));
  TEST_ASSERT_EQUAL_INT(70, o.value);
}

void test_expiry() {
  RemoteCmd c = rc("a", LinkAct::On, 0, 1000);
  TEST_ASSERT_FALSE(remoteExpired(c, 1000));
  TEST_ASSERT_FALSE(remoteExpired(c, 1000 + kRemoteMaxAgeMs));      // exactly at the limit is still fresh
  TEST_ASSERT_TRUE(remoteExpired(c, 1000 + kRemoteMaxAgeMs + 1));
  RemoteCmd w = rc("a", LinkAct::On, 0, 0xFFFFFF00u);               // millis() wrap
  TEST_ASSERT_FALSE(remoteExpired(w, 0x100u));
  TEST_ASSERT_TRUE(remoteExpired(w, 0x100u + kRemoteMaxAgeMs));
}

void test_body() {
  char b[128];
  TEST_ASSERT_TRUE(remoteBody(b, sizeof b, rc("valve1", LinkAct::Toggle)));
  TEST_ASSERT_EQUAL_STRING("{\"id\":\"valve1\",\"action\":\"toggle\"}", b);
  TEST_ASSERT_TRUE(remoteBody(b, sizeof b, rc("valve1", LinkAct::On)));
  TEST_ASSERT_EQUAL_STRING("{\"id\":\"valve1\",\"action\":\"set\",\"value\":true}", b);
  TEST_ASSERT_TRUE(remoteBody(b, sizeof b, rc("valve1", LinkAct::Off)));
  TEST_ASSERT_EQUAL_STRING("{\"id\":\"valve1\",\"action\":\"set\",\"value\":false}", b);
  TEST_ASSERT_TRUE(remoteBody(b, sizeof b, rc("p", LinkAct::Set, -40)));
  TEST_ASSERT_EQUAL_STRING("{\"id\":\"p\",\"action\":\"set\",\"value\":-40}", b);
  char small[10];
  TEST_ASSERT_FALSE(remoteBody(small, sizeof small, rc("valve1", LinkAct::Toggle)));
  char exact[34];                                                    // the toggle body is 33 chars + NUL
  TEST_ASSERT_TRUE(remoteBody(exact, 34, rc("valve1", LinkAct::Toggle)));
  TEST_ASSERT_FALSE(remoteBody(exact, 33, rc("valve1", LinkAct::Toggle)));
  // what we send is what the master's parser takes
  Command cmd;
  TEST_ASSERT_TRUE(parseAction("toggle", cmd.action));
}

// ---- master's answer

void test_reply_classes() {
  TEST_ASSERT_EQUAL(int(CmdReply::Done), int(classifyCmdReply(200, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Done), int(classifyCmdReply(202, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Done), int(classifyCmdReply(299, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Relogin), int(classifyCmdReply(401, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::DefaultPassword), int(classifyCmdReply(403, true)));
  TEST_ASSERT_EQUAL(int(CmdReply::Rejected), int(classifyCmdReply(403, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Locked), int(classifyCmdReply(429, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Retry), int(classifyCmdReply(503, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Retry), int(classifyCmdReply(-1, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Rejected), int(classifyCmdReply(404, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Rejected), int(classifyCmdReply(409, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Rejected), int(classifyCmdReply(400, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Rejected), int(classifyCmdReply(500, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Rejected), int(classifyCmdReply(199, false)));
  TEST_ASSERT_EQUAL(int(CmdReply::Rejected), int(classifyCmdReply(300, false)));
}

void test_backoff() {
  TEST_ASSERT_EQUAL_UINT32(1000, roleBackoffMs(0));
  TEST_ASSERT_EQUAL_UINT32(2000, roleBackoffMs(1));
  TEST_ASSERT_EQUAL_UINT32(4000, roleBackoffMs(2));
  TEST_ASSERT_EQUAL_UINT32(8000, roleBackoffMs(3));
  TEST_ASSERT_EQUAL_UINT32(16000, roleBackoffMs(4));
  TEST_ASSERT_EQUAL_UINT32(30000, roleBackoffMs(5));
  TEST_ASSERT_EQUAL_UINT32(30000, roleBackoffMs(999));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_build_request);
  RUN_TEST(test_build_request_limits);
  RUN_TEST(test_status_line);
  RUN_TEST(test_json_string);
  RUN_TEST(test_master_default_valid);
  RUN_TEST(test_slave_needs_host_and_password);
  RUN_TEST(test_port_and_terminators);
  RUN_TEST(test_link_rules);
  RUN_TEST(test_max_links);
  RUN_TEST(test_parse_names);
  RUN_TEST(test_events);
  RUN_TEST(test_fires);
  RUN_TEST(test_queue_toggles_stack_and_sets_merge);
  RUN_TEST(test_queue_full);
  RUN_TEST(test_set_merge_keeps_latest_value);
  RUN_TEST(test_expiry);
  RUN_TEST(test_body);
  RUN_TEST(test_reply_classes);
  RUN_TEST(test_backoff);
  return UNITY_END();
}
