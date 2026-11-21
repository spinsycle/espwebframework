#pragma once
// Device registry + state manager. Owns every device's config and live state.
// Rule: request*() only validate and queue (safe from the async server's task); begin() and loop()
// do all hardware work and are the only writers of device state.
#include <ArduinoJson.h>

#include "DeviceTypes.h"
#include "dashboard/DashboardRules.h"
#include "OpQueue.h"

namespace espwf {

struct DeviceView {  // a copy of one device, safe to read outside any lock
  DeviceConfig cfg;
  bool on = false;       // relay: energized; input: active
  uint8_t duty = 0;      // pwm percent
  uint32_t edges = 0;    // input: debounced changes since boot; serial: lines received
  int16_t val = 0;       // motor: speed -100..100; servo: angle
  uint32_t longs = 0;    // input: long presses since boot
  bool ok = false;       // sensor: last reading is good
  bool hasHum = false;   // sensor: the chip reports humidity (BME280, not BMP280)
  float temp = 0, hum = 0, press = 0;
  char text[kTextCap] = {0};  // serial: last line received
  bool tripped = false;  // failsafe fired and no command has arrived since
  uint16_t errs = 0;     // sensor: failed readings since boot
};

enum class ScanErr : uint8_t { Ok, Busy, PinsInUse };

// Result of validating a whole device list for import: which entry failed and why.
struct ImportResult {
  DevErr err = DevErr::Ok;
  size_t index = 0;
};

class DeviceRegistry {
 public:
  void begin();  // load config, put every output into its safe boot state
  void loop();

  DevErr requestAdd(const DeviceConfig& cfg);
  DevErr requestRemove(const char* id);
  bool config(const char* id, DeviceConfig& out) const;
  // One reading as a number (on/off = 1/0), for the automation engine. False if there is no such device,
  // it does not offer that property, or a sensor has no good reading right now.
  bool readProp(const char* id, Prop p, double& out) const;  // current config of one device; false if absent
  DevErr requestUpdate(const DeviceConfig& cfg);          // edit in place: same id and type, new settings
  // A button's debounced state and its long-press count, for a slave's links. False unless `id` is an input device.
  bool inputSnapshot(const char* id, bool& active, uint32_t& longs) const;

  // Replace every device with `list` (already parsed). Validates all of it first; on any failure nothing
  // changes. The swap itself happens in loop() and ends with restartRequested() turning true.
  ImportResult requestImport(const DeviceConfig* list, size_t n);
  bool restartRequested() const { return restart_; }
  void halt() { restart_ = true; }  // stop applying changes (and saving) ahead of a restart
  CmdErr requestCommand(const char* id, const Command& cmd);
  // "The controller is still there": any logged-in client, from any task. Keeps every failsafe timer from running out.
  void heartbeat() { beat_ = millis(); }

  // I2C bus scan: queued here, run in loop(); poll describeScan() for the result.
  ScanErr requestScan();
  void describeScan(JsonObject out) const;

  size_t count() const;
  bool view(size_t index, DeviceView& out) const;
  static void describe(const DeviceView& v, JsonObject out);       // config + live state + warnings
  static void describeState(const DeviceView& v, JsonObject out);  // just the live state object

  // Bumped whenever a device's state changes / a device is added or removed. Lets the WebSocket layer
  // skip scanning when nothing happened. Read without a lock: a stale read only delays an update by one pass.
  uint32_t stateVersion() const { return stateVer_; }
  uint32_t structureVersion() const { return structVer_; }

 private:
  struct Runtime {
    bool on = false;
    uint8_t duty = 0;
    uint32_t edges = 0;
    uint8_t slot = 0;  // pwm slot
    Debouncer deb;
    int16_t val = 0;   // motor speed / servo angle
    uint32_t longs = 0;
    uint32_t alive = 0;      // millis() of the last command (or of the start), for the failsafe
    bool tripped = false;    // failsafe fired; cleared by the next command
    uint16_t errs = 0;       // sensor: failed readings since boot
    uint32_t pressedAt = 0;
    bool longDone = true;
    // sensor
    bool ok = false, hasHum = false, ready = false;
    uint8_t phase = 0, fails = 0;  // phase 0 = idle, 1 = waiting for a conversion
    uint32_t next = 0, t0 = 0;
    float temp = 0, hum = 0, press = 0;
    void* h = nullptr;  // DS18B20 line handle
  };
  struct Op {
    enum Kind : uint8_t { Add, Remove, Cmd, Update } kind = Add;
    DeviceConfig cfg;
    char id[kIdCap] = {0};
    Command cmd;
  };
  static constexpr size_t kQueue = 6;

  int find(const char* id) const;  // -1 if absent; callable from any task (reads under no lock: ids only change in loop())
  bool push(const Op& op);
  bool pop(Op& op);
  void doAdd(const DeviceConfig& c);
  void doRemove(const char* id);
  void doUpdate(const DeviceConfig& c);
  void doImport();
  void doCommand(const char* id, const Command& cmd);
  void startHw(size_t i);
  void stopHw(size_t i);
  uint8_t allocSlot();
  void driveRelay(size_t i);
  void drivePwm(size_t i);
  void driveMotor(size_t i);
  void driveServo(size_t i);
  bool applySafe(size_t i);  // put the device in its safe state; true if that changed anything
  void checkFailsafes();
  void pollInputs();
  void pollSensors();
  void sensorFail(size_t i, uint32_t now);
  void sensorSet(size_t i, float t, float h, float p, uint32_t now);
  void pollSerial();
  void serialEmit();
  void doScan();

  DeviceConfig cfg_[kMaxDevices];
  Runtime rt_[kMaxDevices];
  size_t n_ = 0;
  volatile uint32_t beat_ = 0;  // millis() of the last heartbeat
  uint8_t pwmSlots_ = 0;  // bit i set = slot i in use

  // The one serial device (validateDevice allows at most one) keeps its text here, not per device.
  char serialLast_[kTextCap] = {0};
  char rxBuf_[kTextCap] = {0};
  uint8_t rxLen_ = 0;
  uint32_t serialLines_ = 0;
  bool serialOn_ = false;

  volatile bool scanReq_ = false;
  volatile bool scanning_ = false;
  volatile bool scanDone_ = false;
  uint8_t found_[16] = {0};
  uint8_t nFound_ = 0;

  // A pending import waits here until loop() writes it (heap, only while an import is in flight).
  DeviceConfig* importList_ = nullptr;
  size_t importN_ = 0;
  volatile bool restart_ = false;

  OpQueue<Op, kQueue> queue_;
  volatile uint32_t stateVer_ = 0;
  volatile uint32_t structVer_ = 0;
};

}  // namespace espwf
