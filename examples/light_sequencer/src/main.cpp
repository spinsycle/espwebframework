// light_sequencer: the whole app. Devices, rules and sequences come from seed/*.json (see tools/build_ui.py); the rest is the framework.
#include <Framework.h>

FrameworkController Framework;

void setup() {
  Framework.setDeviceName("Light Sequencer");
  Framework.begin();
}

void loop() { Framework.loop(); }
