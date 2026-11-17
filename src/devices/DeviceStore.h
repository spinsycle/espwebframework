#pragma once
// Device config <-> JSON and /config/devices.json persistence (temp file + rename, like NetworkStore).
// P5 folds this into the unified config system.
#include <ArduinoJson.h>

#include "DeviceTypes.h"

namespace espwf {
namespace DeviceStore {

// Merges the fields present in `src` into `cfg`. Wrong JSON types return the matching DevErr.
DevErr applyJson(JsonVariantConst src, DeviceConfig& cfg);
void toJson(const DeviceConfig& cfg, JsonObject out);

// Loads and validates /config/devices.json; entries that fail validation are skipped (logged).
size_t load(DeviceConfig* out, size_t cap, Chip chip);
bool save(const DeviceConfig* devs, size_t n);

}  // namespace DeviceStore
}  // namespace espwf
