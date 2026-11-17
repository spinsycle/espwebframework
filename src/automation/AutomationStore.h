#pragma once
// Automation <-> JSON, and /config/automation.json persistence (temp file then rename, previous copy kept as .bak).
// The file is written one rule, sequence or timer at a time, so no big JSON document is ever built on the heap.
#include <ArduinoJson.h>

#include "AutomationCore.h"

namespace espwf {
namespace AutomationStore {

// Merges the fields present in `src` into `r`. Only checks JSON types and string lengths; validateRule does the rest.
AutoErr parseRule(JsonVariantConst src, Rule& r);
void ruleToJson(const Rule& r, JsonObject out);

AutoErr parseSeq(JsonVariantConst src, SeqDef& q);
void seqToJson(const SeqDef& q, JsonObject out);

AutoErr parseTimer(JsonVariantConst src, TimerDef& t);
void timerToJson(const TimerDef& t, JsonObject out);

AutoErr parseSettings(JsonVariantConst src, AutoSettings& s);
void settingsToJson(const AutoSettings& s, JsonObject out);

// Loads the file into the core (settings, timers, sequences, rules). Entries that no longer parse are skipped.
// References (devices, sequences, timers) are not checked here: a rule whose device was removed stays, and
// reports it when it runs.
size_t load(AutomationCore& core);

bool save(const AutoSettings& s, const AutomationCore& core);

}  // namespace AutomationStore
}  // namespace espwf
