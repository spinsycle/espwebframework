#pragma once
// JSON <-> RoleConfig and the /config/role.json file (temp file + rename, with a .bak of the previous good copy).
#include <ArduinoJson.h>

#include "RoleConfig.h"

namespace espwf {
namespace RoleStore {

// Merges the fields present in `src` into `cfg`; an absent field (and an absent password) is left alone,
// a present "links" array replaces the whole list. badLink as in validateRoleConfig.
RoleCfgError applyJson(JsonVariantConst src, RoleConfig& cfg, int* badLink = nullptr);

// The password only goes to the flash file; the API gets `password_set`.
void toJson(const RoleConfig& cfg, JsonObject out, bool includeSecrets);

bool load(RoleConfig& cfg);          // false = no usable file, cfg untouched
bool save(const RoleConfig& cfg);

}  // namespace RoleStore
}  // namespace espwf
