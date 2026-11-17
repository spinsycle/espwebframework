#pragma once
// WebSocket /ws: pushes live device state and status to every open page and takes commands.
//
// Server -> client events (JSON text frames):
//   {"event":"hello","proto":1}                      on connect; the client then GETs /api/v1/devices
//   {"event":"devices_changed"}                      a device was added or removed: refetch the list
//   {"event":"device_state","id":"x","state":{...}}  one device's state changed
//   {"event":"status", ...same body as GET /status}  every 2 s
//   {"event":"error","error":"...","field":"...","id":"..."}  answer to a rejected message
// Client -> server:
//   {"type":"command","id":"x","action":"set","value":true}   same rules as POST /api/v1/command
//   {"type":"ping"}  ->  {"event":"pong"}
//
// Rule as everywhere: the async callbacks validate and queue; loop() does the scanning and broadcasting.
#include <stddef.h>
#include <stdint.h>

#include "devices/DeviceTypes.h"
#include "net/RateLimiter.h"

class AsyncWebServer;
class AsyncWebSocket;
class AsyncWebSocketClient;
class FrameworkController;

namespace espwf {

class DeviceRegistry;

class WsHub {
 public:
  static constexpr size_t kMaxClients = 3;        // each client costs RAM on the ESP8266
  static constexpr uint16_t kMaxMsgsPerSec = 40;  // per client; a slider drag is about 20-30 per second
  static constexpr uint32_t kStatusEveryMs = 2000;
  static constexpr size_t kMaxMsgBytes = 256;

  void attach(AsyncWebServer& server, FrameworkController& fw, DeviceRegistry& devices);
  void loop();
  size_t clientCount() const;

 private:
  struct Slot {
    uint32_t id = 0;  // 0 = free
    RateLimiter limiter;
    bool warned = false;
  };
  struct Last {
    bool on = false;
    uint8_t duty = 0;
    uint32_t edges = 0;
  };

  void onData(AsyncWebSocketClient* c, void* arg, uint8_t* data, size_t len);
  void reply(AsyncWebSocketClient* c, const char* error, const char* field, const char* id);
  Slot* slotFor(uint32_t clientId);
  void scanStates(bool send);

  AsyncWebSocket* ws_ = nullptr;
  FrameworkController* fw_ = nullptr;
  DeviceRegistry* dev_ = nullptr;
  Slot slots_[kMaxClients + 1];
  Last last_[kMaxDevices];
  uint32_t seenState_ = 0, seenStruct_ = 0;
  uint32_t nextStatus_ = 0;
  bool dirty_ = false;  // a scan stopped early because a send queue was full
};

}  // namespace espwf
