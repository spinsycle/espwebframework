#pragma once
// MQTT client: publishes every device's state (retained) and accepts commands on <base>/<id>/set.
// Same rule as the rest of the firmware: the library's callbacks (they run on the async task) only set flags or
// queue a message; the real work, including every DeviceRegistry call, happens in loop().
#include <ArduinoJson.h>

#include "MqttConfig.h"
#include "NetworkConfig.h"  // kHostCap: the client id is the board's hostname
#include "devices/OpQueue.h"

class AsyncMqttClient;

namespace espwf {

class DeviceRegistry;
class NetworkManager;

class MqttManager {
 public:
  void begin(const MqttConfig& cfg, DeviceRegistry& devices, NetworkManager& net);
  void loop();

  // Validates, then saves and applies from loop(). Safe to call from an async HTTP handler.
  MqttCfgError requestApply(const MqttConfig& cfg);

  const MqttConfig& config() const { return cfg_; }
  void status(JsonObject out) const;

 private:
  enum class State : uint8_t { Off, Idle, Connecting, Connected };
  struct Msg {
    char id[kIdCap] = {0};
    uint8_t len = 0;
    char payload[kTextCap] = {0};
  };

  void applyPending();
  void ensureClient();
  void startConnect();
  void onConnected();
  void stop();
  void fail(const char* why, uint32_t now);
  void drainMessages();
  void publishChanged();
  void buildNames();

  MqttConfig cfg_;
  MqttConfig* volatile pending_ = nullptr;  // a saved-but-not-applied request; heap, so an idle board pays nothing for it
  DeviceRegistry* dev_ = nullptr;
  NetworkManager* net_ = nullptr;
  AsyncMqttClient* client_ = nullptr;  // allocated on first use: a board with MQTT off pays no RAM for it

  State state_ = State::Off;
  uint32_t nextTry_ = 0, connStart_ = 0;
  unsigned attempt_ = 0;
  volatile bool gotConn_ = false, gotDisc_ = false;
  volatile int discReason_ = 0;
  const char* lastError_ = "";

  // The client library keeps pointers to these, so they live as long as the manager.
  char clientId_[kHostCap] = {0};
  char willTopic_[kMqttTopicCap] = {0};

  static constexpr size_t kMax = kMaxDevices;
  struct Rt {  // everything a running connection needs; allocated the first time MQTT is enabled
    OpQueue<Msg, 5> inbox;  // callback -> loop(); usable depth 4
    MqttSig sigs[kMax];
    bool sent[kMax] = {false};
  };
  Rt* rt_ = nullptr;
  uint32_t structSeen_ = 0;

  uint32_t published_ = 0, received_ = 0, cmdOk_ = 0, cmdRejected_ = 0, dropped_ = 0;
  const char* lastCmdError_ = "";
};

}  // namespace espwf
