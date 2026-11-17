#include "AutomationStore.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <new>

#include "config/ConfigRules.h"

namespace espwf {
namespace AutomationStore {

static const char* kFile = "/config/automation.json";
static const char* kTmp = "/config/automation.json.tmp";
static const char* kBak = "/config/automation.json.bak";

static bool getStr(JsonVariantConst v, char* dst, unsigned cap) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) return false;
  const char* s = v.as<const char*>();
  if (strlen(s) >= cap) return false;
  strcpy(dst, s);
  return true;
}

static bool getInt(JsonVariantConst v, long lo, long hi, long& out) {
  if (v.isNull()) return true;
  if (!v.is<long>() || v.is<bool>()) return false;
  long x = v.as<long>();
  if (x < lo || x > hi) return false;
  out = x;
  return true;
}

// "HH:MM" <-> minutes of the day
static bool parseHm(JsonVariantConst v, uint16_t& out, bool& present) {
  present = !v.isNull();
  if (!present) return true;
  if (!v.is<const char*>()) return false;
  const char* s = v.as<const char*>();
  if (strlen(s) != 5 || s[2] != ':') return false;
  for (int i : {0, 1, 3, 4})
    if (s[i] < '0' || s[i] > '9') return false;
  int h = (s[0] - '0') * 10 + (s[1] - '0'), m = (s[3] - '0') * 10 + (s[4] - '0');
  if (h > 23 || m > 59) return false;
  out = uint16_t(h * 60 + m);
  return true;
}
static void hm(uint16_t minutes, char* out) { snprintf(out, 6, "%02u:%02u", unsigned(minutes / 60), unsigned(minutes % 60)); }

// days: array of 0..6 (0 = Sunday). A missing field keeps the default (every day); an empty array is an error.
static bool parseDays(JsonVariantConst v, uint8_t& mask) {
  if (v.isNull()) return true;
  if (!v.is<JsonArrayConst>()) return false;
  uint8_t m = 0;
  for (JsonVariantConst d : v.as<JsonArrayConst>()) {
    if (!d.is<long>() || d.is<bool>() || d.as<long>() < 0 || d.as<long>() > 6) return false;
    m |= uint8_t(1u << d.as<long>());
  }
  if (!m) return false;
  mask = m;
  return true;
}
static void daysToJson(uint8_t mask, JsonArray out) {
  for (int d = 0; d < 7; d++)
    if (mask & (1u << d)) out.add(d);
}

// ---- conditions and actions (shared by rules and sequence steps) ----------------------------------------

static AutoErr parseCond(JsonVariantConst c, Cond& k) {
  if (!c.is<JsonObjectConst>()) return AutoErr::Conds;
  k = Cond();
  const char* kind = c["type"].is<const char*>() ? c["type"].as<const char*>() : "compare";
  if (!strcmp(kind, "time")) {
    k.kind = CondKind::Time;
    bool a, b;
    if (!parseHm(c["from"], k.from, a) || !parseHm(c["to"], k.to, b) || !a || !b) return AutoErr::Time;
    if (!parseDays(c["days"], k.days)) return AutoErr::Days;
    return AutoErr::Ok;
  }
  if (strcmp(kind, "compare")) return AutoErr::Conds;
  if (!getStr(c["device"], k.device, kIdCap)) return AutoErr::Device;
  if (!c["property"].is<const char*>() || !parseProp(c["property"].as<const char*>(), k.prop)) return AutoErr::Prop;
  if (!c["operator"].is<const char*>() || !parseOp(c["operator"].as<const char*>(), k.op)) return AutoErr::Op;
  if (!c["value"].is<double>()) return AutoErr::Value;
  k.value = c["value"].as<float>();
  long n = 0;
  if (!getInt(c["for_s"], 0, 1000000, n)) return AutoErr::Duration;
  k.forS = uint32_t(n);
  return AutoErr::Ok;
}

static void condToJson(const Cond& c, JsonObject o) {
  char b[6];
  if (c.kind == CondKind::Time) {
    o["type"] = "time";
    hm(c.from, b); o["from"] = b;
    hm(c.to, b); o["to"] = b;
    daysToJson(c.days, o["days"].to<JsonArray>());
    return;
  }
  o["device"] = c.device;
  o["property"] = propName(c.prop);
  o["operator"] = opName(c.op);
  o["value"] = c.value;
  if (c.forS) o["for_s"] = c.forS;
}

