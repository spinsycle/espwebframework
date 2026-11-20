// pan_tilt_rover: the whole app. Devices, rules and sequences come from seed/*.json (see tools/build_ui.py); the rest is the framework.
#include <Framework.h>

FrameworkController Framework;

void setup() {
  Framework.setDeviceName("Pan Tilt Rover");
  Framework.begin();
}

void loop() { Framework.loop(); }
