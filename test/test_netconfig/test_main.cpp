#include <unity.h>
#include "net/NetworkConfig.h"

using namespace espwf;

void setUp() {}
void tearDown() {}

static NetworkConfig defaults() {
  NetworkConfig c;
  setNetworkDefaults(c, 0xABCDEF);
  return c;
}

void test_defaults_valid() {
  NetworkConfig c = defaults();
  TEST_ASSERT_EQUAL_STRING("ESPWF-ABCDEF", c.apSsid);
  TEST_ASSERT_EQUAL_STRING("espwf-abcdef", c.hostname);
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
}

void test_sta_requires_ssid() {
  NetworkConfig c = defaults();
  c.mode = NetMode::STA;
  TEST_ASSERT_EQUAL(int(NetCfgError::StaSsid), int(validateNetworkConfig(c)));
  strcpy(c.staSsid, "home");
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
  c.mode = NetMode::AP_STA;
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
}

void test_passwords() {
  NetworkConfig c = defaults();
  strcpy(c.apPass, "short");
  TEST_ASSERT_EQUAL(int(NetCfgError::ApPass), int(validateNetworkConfig(c)));
  c = defaults();
  c.apPass[0] = 0;  // AP must not be open
  TEST_ASSERT_EQUAL(int(NetCfgError::ApPass), int(validateNetworkConfig(c)));
  c = defaults();
  strcpy(c.staSsid, "home");
  c.mode = NetMode::STA;
  strcpy(c.staPass, "1234567");  // 7 chars: invalid for WPA
  TEST_ASSERT_EQUAL(int(NetCfgError::StaPass), int(validateNetworkConfig(c)));
  c.staPass[0] = 0;              // open STA network is allowed
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
}

void test_channel_and_ap_ip() {
  NetworkConfig c = defaults();
  c.apChannel = 0;
  TEST_ASSERT_EQUAL(int(NetCfgError::ApChannel), int(validateNetworkConfig(c)));
  c.apChannel = 14;
  TEST_ASSERT_EQUAL(int(NetCfgError::ApChannel), int(validateNetworkConfig(c)));
  c = defaults();
  c.apIp[3] = 255;
  TEST_ASSERT_EQUAL(int(NetCfgError::ApIp), int(validateNetworkConfig(c)));
  c.apIp[3] = 0;  // network address, not a host
  TEST_ASSERT_EQUAL(int(NetCfgError::ApIp), int(validateNetworkConfig(c)));
  c.apIp[3] = 1;
  c.apIp[0] = 127; c.apIp[3] = 1;
  TEST_ASSERT_EQUAL(int(NetCfgError::ApIp), int(validateNetworkConfig(c)));
}

void test_static_ip_rules() {
  NetworkConfig c = defaults();
  c.mode = NetMode::STA;
  strcpy(c.staSsid, "home");
  c.staStatic = true;
  TEST_ASSERT_EQUAL(int(NetCfgError::StaIp), int(validateNetworkConfig(c)));
  uint8_t ip[4] = {192, 168, 1, 50}, gw[4] = {192, 168, 1, 1};
  memcpy(c.staIp, ip, 4); memcpy(c.staGateway, gw, 4);
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
  c.staMask[0] = 255; c.staMask[1] = 0; c.staMask[2] = 255; c.staMask[3] = 0;  // non-contiguous
  TEST_ASSERT_EQUAL(int(NetCfgError::StaMask), int(validateNetworkConfig(c)));
  c.staMask[0] = 255; c.staMask[1] = 255; c.staMask[2] = 255; c.staMask[3] = 255;  // /32
  TEST_ASSERT_EQUAL(int(NetCfgError::StaMask), int(validateNetworkConfig(c)));
}

void test_hostname() {
  NetworkConfig c = defaults();
  strcpy(c.hostname, "-bad");
  TEST_ASSERT_EQUAL(int(NetCfgError::Hostname), int(validateNetworkConfig(c)));
  strcpy(c.hostname, "has space");
  TEST_ASSERT_EQUAL(int(NetCfgError::Hostname), int(validateNetworkConfig(c)));
  strcpy(c.hostname, "good-name1");
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
}

void test_mdns_rules() {
  NetworkConfig c = defaults();
  TEST_ASSERT_FALSE(c.mdnsEnabled);                       // off by default
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
  c.mdnsEnabled = true;                                   // enabled needs a name
  TEST_ASSERT_EQUAL(int(NetCfgError::MdnsName), int(validateNetworkConfig(c)));
  strcpy(c.mdnsName, "relays");
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
  strcpy(c.mdnsName, "re lays");
  TEST_ASSERT_EQUAL(int(NetCfgError::MdnsName), int(validateNetworkConfig(c)));
  strcpy(c.mdnsName, "relays.local");                     // dots are not allowed in the label
  TEST_ASSERT_EQUAL(int(NetCfgError::MdnsName), int(validateNetworkConfig(c)));
  c.mdnsEnabled = false;                                  // a bad stored name is rejected even when off
  TEST_ASSERT_EQUAL(int(NetCfgError::MdnsName), int(validateNetworkConfig(c)));
  c.mdnsName[0] = 0;
  TEST_ASSERT_EQUAL(int(NetCfgError::Ok), int(validateNetworkConfig(c)));
  memset(c.mdnsName, 'a', sizeof c.mdnsName);             // no NUL
  TEST_ASSERT_EQUAL(int(NetCfgError::MdnsName), int(validateNetworkConfig(c)));
  TEST_ASSERT_EQUAL_STRING("mdns.name", netCfgErrorField(NetCfgError::MdnsName));
}

void test_unterminated_string_rejected() {
  NetworkConfig c = defaults();
  memset(c.apSsid, 'a', sizeof c.apSsid);  // no NUL
  TEST_ASSERT_EQUAL(int(NetCfgError::ApSsid), int(validateNetworkConfig(c)));
}

void test_parse_ipv4() {
  uint8_t o[4];
  TEST_ASSERT_TRUE(parseIPv4("192.168.4.1", o));
  TEST_ASSERT_EQUAL_UINT8(192, o[0]);
  TEST_ASSERT_EQUAL_UINT8(1, o[3]);
  TEST_ASSERT_FALSE(parseIPv4("192.168.4", o));
  TEST_ASSERT_FALSE(parseIPv4("192.168.4.256", o));
  TEST_ASSERT_FALSE(parseIPv4("192.168.4.1.5", o));
  TEST_ASSERT_FALSE(parseIPv4("a.b.c.d", o));
  TEST_ASSERT_FALSE(parseIPv4("", o));
  TEST_ASSERT_FALSE(parseIPv4(nullptr, o));
}

void test_parse_mode() {
  NetMode m;
  TEST_ASSERT_TRUE(parseNetMode("sta", m));
  TEST_ASSERT_EQUAL(int(NetMode::STA), int(m));
  TEST_ASSERT_FALSE(parseNetMode("wifi", m));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_valid);
  RUN_TEST(test_sta_requires_ssid);
  RUN_TEST(test_passwords);
  RUN_TEST(test_channel_and_ap_ip);
  RUN_TEST(test_static_ip_rules);
  RUN_TEST(test_hostname);
  RUN_TEST(test_mdns_rules);
  RUN_TEST(test_unterminated_string_rejected);
  RUN_TEST(test_parse_ipv4);
  RUN_TEST(test_parse_mode);
  return UNITY_END();
}
