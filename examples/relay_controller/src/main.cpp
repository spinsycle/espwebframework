// Relay controller: the whole app. Everything else (Wi-Fi setup, login, device list, dashboard,
// rules, OTA) is the framework. Add the relay on the Devices tab, or ship it as a default:
// after begin(), POST /api/v1/devices {"id":"relay1","type":"relay","pin":5,"invert":true}.
#include <Framework.h>

FrameworkController Framework;

void setup() {
  Framework.setDeviceName("Relay Controller");
  Framework.begin();
}

void loop() { Framework.loop(); }
