// Smoke harness: framework up (AP + web UI + device registry). Devices are added from the Devices tab,
// so this app touches no pins itself. Use a relay on the board LED pin (GPIO2) to prove a pin works.
#include <Framework.h>

FrameworkController Framework;

void setup() {
  Framework.setDeviceName("Smoke Test");
  Framework.begin();
}

void loop() { Framework.loop(); }
