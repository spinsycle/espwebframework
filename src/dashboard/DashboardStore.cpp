#include "DashboardStore.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <new>

#include "config/ConfigRules.h"

namespace espwf {
namespace DashboardStore {

static const char* kFile = "/config/dashboard.json";
static const char* kTmp = "/config/dashboard.json.tmp";
static const char* kBak = "/config/dashboard.json.bak";

static bool lookup(void* ctx, const char* id, DeviceConfig& out) {
  return static_cast<DeviceRegistry*>(ctx)->config(id, out);
}

// Optional string into a fixed buffer. Wrong type or too long = false.
static bool getStr(JsonVariantConst v, char* dst, unsigned cap) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) return false;
  const char* s = v.as<const char*>();
  if (strlen(s) >= cap) return false;
  strcpy(dst, s);
  return true;
}

// Optional integer in [lo, hi]. Wrong type or range = false.
static bool getInt(JsonVariantConst v, long lo, long hi, long& out) {
  if (v.isNull()) return true;
  if (!v.is<long>() || v.is<bool>()) return false;
  long x = v.as<long>();
  if (x < lo || x > hi) return false;
  out = x;
  return true;
}

DashErr parseComponent(JsonVariantConst src, DashComp& c) {
  if (!src.is<JsonObjectConst>()) return DashErr::Format;
  if (!getStr(src["id"], c.id, kIdCap)) return DashErr::Id;
  if (!src["type"].is<const char*>() || !parseCompType(src["type"].as<const char*>(), c.type)) return DashErr::Type;
  if (!getStr(src["label"], c.label, kLabelCap)) return DashErr::Label;

  long n;
  JsonVariantConst lay = src["layout"];
  if (!lay.isNull() && !lay.is<JsonObjectConst>()) return DashErr::Layout;
  n = c.col; if (!getInt(lay["column"], 0, 255, n)) return DashErr::Layout; c.col = uint8_t(n);
  n = c.row; if (!getInt(lay["row"], 0, 255, n)) return DashErr::Layout; c.row = uint8_t(n);
  n = c.w; if (!getInt(lay["width"], 0, 255, n)) return DashErr::Layout; c.w = uint8_t(n);
  n = c.h; if (!getInt(lay["height"], 0, 255, n)) return DashErr::Layout; c.h = uint8_t(n);
  n = c.tab; if (!getInt(src["tab"], 0, 255, n)) return DashErr::Tab; c.tab = uint8_t(n);

  JsonVariantConst b = src["binding"];
  if (!b.isNull() && !b.is<JsonObjectConst>()) return DashErr::Device;
  if (!getStr(b["device"], c.device, kIdCap) || !getStr(b["device2"], c.device2, kIdCap)) return DashErr::Device;
  JsonVariantConst p = b["property"];
  if (!p.isNull() && (!p.is<const char*>() || !parseProp(p.as<const char*>(), c.prop))) return DashErr::Prop;

  // Push button: {"action":"set","value":50} or {"action":"send","value":"text"}.
  JsonVariantConst a = b["action"];
  if (!a.isNull()) {
    if (!a.is<const char*>() || !parseAction(a.as<const char*>(), c.cmd.action)) return DashErr::Action;
    JsonVariantConst v = b["value"];
    if (v.is<bool>()) {
      c.cmd.hasValue = c.cmd.isBool = true;
      c.cmd.value = v.as<bool>() ? 1 : 0;
    } else if (v.is<double>()) {
      c.cmd.hasValue = true;
      c.cmd.value = v.as<double>();
    } else if (v.is<const char*>()) {
      const char* t = v.as<const char*>();
      c.cmd.isText = true;
      if (strlen(t) >= kTextCap) return DashErr::Action;
      strcpy(c.cmd.text, t);
    } else if (!v.isNull()) {
      return DashErr::Action;
    }
  }

  JsonVariantConst devs = b["devices"];
  if (!devs.isNull()) {
    if (!devs.is<JsonArrayConst>() || devs.size() > kMaxGroupDevices) return DashErr::Device;
    for (JsonVariantConst d : devs.as<JsonArrayConst>()) {
      if (!d.is<const char*>() || strlen(d.as<const char*>()) >= kIdCap) return DashErr::Device;
      strcpy(c.devs[c.nDevs++], d.as<const char*>());
    }
  }

  JsonVariantConst opts = src["options"];
  if (!opts.isNull()) {
    if (!opts.is<JsonArrayConst>() || opts.size() > kMaxOptions) return DashErr::Options;
    for (JsonVariantConst o : opts.as<JsonArrayConst>()) {
      long v = 0;
      if (!o.is<JsonObjectConst>() || !o["label"].is<const char*>() || !getStr(o["label"], c.opts[c.nOpts].label, kOptCap)) return DashErr::Options;
      if (!o["value"].is<long>() || !getInt(o["value"], -32768, 32767, v)) return DashErr::Options;
      c.opts[c.nOpts++].value = int16_t(v);
    }
  }
  return DashErr::Ok;
}

