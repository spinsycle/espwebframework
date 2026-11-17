#pragma once
// Hardware abstraction (P1 subset): chip facts, heap, restart, filesystem.
// One implementation per chip (Hal_esp8266.cpp / Hal_esp32.cpp, each #ifdef-guarded).
// GPIO, PWM, timers, UART, I2C join here in P2+.
#include <stddef.h>
#include <stdint.h>

namespace espwf {
namespace hal {

const char* chipName();           // "ESP8266" / "ESP32"
uint32_t chipId();                // last 3 bytes of the MAC, same convention on both chips
uint32_t freeHeap();
uint32_t maxFreeBlock();          // largest allocatable block (fragmentation indicator)
uint32_t flashSize();
void resetReason(char* out, size_t n);
void restart();

bool fsBegin();                   // mounts LittleFS, formats it on first use
size_t fsTotal();
size_t fsUsed();

}  // namespace hal
}  // namespace espwf
