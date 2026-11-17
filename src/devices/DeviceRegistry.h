#pragma once
// Device registry + state manager. Owns every device's config and live state.
// Rule: request*() only validate and queue (safe from the async server's task); begin() and loop()
// do all hardware work and are the only writers of device state.
#include <ArduinoJson.h>

#include "DeviceTypes.h"

namespace espwf {

struct DeviceView {  // a copy of one device, safe to read outside any lock
  DeviceConfig cfg;
  bool on = false;       // relay: energized; input: active
  uint8_t duty = 0;      // pwm percent
  uint32_t edges = 0;    // input: debounced changes since boot
};

class DeviceRegistry {
 public:
  void begin();  // load config, put every output into its safe boot state
  void loop();

  DevErr requestAdd(const DeviceConfig& cfg);
  DevErr requestRemove(const char* id);
  CmdErr requestCommand(const char* id, const Command& cmd);

  size_t count() const;
  bool view(size_t index, DeviceView& out) const;
  static void describe(const DeviceView& v, JsonObject out);  // config + live state + warnings

 private:
  struct Runtime {
    bool on = false;
    uint8_t duty = 0;
    uint32_t edges = 0;
    uint8_t slot = 0;  // pwm slot
    Debouncer deb;
  };
  struct Op {
    enum Kind : uint8_t { Add, Remove, Cmd } kind = Add;
    DeviceConfig cfg;
    char id[kIdCap] = {0};
    Command cmd;
  };
  static constexpr size_t kQueue = 8;

  int find(const char* id) const;  // -1 if absent; callable from any task (reads under no lock: ids only change in loop())
  bool push(const Op& op);
  bool pop(Op& op);
  void doAdd(const DeviceConfig& c);
  void doRemove(const char* id);
  void doCommand(const char* id, const Command& cmd);
  void startHw(size_t i);
  void stopHw(size_t i);
  void driveRelay(size_t i);
  void drivePwm(size_t i);
  void pollInputs();

  DeviceConfig cfg_[kMaxDevices];
  Runtime rt_[kMaxDevices];
  size_t n_ = 0;
  uint8_t pwmSlots_ = 0;  // bit i set = slot i in use

  Op queue_[kQueue];
  size_t qHead_ = 0, qTail_ = 0;
};

}  // namespace espwf
