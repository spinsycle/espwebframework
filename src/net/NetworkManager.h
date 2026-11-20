#pragma once
// Wi-Fi AP / STA / AP+STA with a non-blocking state machine.
// Settings changes are queued and applied from loop() so the HTTP response can flush first.
#include <ArduinoJson.h>

#include "NetworkConfig.h"

namespace espwf {

class NetworkManager {
 public:
  void begin(const NetworkConfig& cfg);
  void loop();

  // Validates, persists and schedules apply. Safe to call from an async HTTP handler.
  NetCfgError requestApply(const NetworkConfig& cfg);

  const NetworkConfig& config() const { return cfg_; }
  void status(JsonObject out) const;
  bool staConnected() const;

  // STA configured but unreachable for this long -> bring up the AP as a recovery path.
  static constexpr uint32_t kRecoveryMs = 30000;
  static constexpr uint32_t kApplyDelayMs = 600;

 private:
  void apply();
  void mdnsTick(bool staUp);  // starts/stops the .local responder to match config + link
  bool wantsSta() const { return cfg_.mode != NetMode::AP; }

  NetworkConfig cfg_;
  NetworkConfig pending_;
  volatile bool hasPending_ = false;  // ponytail: single flag; pending_ is written before it is set
  uint32_t applyAt_ = 0;
  uint32_t staLostSince_ = 0;
  bool recoveryAp_ = false;
  bool lastConn_ = false;
  bool mdnsUp_ = false;
  bool staticApplied_ = false;  // a static IP was set since boot, so DHCP must be restored explicitly
};

}  // namespace espwf
