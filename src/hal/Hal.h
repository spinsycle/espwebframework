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

// Brief critical section between loop() and the async server's task (a real second task on ESP32,
// a no-op on the cooperative ESP8266). Copy small things inside it; never print or serialize.
struct CriticalSection {
  CriticalSection();
  ~CriticalSection();
  CriticalSection(const CriticalSection&) = delete;
  CriticalSection& operator=(const CriticalSection&) = delete;
};

// PWM, duty in permille (0..1000). `slot` is a small index unique per PWM device (ESP32 LEDC channel).
// ESP8266: the PWM frequency is global; the last device attached sets it.
void pwmAttach(int pin, uint32_t freqHz, uint8_t slot);
void pwmWrite(int pin, uint8_t slot, uint16_t permille);
void pwmDetach(int pin, uint8_t slot);

}  // namespace hal
}  // namespace espwf
