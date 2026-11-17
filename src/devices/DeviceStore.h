#pragma once
// Device config <-> JSON and /config/devices.json persistence (temp file + rename, like NetworkStore).
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

// Import path: write the new list to devices.json.new, then commit it over devices.json (the old file
// becomes devices.json.bak). Two steps so the write can be refused before anything live is touched.
bool saveNew(const DeviceConfig* devs, size_t n);
bool commitNew();

// Schema check on a parsed file or bundle (missing schema_version = version 1).
bool schemaOk(JsonVariantConst doc);

}  // namespace DeviceStore
}  // namespace espwf