static AutoErr parseAction(JsonVariantConst a, Action& k) {
  if (!a.is<JsonObjectConst>()) return AutoErr::Action;
  k = Action();
  if (!a["type"].is<const char*>() || !parseAct(a["type"].as<const char*>(), k.type)) return AutoErr::Action;
  if (!getStr(a["device"], k.device, kIdCap)) return AutoErr::Device;
  if (!getStr(a["text"], k.text, kActTextCap)) return AutoErr::Value;
  if (!a["value"].isNull()) {
    if (!a["value"].is<double>() || a["value"].is<bool>()) return AutoErr::Value;
    k.value = a["value"].as<float>();
  }
  return AutoErr::Ok;
}

static void actionToJson(const Action& a, JsonObject o) {
  o["type"] = actName(a.type);
  if (a.type != ActType::Event) o["device"] = a.device;  // for seq_* and timer_* actions it names the sequence or timer
  if (a.type == ActType::Set) o["value"] = a.value;
  if (a.type == ActType::Send || a.type == ActType::Event) o["text"] = a.text;
}

// ---- rules ---------------------------------------------------------------------------------------------

AutoErr parseRule(JsonVariantConst src, Rule& r) {
  if (!src.is<JsonObjectConst>()) return AutoErr::Format;
  if (!getStr(src["id"], r.id, kIdCap)) return AutoErr::Id;
  if (!getStr(src["name"], r.name, kRuleNameCap)) return AutoErr::Name;
  if (!src["enabled"].isNull()) {
    if (!src["enabled"].is<bool>()) return AutoErr::Format;
    r.enabled = src["enabled"].as<bool>();
  }
  long n;
  if (!src["cooldown_s"].isNull()) {
    n = 0;
    if (!getInt(src["cooldown_s"], 0, 86400, n)) return AutoErr::Cooldown;
    r.cooldownS = uint32_t(n);
  }
  JsonVariantConst match = src["match"];
  if (!match.isNull()) {
    if (!match.is<const char*>()) return AutoErr::Conds;
    const char* m = match.as<const char*>();
    if (!strcmp(m, "any")) r.matchAny = true;
    else if (!strcmp(m, "all")) r.matchAny = false;
    else return AutoErr::Conds;
  }

  JsonVariantConst t = src["trigger"];
  if (!t.is<JsonObjectConst>() || !parseTrig(t["type"].as<const char*>(), r.trig.type)) return AutoErr::Trigger;
  if (!getStr(t["device"], r.trig.device, kIdCap)) return AutoErr::Device;
  if (!t["property"].isNull() && (!t["property"].is<const char*>() || !parseProp(t["property"].as<const char*>(), r.trig.prop))) return AutoErr::Prop;
  n = 0;
  if (!getInt(t["every_s"], 0, 1000000, n)) return AutoErr::Time;
  if (!t["every_s"].isNull()) r.trig.everyS = uint32_t(n);
  bool present;
  if (!parseHm(t["time"], r.trig.minute, present)) return AutoErr::Time;
  if (!parseDays(t["days"], r.trig.days)) return AutoErr::Days;
  if (!t["event"].isNull()) {
    if (!t["event"].is<const char*>()) return AutoErr::Trigger;
    const char* e = t["event"].as<const char*>();
    if (!strcmp(e, "connected")) r.trig.wifiUp = true;
    else if (!strcmp(e, "disconnected")) r.trig.wifiUp = false;
    else if (!strcmp(e, "done")) r.trig.onStopped = false;
    else if (!strcmp(e, "stopped")) r.trig.onStopped = true;
    else return AutoErr::Trigger;
  }

  JsonVariantConst conds = src["conditions"];
  if (!conds.isNull()) {
    if (!conds.is<JsonArrayConst>() || conds.size() > kMaxConds) return AutoErr::Conds;
    r.nConds = 0;
    for (JsonVariantConst c : conds.as<JsonArrayConst>()) {
      AutoErr e = parseCond(c, r.conds[r.nConds++]);
      if (e != AutoErr::Ok) return e;
    }
  }

  JsonVariantConst acts = src["actions"];
  if (!acts.isNull()) {
    if (!acts.is<JsonArrayConst>() || acts.size() > kMaxActions) return AutoErr::Actions;
    r.nActs = 0;
    for (JsonVariantConst a : acts.as<JsonArrayConst>()) {
      AutoErr e = parseAction(a, r.acts[r.nActs++]);
      if (e != AutoErr::Ok) return e;
    }
  }
  return AutoErr::Ok;
}

