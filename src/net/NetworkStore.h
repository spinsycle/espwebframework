#pragma once
// JSON <-> NetworkConfig and the /config/network.json file (P5 generalizes config storage).
#include <ArduinoJson.h>

#include "NetworkConfig.h"

namespace espwf {
namespace NetworkStore {

// Merges the fields present in `src` into `cfg`; absent fields are left alone.
// Does NOT validate semantics (call validateNetworkConfig); only rejects wrong types / too-long strings.
NetCfgError applyJson(JsonVariantConst src, NetworkConfig& cfg);

// Writes cfg without passwords unless includeSecrets (secrets only go to the flash file).
void toJson(const NetworkConfig& cfg, JsonObject out, bool includeSecrets);

bool load(NetworkConfig& cfg);          // false = no usable file, cfg untouched
bool save(const NetworkConfig& cfg);    // temp file + rename

}  // namespace NetworkStore
}  // namespace espwf
