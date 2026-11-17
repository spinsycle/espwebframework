// Smoke harness: framework up (AP + web UI) plus a non-blocking LED blink as a liveness signal.
#include <Framework.h>

FrameworkController Framework;

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Framework.setDeviceName("Smoke Test");
  Framework.begin();
}

void loop() {
  Framework.loop();

  static uint32_t last = 0;
  static bool on = false;
  if (millis() - last >= 500) {  // no delay()
    last = millis();
    on = !on;
    digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
  }
}