void ruleToJson(const Rule& r, JsonObject out) {
  out["id"] = r.id;
  out["name"] = r.name;
  out["enabled"] = r.enabled;
  JsonObject t = out["trigger"].to<JsonObject>();
  t["type"] = trigName(r.trig.type);
  char b[6];
  switch (r.trig.type) {
    case TrigType::Change: t["device"] = r.trig.device; t["property"] = propName(r.trig.prop); break;
    case TrigType::Interval: t["every_s"] = r.trig.everyS; break;
    case TrigType::Schedule: hm(r.trig.minute, b); t["time"] = b; daysToJson(r.trig.days, t["days"].to<JsonArray>()); break;
    case TrigType::Wifi: t["event"] = r.trig.wifiUp ? "connected" : "disconnected"; break;
    case TrigType::Timer: t["device"] = r.trig.device; break;
    case TrigType::Sequence: t["device"] = r.trig.device; t["event"] = r.trig.onStopped ? "stopped" : "done"; break;
    default: break;
  }
  out["match"] = r.matchAny ? "any" : "all";
  JsonArray conds = out["conditions"].to<JsonArray>();
  for (size_t i = 0; i < r.nConds; i++) condToJson(r.conds[i], conds.add<JsonObject>());
  JsonArray acts = out["actions"].to<JsonArray>();
  for (size_t i = 0; i < r.nActs; i++) actionToJson(r.acts[i], acts.add<JsonObject>());
  out["cooldown_s"] = r.cooldownS;
}

// ---- sequences and timers ------------------------------------------------------------------------------

static AutoErr parseStepJson(JsonVariantConst src, Step& st) {
  if (!src.is<JsonObjectConst>()) return AutoErr::Steps;
  st = Step();
  if (!src["type"].is<const char*>() || !parseStep(src["type"].as<const char*>(), st.type)) return AutoErr::Steps;
  long n = 0;
  switch (st.type) {
    case StepType::Do: {
      AutoErr e = parseAction(src["action"], st.act);
      if (e != AutoErr::Ok) return e;
      if (!src["guard"].isNull()) {
        st.hasGuard = true;
        e = parseCond(src["guard"], st.cond);
        if (e != AutoErr::Ok) return e;
      }
      if (!src["else"].isNull()) {
        if (!src["else"].is<const char*>()) return AutoErr::Steps;
        const char* v = src["else"].as<const char*>();
        if (!strcmp(v, "stop")) st.stopOnFalse = true;
        else if (strcmp(v, "skip")) return AutoErr::Steps;
      }
      return AutoErr::Ok;
    }
    case StepType::Wait:
      if (!getInt(src["seconds"], 0, 1000000, n)) return AutoErr::Time;
      st.secs = uint32_t(n);
      return AutoErr::Ok;
    case StepType::Until: {
      AutoErr e = parseCond(src["condition"], st.cond);
      if (e != AutoErr::Ok) return e;
      if (!getInt(src["timeout_s"], 0, 1000000, n)) return AutoErr::Time;
      st.secs = uint32_t(n);
      if (!src["on_timeout"].isNull()) {
        if (!src["on_timeout"].is<const char*>()) return AutoErr::Steps;
        const char* v = src["on_timeout"].as<const char*>();
        if (!strcmp(v, "stop")) st.stopOnTimeout = true;
        else if (strcmp(v, "continue")) return AutoErr::Steps;
      }
      return AutoErr::Ok;
    }
    default: return AutoErr::Steps;
  }
}

