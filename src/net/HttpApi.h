#pragma once
// REST routes (/api/v1/...) and static UI from LittleFS (/www). ESPAsyncWebServer underneath.
// Rule: handlers only validate and queue work; hardware changes happen in loop().

class FrameworkController;
class AsyncWebServer;

namespace espwf {

class NetworkManager;
class DeviceRegistry;
class AutomationEngine;
class AuthManager;
class WsHub;
class MqttManager;
class RoleManager;

class HttpApi {
 public:
  void begin(FrameworkController& fw, NetworkManager& net, DeviceRegistry& devices, WsHub& ws, AutomationEngine& automation, AuthManager& auth,
             MqttManager& mqtt, RoleManager* role);  // role is null on a build without it (ESP8266)

 private:
  AsyncWebServer* server_ = nullptr;  // allocated once, lives for the program's lifetime
};

}  // namespace espwf