Result validate(JsonVariantConst doc, DeviceRegistry& devices) {
  Result r;
  if (!doc.is<JsonObjectConst>()) { r.err = DashErr::Format; return r; }
  JsonVariantConst sv = doc["schema_version"];
  if (!sv.isNull() && (!sv.is<long>() || sv.is<bool>() || configSchemaCheck(sv.as<long>()) != SchemaCheck::Ok)) {
    r.err = DashErr::Format;
    return r;
  }
  JsonVariantConst mode = doc["mode"];
  if (!mode.is<const char*>() || (strcmp(mode.as<const char*>(), "auto") && strcmp(mode.as<const char*>(), "manual"))) {
    r.err = DashErr::Mode;
    return r;
  }

  char names[kMaxTabs][kTabCap] = {{0}};
  size_t nTabs = 0;
  JsonVariantConst tabs = doc["tabs"];
  if (!tabs.isNull()) {
    if (!tabs.is<JsonArrayConst>() || tabs.size() > kMaxTabs) { r.err = DashErr::Tabs; return r; }
    for (JsonVariantConst t : tabs.as<JsonArrayConst>()) {
      if (!t.is<const char*>() || strlen(t.as<const char*>()) >= kTabCap) { r.err = DashErr::Tabs; return r; }
      strcpy(names[nTabs++], t.as<const char*>());
    }
  }
  r.err = validateTabNames(names, nTabs);
  if (r.err != DashErr::Ok) return r;

  JsonVariantConst comps = doc["components"];
  if (comps.isNull()) return r;
  if (!comps.is<JsonArrayConst>()) { r.err = DashErr::Format; return r; }
  if (comps.size() > kMaxComponents) { r.err = DashErr::TooMany; return r; }
  // Automatic mode does not use the components, so they are stored as sent and checked when manual mode is switched on.
  if (!strcmp(mode.as<const char*>(), "auto")) return r;

  DashComp* c = new (std::nothrow) DashComp;  // one component at a time: it is about 400 bytes
  DashSet* set = new (std::nothrow) DashSet;
  if (!c || !set) {
    delete c;
    delete set;
    r.err = DashErr::TooMany;
    return r;
  }
  int i = 0;
  for (JsonVariantConst item : comps.as<JsonArrayConst>()) {
    *c = DashComp();
    DashErr e = parseComponent(item, *c);
    if (e == DashErr::Ok) e = validateComponent(*c, lookup, &devices, nTabs);
    if (e == DashErr::Ok) e = set->add(*c);
    if (e != DashErr::Ok) {
      r.err = e;
      r.index = i;
      strlcpy(r.id, c->id, sizeof r.id);
      break;
    }
    i++;
  }
  delete c;
  delete set;
  return r;
}

bool save(JsonVariantConst doc) {
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  File f = LittleFS.open(kTmp, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) { LittleFS.remove(kTmp); return false; }
  LittleFS.remove(kBak);
  if (LittleFS.exists(kFile)) LittleFS.rename(kFile, kBak);
  return LittleFS.rename(kTmp, kFile);
}

static const char* needName(Need n) {
  static const char* const names[] = {"card", "devices", "action", "write_state", "write_level", "read_number", "read_state", "read_text"};
  return names[uint8_t(n)];
}

void describeSchema(JsonObject out) {
  JsonObject g = out["grid"].to<JsonObject>();
  g["columns"] = kGridCols;
  g["max_row"] = kMaxRow;
  g["max_span"] = kMaxSpan;
  JsonObject l = out["limits"].to<JsonObject>();
  l["components"] = kMaxComponents;
  l["tabs"] = kMaxTabs;
  l["options"] = kMaxOptions;
  l["group_devices"] = kMaxGroupDevices;
  l["label"] = kLabelCap - 1;
  l["tab_name"] = kTabCap - 1;
  JsonArray types = out["types"].to<JsonArray>();
  for (uint8_t i = 0; i < uint8_t(CompType::kCount); i++) {
    CompType t = CompType(i);
    Need need = compNeed(t);
    JsonObject o = types.add<JsonObject>();
    o["name"] = compTypeName(t);
    o["need"] = needName(need);
    uint8_t w, h;
    compMinSize(t, w, h);
    o["min_w"] = w;
    o["min_h"] = h;
    JsonArray props = o["props"].to<JsonArray>();
    for (uint8_t p = 1; p < uint8_t(Prop::kCount); p++)
      if (needAccepts(need, Prop(p), true)) props.add(propName(Prop(p)));
  }
}

}  // namespace DashboardStore
}  // namespace espwf