static void stepToJson(const Step& st, JsonObject o) {
  o["type"] = stepName(st.type);
  switch (st.type) {
    case StepType::Do:
      actionToJson(st.act, o["action"].to<JsonObject>());
      if (st.hasGuard) {
        condToJson(st.cond, o["guard"].to<JsonObject>());
        o["else"] = st.stopOnFalse ? "stop" : "skip";
      }
      break;
    case StepType::Wait: o["seconds"] = st.secs; break;
    case StepType::Until:
      condToJson(st.cond, o["condition"].to<JsonObject>());
      o["timeout_s"] = st.secs;
      o["on_timeout"] = st.stopOnTimeout ? "stop" : "continue";
      break;
    default: break;
  }
}

AutoErr parseSeq(JsonVariantConst src, SeqDef& q) {
  if (!src.is<JsonObjectConst>()) return AutoErr::Format;
  if (!getStr(src["id"], q.id, kIdCap)) return AutoErr::Id;
  if (!getStr(src["name"], q.name, kRuleNameCap)) return AutoErr::Name;
  long n = q.repeat;
  if (!getInt(src["repeat"], 0, 255, n)) return AutoErr::Loop;
  q.repeat = uint8_t(n);
  JsonVariantConst steps = src["steps"];
  if (!steps.isNull()) {
    if (!steps.is<JsonArrayConst>() || steps.size() > kMaxSteps) return AutoErr::Steps;
    q.nSteps = 0;
    for (JsonVariantConst s : steps.as<JsonArrayConst>()) {
      AutoErr e = parseStepJson(s, q.steps[q.nSteps++]);
      if (e != AutoErr::Ok) return e;
    }
  }
  return AutoErr::Ok;
}

void seqToJson(const SeqDef& q, JsonObject out) {
  out["id"] = q.id;
  out["name"] = q.name;
  out["repeat"] = q.repeat;
  JsonArray steps = out["steps"].to<JsonArray>();
  for (size_t i = 0; i < q.nSteps; i++) stepToJson(q.steps[i], steps.add<JsonObject>());
}

AutoErr parseTimer(JsonVariantConst src, TimerDef& t) {
  if (!src.is<JsonObjectConst>()) return AutoErr::Format;
  if (!getStr(src["id"], t.id, kIdCap)) return AutoErr::Id;
  if (!getStr(src["name"], t.name, kRuleNameCap)) return AutoErr::Name;
  long n = long(t.durationS);
  if (!getInt(src["duration_s"], 0, 1000000, n)) return AutoErr::Timer;
  t.durationS = uint32_t(n);
  if (!src["repeat"].isNull()) {
    if (!src["repeat"].is<bool>()) return AutoErr::Timer;
    t.repeat = src["repeat"].as<bool>();
  }
  return AutoErr::Ok;
}

void timerToJson(const TimerDef& t, JsonObject out) {
  out["id"] = t.id;
  out["name"] = t.name;
  out["duration_s"] = t.durationS;
  out["repeat"] = t.repeat;
}

// ---- settings ------------------------------------------------------------------------------------------

AutoErr parseSettings(JsonVariantConst src, AutoSettings& s) {
  if (!src.is<JsonObjectConst>()) return AutoErr::Format;
  if (!src["enabled"].isNull()) {
    if (!src["enabled"].is<bool>()) return AutoErr::Format;
    s.enabled = src["enabled"].as<bool>();
  }
  long n = s.utcOffsetMin;
  if (!getInt(src["utc_offset_min"], -720, 840, n)) return AutoErr::Offset;
  s.utcOffsetMin = int16_t(n);
  JsonVariantConst locks = src["interlocks"];
  if (!locks.isNull()) {
    if (!locks.is<JsonArrayConst>() || locks.size() > kMaxInterlocks) return AutoErr::Interlock;
    s.nLocks = 0;
    for (JsonVariantConst p : locks.as<JsonArrayConst>()) {
      if (!p.is<JsonArrayConst>() || p.size() != 2) return AutoErr::Interlock;
      Interlock& l = s.locks[s.nLocks++];
      l = Interlock();
      if (!p[0].is<const char*>() || strlen(p[0].as<const char*>()) >= kIdCap) return AutoErr::Interlock;
      if (!p[1].is<const char*>() || strlen(p[1].as<const char*>()) >= kIdCap) return AutoErr::Interlock;
      strcpy(l.a, p[0].as<const char*>());
      strcpy(l.b, p[1].as<const char*>());
    }
  }
  return AutoErr::Ok;
}

