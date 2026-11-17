#pragma once
// Pure config rules (no Arduino, no filesystem) so they are tested on the host.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace espwf {

// Bump when a saved or exported file changes shape, and add the upgrade step to configSchemaCheck's callers.
constexpr int kConfigSchema = 1;

enum class SchemaCheck : uint8_t { Ok, Bad, TooNew };

// version < 0 means the file has no schema_version: files written before P5 are version 1.
inline SchemaCheck configSchemaCheck(long version) {
  if (version < 0) return SchemaCheck::Ok;
  if (version < 1) return SchemaCheck::Bad;
  if (version > kConfigSchema) return SchemaCheck::TooNew;  // written by a newer framework: refuse, do not guess
  return SchemaCheck::Ok;
}

// Every file the stores create under /config. Factory reset removes exactly these.
static const char* const kConfigFiles[] = {
    "/config/network.json",     "/config/network.json.tmp",
    "/config/devices.json",     "/config/devices.json.tmp",
    "/config/devices.json.bak", "/config/devices.json.new",
    "/config/dashboard.json",   "/config/dashboard.json.tmp",
    "/config/dashboard.json.bak",
    "/config/automation.json",  "/config/automation.json.tmp",
    "/config/automation.json.bak",
    "/config/security.json",     "/config/security.json.tmp",
};
constexpr size_t kConfigFileCount = sizeof kConfigFiles / sizeof kConfigFiles[0];

inline bool isConfigFile(const char* path) {
  for (const char* f : kConfigFiles)
    if (!strcmp(f, path)) return true;
  return false;
}

}  // namespace espwf
