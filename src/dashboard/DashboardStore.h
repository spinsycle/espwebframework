#pragma once
// Dashboard config <-> JSON, validation against the live device list, and /config/dashboard.json persistence.
// The browser edits the layout; the firmware only checks it and stores it. Nothing here draws anything.
#include <ArduinoJson.h>

#include "DashboardRules.h"
#include "devices/DeviceRegistry.h"

namespace espwf {
namespace DashboardStore {

struct Result {
  DashErr err = DashErr::Ok;
  int index = -1;  // component the error is in (-1 = the document itself)
  char id[kIdCap] = {0};
};

// Parses one component object. Only checks the JSON types; validateComponent does the rest.
DashErr parseComponent(JsonVariantConst src, DashComp& out);

// Validates a whole dashboard document against the registered devices.
Result validate(JsonVariantConst doc, DeviceRegistry& devices);

// Writes the document (temp file, then rename; the old file becomes dashboard.json.bak).
bool save(JsonVariantConst doc);

// The published tables the editor builds its menus from (GET /api/v1/dashboard/schema).
void describeSchema(JsonObject out);

}  // namespace DashboardStore
}  // namespace espwf
