#pragma once
// Board role. A master does nothing extra (it is the board as before). A slave watches its own buttons and,
// when a link fires, sends a command to the master's REST API: log in once, keep the session token, send
// POST /api/v1/command, log in again on a 401. One request at a time, a new TCP connection for each.
//
// Safety rule for irrigation: a command is never sent late. One older than 10 s (master unreachable, wrong
// password, busy) is dropped, so a valve never opens minutes after the button was pressed.
//
// As everywhere else: the TCP callbacks (they run on the async task) only copy bytes and set flags; the
// real work happens in loop().
#include <ArduinoJson.h>

#include "RoleConfig.h"

class AsyncClient;

namespace espwf {

class DeviceRegistry;
class NetworkManager;

class RoleManager {
 public:
  void begin(const RoleConfig& cfg, DeviceRegistry& devices, NetworkManager& net);
  void loop();

  // Validates, then saves and applies from loop(). Safe to call from an async HTTP handler.
  RoleCfgError requestApply(const RoleConfig& cfg, int* badLink = nullptr);

  const RoleConfig& config() const { return cfg_; }
  void status(JsonObject out) const;

 private:
  enum class Req : uint8_t { None, Login, Command };
  struct Rt {  // allocated the first time this board runs as a slave
    OpQueue<RemoteCmd, kRemoteQueue> q;
    LinkState ls[kMaxLinks];
    char req[320];
    char resp[640];
    char tok[48];
  };

  void applyPending();
  void pollButtons(uint32_t now);
  void service(uint32_t now);
  bool startRequest(Req r, uint32_t now);
  void finish(uint32_t now);
  void handleLogin(int st, uint32_t now);
  void handleCommand(int st, uint32_t now);
  void closeClient();
  void setError(const char* e);
  void failRequest(const char* why, uint32_t now);

  RoleConfig cfg_;
  RoleConfig* volatile pending_ = nullptr;  // heap: a master (the common case) pays nothing for it
  DeviceRegistry* dev_ = nullptr;
  NetworkManager* net_ = nullptr;
  Rt* rt_ = nullptr;
  AsyncClient* client_ = nullptr;

  Req req_ = Req::None;
  uint32_t reqStart_ = 0, nextTry_ = 0, nextPoll_ = 0;
  unsigned attempt_ = 0, relogins_ = 0;
  bool hasTok_ = false, badPw_ = false;
  RemoteCmd cur_;
  bool haveCur_ = false;
  volatile bool done_ = false;
  volatile size_t respLen_ = 0;
  char err_[40] = {0};
  char lastReject_[24] = {0};
  uint8_t missing_ = 0;  // links whose local button does not exist

  uint32_t sent_ = 0, rejected_ = 0, expired_ = 0, dropped_ = 0;
};

}  // namespace espwf
