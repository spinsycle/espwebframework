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

// PWM, duty in 1/10000 (0..10000; a servo pulse needs the fine steps). `slot` is a small index unique per
// PWM device (0..7). ESP8266: the PWM frequency is global (the last device attached sets it, and
// validateDevice keeps every PWM device on one frequency). ESP32: each slot owns its own LEDC timer.
void pwmAttach(int pin, uint32_t freqHz, uint8_t slot);
void pwmWrite(int pin, uint8_t slot, uint16_t duty10k);
void pwmDetach(int pin, uint8_t slot);

// One extra serial port for a "serial" device (UART0 stays the log console).
// ESP8266: software serial on any two pins (reliable to 38400 baud). ESP32: UART2 on any two pins.
bool uartBegin(int rxPin, int txPin, uint32_t baud);
void uartEnd();
int uartRead();                        // next byte, or -1 if none waiting
void uartWrite(const char* s, size_t n);

// Wall clock. clockBegin() starts NTP (it only gets an answer once the Wi-Fi client is connected);
// epochUtc() is UTC seconds, which is below ~1.6e9 until the first answer arrives.
void clockBegin();
uint32_t epochUtc();

// Bytes from the chip's hardware random number generator (session tokens, password salts).
void randomBytes(uint8_t* out, size_t n);

}  // namespace hal
}  // namespace espwf
