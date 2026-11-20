#pragma once
// JSON <-> MqttConfig and the /config/mqtt.json file (temp file + rename, with a .bak of the previous good copy).
#include <ArduinoJson.h>

#include "MqttConfig.h"

namespace espwf {
namespace MqttStore {

// Merges the fields present in `src` into `cfg`; absent fields (and an absent password) are left alone.
MqttCfgError applyJson(JsonVariantConst src, MqttConfig& cfg);

// Writes cfg; the password only goes to the flash file, the API gets `password_set`.
void toJson(const MqttConfig& cfg, JsonObject out, bool includeSecrets);

bool load(MqttConfig& cfg);          // false = no usable file, cfg untouched
bool save(const MqttConfig& cfg);

}  // namespace MqttStore
}  // namespace espwf