void settingsToJson(const AutoSettings& s, JsonObject out) {
  out["enabled"] = s.enabled;
  out["utc_offset_min"] = s.utcOffsetMin;
  JsonArray locks = out["interlocks"].to<JsonArray>();
  for (size_t i = 0; i < s.nLocks; i++) {
    JsonArray p = locks.add<JsonArray>();
    p.add(s.locks[i].a);
    p.add(s.locks[i].b);
  }
}

// ---- the file ------------------------------------------------------------------------------------------

static bool readFile(const char* path, AutomationCore& core, size_t& loaded) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  JsonVariantConst sv = doc["schema_version"];
  if (e || (!sv.isNull() && (!sv.is<long>() || configSchemaCheck(sv.as<long>()) != SchemaCheck::Ok))) {
    Serial.printf("[auto] %s unreadable or too new\n", path);
    return false;
  }
  AutoSettings s;
  if (parseSettings(doc, s) == AutoErr::Ok) core.settings = s;
  loaded = 0;
  for (JsonVariantConst item : doc["timers"].as<JsonArrayConst>()) {
    TimerDef t;
    AutoErr err = parseTimer(item, t);
    if (err == AutoErr::Ok) err = core.addTimer(t);
    if (err != AutoErr::Ok) Serial.printf("[auto] skipped saved timer '%s': %s\n", t.id, autoErrCode(err));
  }
  SeqDef* q = new (std::nothrow) SeqDef;  // about 1.5 KB: on the heap, one at a time
  for (JsonVariantConst item : doc["sequences"].as<JsonArrayConst>()) {
    if (!q) break;
    *q = SeqDef();
    AutoErr err = parseSeq(item, *q);
    if (err == AutoErr::Ok) err = core.addSeq(*q);
    if (err != AutoErr::Ok) Serial.printf("[auto] skipped saved sequence '%s': %s\n", q->id, autoErrCode(err));
  }
  delete q;
  for (JsonVariantConst item : doc["rules"].as<JsonArrayConst>()) {
    Rule r;
    AutoErr err = parseRule(item, r);
    if (err == AutoErr::Ok) err = core.add(r);
    if (err != AutoErr::Ok) { Serial.printf("[auto] skipped saved rule '%s': %s\n", r.id, autoErrCode(err)); continue; }
    loaded++;
  }
  return true;
}

size_t load(AutomationCore& core) {
  size_t n = 0;
  // tmp only survives a power cut between the renames in save(); bak is the previous good file.
  if (readFile(kFile, core, n) || readFile(kTmp, core, n) || readFile(kBak, core, n)) return n;
  return 0;
}

bool save(const AutoSettings& s, const AutomationCore& core) {
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  File f = LittleFS.open(kTmp, "w");
  if (!f) return false;
  {
    JsonDocument d;
    settingsToJson(s, d.to<JsonObject>());
    d["schema_version"] = kConfigSchema;
    String head;
    serializeJson(d, head);
    head.remove(head.length() - 1);  // drop the closing brace: the arrays follow
    f.print(head);
    f.print(",\"rules\":[");
  }
  for (size_t i = 0; i < core.count(); i++) {
    JsonDocument d;
    ruleToJson(core.rule(i), d.to<JsonObject>());
    if (i) f.print(',');
    serializeJson(d, f);
  }
  f.print("],\"timers\":[");
  for (size_t i = 0; i < core.timerCount(); i++) {
    JsonDocument d;
    timerToJson(core.timer(i), d.to<JsonObject>());
    if (i) f.print(',');
    serializeJson(d, f);
  }
  f.print("],\"sequences\":[");
  SeqDef* q = new (std::nothrow) SeqDef;
  for (size_t i = 0; q && i < core.seqCount(); i++) {
    core.seqDef(i, *q);
    JsonDocument d;
    seqToJson(*q, d.to<JsonObject>());
    if (i) f.print(',');
    serializeJson(d, f);
  }
  delete q;
  f.print("]}");
  f.close();
  LittleFS.remove(kBak);
  if (LittleFS.exists(kFile)) LittleFS.rename(kFile, kBak);
  return LittleFS.rename(kTmp, kFile);
}

}  // namespace AutomationStore
}  // namespace espwf
