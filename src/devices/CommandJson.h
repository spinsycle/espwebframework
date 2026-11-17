#pragma once
// {"id":"relay1","action":"set","value":true} -> Command. Shared by REST (POST /command) and the WebSocket.
#include <ArduinoJson.h>

#include "DeviceTypes.h"

namespace espwf {

// Ok, or BadId / BadAction. `id` points into `json` and is only valid while the document lives.
// The value is not range-checked here: DeviceRegistry::requestCommand does that against the device type.
CmdErr parseCommandJson(JsonVariantConst json, const char*& id, Command& out);

}  // namespace espwf
