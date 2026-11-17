// P0 smoke harness: blink the onboard LED and print the framework version.
#include <Arduino.h>
#include "core/Version.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  char v[16];
  espwf::versionString(v, sizeof v);
  Serial.printf("\nespwebframework %s smoke\n", v);
}

void loop() {
  static uint32_t last = 0;
  static bool on = false;
  if (millis() - last >= 500) {  // non-blocking, no delay()
    last = millis();
    on = !on;
    digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
  }
}
