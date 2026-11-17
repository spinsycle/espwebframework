#include "HttpApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <LittleFS.h>
#include <new>

#include "config/ConfigRules.h"
#include "core/Framework.h"
#include "core/Version.h"
#include "hal/Hal.h"
#include "automation/AutomationEngine.h"
#include "automation/AutomationStore.h"
#include "dashboard/DashboardStore.h"
#include "devices/CommandJson.h"
#include "devices/DeviceRegistry.h"
#include "devices/DeviceStore.h"
#include "net/NetworkStore.h"
#include "net/WsHub.h"

namespace espwf {

static void sendJson(AsyncWebServerRequest* r, JsonDocument& doc, int code = 200) {
  AsyncResponseStream* s = r->beginResponseStream("application/json");
  s->setCode(code);
  s->addHeader("Cache-Control", "no-store");
  serializeJson(doc, *s);
  r->send(s);
}

static void sendError(AsyncWebServerRequest* r, int code, const char* error, const char* field = nullptr) {
  JsonDocument d;
  d["error"] = error;
  if (field && *field) d["field"] = field;
  sendJson(r, d, code);
}

static int statusFor(DevErr e) { return e == DevErr::NotFound ? 404 : e == DevErr::Full ? 503 : 400; }

static void sendDevErr(AsyncWebServerRequest* r, DevErr e) {
  sendError(r, statusFor(e), devErrCode(e), devErrField(e));
}

static int statusFor(AutoErr e) {
  switch (e) {
    case AutoErr::NotFound: return 404;
    case AutoErr::Busy: return 503;
    case AutoErr::Duplicate:
    case AutoErr::TooMany: return 409;
    default: return 400;
  }
}

static void sendAutoErr(AsyncWebServerRequest* r, AutoErr e) { sendError(r, statusFor(e), autoErrCode(e), autoErrField(e)); }

void HttpApi::begin(FrameworkController& fw, NetworkManager& net, DeviceRegistry& devices, WsHub& ws, AutomationEngine& automation) {
  server_ = new AsyncWebServer(80);
  ws.attach(*server_, fw, devices);  // before the static-file handler, which would otherwise answer /ws
  FrameworkController* f = &fw;
  NetworkManager* n = &net;
  DeviceRegistry* dv = &devices;
  AutomationEngine* at = &automation;

  server_->on("/api/v1/system", HTTP_GET, [f](AsyncWebServerRequest* r) {
    JsonDocument d;
    f->getSystemInfo(d.to<JsonObject>());
    sendJson(r, d);
  });

  server_->on("/api/v1/status", HTTP_GET, [f](AsyncWebServerRequest* r) {
    JsonDocument d;
    f->getStatus(d.to<JsonObject>());
    sendJson(r, d);
  });

  // No auth until P8: anyone on the network can restart the board.
  server_->on("/api/v1/system/restart", HTTP_POST, [f](AsyncWebServerRequest* r) {
    f->restart();
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d);
  });

  // P1 interim network endpoints; P5 folds them into /api/v1/config.
  server_->on("/api/v1/network", HTTP_GET, [n](AsyncWebServerRequest* r) {
    JsonDocument d;
    NetworkStore::toJson(n->config(), d.to<JsonObject>(), false);
    sendJson(r, d);
  });

  auto* put = new AsyncCallbackJsonWebHandler("/api/v1/network", [n](AsyncWebServerRequest* r, JsonVariant& json) {
    Serial.printf("[http] PUT /network from %s\n", r->client()->remoteIP().toString().c_str());
    NetworkConfig c = n->config();
    NetCfgError e = NetworkStore::applyJson(json, c);
    if (e == NetCfgError::Ok) e = validateNetworkConfig(c);
    if (e == NetCfgError::Ok) e = n->requestApply(c);
    if (e != NetCfgError::Ok) {
      Serial.printf("[http] network config rejected: %s\n", netCfgErrorField(e));
      sendError(r, 400, "invalid_network_config", netCfgErrorField(e));
      return;
    }
    JsonDocument d;
    d["ok"] = true;
    d["apply_in_ms"] = NetworkManager::kApplyDelayMs;
    sendJson(r, d);
    Serial.println("[http] network config accepted, reply sent");
  });
  put->setMethod(HTTP_PUT);
  put->setMaxContentLength(1024);
  server_->addHandler(put);

  // ---- devices. Config changes and commands are queued (202); loop() applies them.
  server_->on("/api/v1/devices", HTTP_GET, [dv](AsyncWebServerRequest* r) {
    AsyncResponseStream* s = r->beginResponseStream("application/json");
    s->addHeader("Cache-Control", "no-store");
    s->print("{\"devices\":[");
    DeviceView v;
    for (size_t i = 0; dv->view(i, v); i++) {  // one device per small document: no big heap block
      JsonDocument d;
      DeviceRegistry::describe(v, d.to<JsonObject>());
      if (i) s->print(',');
      serializeJson(d, *s);
    }
    s->print("]}");
    r->send(s);
  });

  server_->on("/api/v1/devices", HTTP_DELETE, [dv](AsyncWebServerRequest* r) {
    if (!r->hasParam("id")) { sendError(r, 400, "bad_id", "id"); return; }
    DevErr e = dv->requestRemove(r->getParam("id")->value().c_str());
    if (e != DevErr::Ok) { sendDevErr(r, e); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });

  // ---- I2C bus scan: POST queues it, GET returns the last result (and whether one is running).
  // Edit in place: the body is merged onto the device named by "id" (type cannot change).
  auto* upd = new AsyncCallbackJsonWebHandler("/api/v1/devices", [dv](AsyncWebServerRequest* r, JsonVariant& json) {
    const char* id = json["id"].is<const char*>() ? json["id"].as<const char*>() : "";
    DeviceConfig c;
    if (!dv->config(id, c)) { sendDevErr(r, DevErr::NotFound); return; }
    bool hadInitial = !json["initial"].isNull();
    uint8_t oldInitial = c.initial;
    DevErr e = DeviceStore::applyJson(json, c);
    if (e == DevErr::Ok && !hadInitial) c.initial = oldInitial;  // applyJson would reset a servo to centre
    if (e == DevErr::Ok) e = dv->requestUpdate(c);
    if (e != DevErr::Ok) { sendDevErr(r, e); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });
  upd->setMethod(HTTP_PUT);
  upd->setMaxContentLength(512);
  server_->addHandler(upd);

  // ---- config: export, import (replace all devices), factory reset. No auth until P8.
  server_->on("/api/v1/config/export", HTTP_GET, [dv](AsyncWebServerRequest* r) {
    AsyncResponseStream* s = r->beginResponseStream("application/json");
    s->addHeader("Cache-Control", "no-store");
    s->addHeader("Content-Disposition", "attachment; filename=\"espwf-config.json\"");
    char v[16];
    versionString(v, sizeof v);
    s->printf("{\"schema_version\":%d,\"framework_version\":\"%s\",\"chip\":\"%s\",\"devices\":[", kConfigSchema, v, hal::chipName());
    DeviceView dvw;
    for (size_t i = 0; dv->view(i, dvw); i++) {  // config only, no live state, no Wi-Fi passwords (network is not part of it)
      JsonDocument d;
      DeviceStore::toJson(dvw.cfg, d.to<JsonObject>());
      if (i) s->print(',');
      serializeJson(d, *s);
    }
    s->print("]}");
    r->send(s);
  });

  auto* imp = new AsyncCallbackJsonWebHandler("/api/v1/config/import", [dv](AsyncWebServerRequest* r, JsonVariant& json) {
    if (!DeviceStore::schemaOk(json)) { sendError(r, 400, "unsupported_schema", "schema_version"); return; }
    JsonArrayConst arr = json["devices"].as<JsonArrayConst>();
    if (!json["devices"].is<JsonArrayConst>()) { sendError(r, 400, "bad_format", "devices"); return; }
    size_t n = arr.size();
    if (n > kMaxDevices) { sendError(r, 400, "too_many_devices", "devices"); return; }
    DeviceConfig* list = new (std::nothrow) DeviceConfig[n ? n : 1];
    if (!list) { sendError(r, 503, "out_of_memory"); return; }
    ImportResult res;
    size_t i = 0;
    for (JsonVariantConst item : arr) {
      DevErr e = DeviceStore::applyJson(item, list[i]);
      if (e != DevErr::Ok) { res.err = e; res.index = i; break; }
      i++;
    }
    if (res.err == DevErr::Ok) res = dv->requestImport(list, n);
    if (res.err != DevErr::Ok) {
      JsonDocument d;
      d["error"] = "import_rejected";
      d["index"] = res.index;
      d["id"] = res.index < n ? list[res.index].id : "";
      d["reason"] = devErrCode(res.err);
      d["field"] = devErrField(res.err);
      sendJson(r, d, 400);
      delete[] list;
      return;
    }
    delete[] list;
    JsonDocument d;
    d["ok"] = true;
    d["devices"] = n;
    d["restarting"] = true;
    sendJson(r, d, 202);
  });
  imp->setMethod(HTTP_POST);
  imp->setMaxContentLength(6144);
  server_->addHandler(imp);

  auto* reset = new AsyncCallbackJsonWebHandler("/api/v1/config/factory-reset", [f](AsyncWebServerRequest* r, JsonVariant& json) {
    const char* c = json["confirm"].is<const char*>() ? json["confirm"].as<const char*>() : "";
    if (strcmp(c, "factory-reset")) { sendError(r, 400, "confirm_required", "confirm"); return; }
    f->requestFactoryReset();
    JsonDocument d;
    d["ok"] = true;
    d["restarting"] = true;
    sendJson(r, d, 202);
  });
  reset->setMethod(HTTP_POST);
  reset->setMaxContentLength(64);
  server_->addHandler(reset);

  // ---- dashboard layout: the browser edits it, the firmware validates it against the devices and stores it.
  server_->on("/api/v1/dashboard/schema", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    DashboardStore::describeSchema(d.to<JsonObject>());
    sendJson(r, d);
  });

  server_->on("/api/v1/dashboard", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!LittleFS.exists("/config/dashboard.json")) {  // nothing saved yet: automatic mode
      JsonDocument d;
      d["schema_version"] = kConfigSchema;
      d["mode"] = "auto";
      d["tabs"].to<JsonArray>();
      d["components"].to<JsonArray>();
      sendJson(r, d);
      return;
    }
    AsyncWebServerResponse* resp = r->beginResponse(LittleFS, "/config/dashboard.json", "application/json");
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
  });

  auto* dash = new AsyncCallbackJsonWebHandler("/api/v1/dashboard", [dv](AsyncWebServerRequest* r, JsonVariant& json) {
    DashboardStore::Result res = DashboardStore::validate(json, *dv);
    if (res.err != DashErr::Ok) {
      JsonDocument d;
      d["error"] = "invalid_dashboard";
      d["reason"] = dashErrCode(res.err);
      d["field"] = dashErrField(res.err);
      d["component"] = res.index;
      d["id"] = res.id;
      sendJson(r, d, 400);
      return;
    }
    if (!DashboardStore::save(json)) { sendError(r, 500, "save_failed"); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d);
  });
  dash->setMethod(HTTP_PUT);
  dash->setMaxContentLength(6144);
  server_->addHandler(dash);

  // ---- automation: rules are validated against the devices, queued, and applied by loop(). No auth until P8.
  server_->on("/api/v1/automation/schema", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    JsonArray t = d["triggers"].to<JsonArray>();
    for (uint8_t i = 0; i < uint8_t(TrigType::kCount); i++) t.add(trigName(TrigType(i)));
    JsonArray o = d["operators"].to<JsonArray>();
    for (uint8_t i = 0; i < uint8_t(Op::kCount); i++) o.add(opName(Op(i)));
    JsonArray a = d["actions"].to<JsonArray>();
    for (uint8_t i = 0; i < uint8_t(ActType::kCount); i++) a.add(actName(ActType(i)));
    JsonObject l = d["limits"].to<JsonObject>();
    l["rules"] = kMaxRules;
    l["conditions"] = kMaxConds;
    l["actions"] = kMaxActions;
    l["interlocks"] = kMaxInterlocks;
    l["text"] = kActTextCap - 1;
    sendJson(r, d);
  });

  // The status route is registered before the config route: this server matches a route as a prefix of longer paths.
  server_->on("/api/v1/automation/status", HTTP_GET, [at](AsyncWebServerRequest* r) {
    AsyncResponseStream* s = r->beginResponseStream("application/json");
    s->addHeader("Cache-Control", "no-store");
    {
      JsonDocument d;
      at->describeStatusHead(d.to<JsonObject>());
      String head;
      serializeJson(d, head);
      head.remove(head.length() - 1);
      s->print(head);
      s->print(",\"status\":[");
    }
    for (size_t i = 0;; i++) {
      JsonDocument d;
      if (!at->describeRule(i, d.to<JsonObject>())) break;
      if (i) s->print(',');
      serializeJson(d, *s);
    }
    s->print("],\"events\":[");
    for (size_t i = 0;; i++) {
      JsonDocument d;
      if (!at->describeEvent(i, d.to<JsonObject>())) break;
      if (i) s->print(',');
      serializeJson(d, *s);
    }
    s->print("]}");
    r->send(s);
  });

  server_->on("/api/v1/automation", HTTP_GET, [at](AsyncWebServerRequest* r) {
    AsyncResponseStream* s = r->beginResponseStream("application/json");
    s->addHeader("Cache-Control", "no-store");
    {
      JsonDocument d;
      AutomationStore::settingsToJson(at->settings(), d.to<JsonObject>());
      d["schema_version"] = kConfigSchema;
      String head;
      serializeJson(d, head);
      head.remove(head.length() - 1);  // without the closing brace: the rules follow
      s->print(head);
      s->print(",\"rules\":[");
    }
    Rule* rule = new (std::nothrow) Rule;  // one rule at a time, off the small stack
    for (size_t i = 0; rule && at->ruleAt(i, *rule); i++) {
      JsonDocument d;
      AutomationStore::ruleToJson(*rule, d.to<JsonObject>());
      if (i) s->print(',');
      serializeJson(d, *s);
    }
    delete rule;
    s->print("]}");
    r->send(s);
  });

  auto* settings = new AsyncCallbackJsonWebHandler("/api/v1/automation/settings", [at](AsyncWebServerRequest* r, JsonVariant& json) {
    AutoSettings st = at->settings();  // merge: only the fields sent change
    AutoErr e = AutomationStore::parseSettings(json, st);
    if (e == AutoErr::Ok) e = at->requestSettings(st);
    if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });
  settings->setMethod(HTTP_PUT);
  settings->setMaxContentLength(512);
  server_->addHandler(settings);

  // Create (POST) and replace (PUT) take a whole rule; replace needs the id of an existing one.
  for (int replace = 0; replace < 2; replace++) {
    auto* h = new AsyncCallbackJsonWebHandler("/api/v1/automation/rules", [at, replace](AsyncWebServerRequest* r, JsonVariant& json) {
      Rule* rule = new (std::nothrow) Rule;
      if (!rule) { sendError(r, 503, "out_of_memory"); return; }
      AutoErr e = AutomationStore::parseRule(json, *rule);
      if (e == AutoErr::Ok) e = replace ? at->requestReplace(*rule) : at->requestAdd(*rule);
      delete rule;
      if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
      JsonDocument d;
      d["ok"] = true;
      sendJson(r, d, 202);
    });
    h->setMethod(replace ? HTTP_PUT : HTTP_POST);
    h->setMaxContentLength(1536);
    server_->addHandler(h);
  }

  server_->on("/api/v1/automation/rules", HTTP_DELETE, [at](AsyncWebServerRequest* r) {
    if (!r->hasParam("id")) { sendError(r, 400, "bad_id", "id"); return; }
    AutoErr e = at->requestRemove(r->getParam("id")->value().c_str());
    if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });

  // enable / disable / run: POST with ?id=
  struct Act { const char* path; int kind; };
  static const Act acts[] = {{"/api/v1/automation/rules/enable", 1}, {"/api/v1/automation/rules/disable", 0}, {"/api/v1/automation/rules/run", 2}};
  for (const Act& a : acts) {
    int kind = a.kind;
    server_->on(a.path, HTTP_POST, [at, kind](AsyncWebServerRequest* r) {
      if (!r->hasParam("id")) { sendError(r, 400, "bad_id", "id"); return; }
      const char* id = r->getParam("id")->value().c_str();
      AutoErr e = kind == 2 ? at->requestRun(id) : at->requestEnable(id, kind == 1);
      if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
      JsonDocument d;
      d["ok"] = true;
      sendJson(r, d, 202);
    });
  }

  server_->on("/api/v1/i2c", HTTP_GET, [dv](AsyncWebServerRequest* r) {
    JsonDocument d;
    dv->describeScan(d.to<JsonObject>());
    sendJson(r, d);
  });
  server_->on("/api/v1/i2c/scan", HTTP_POST, [dv](AsyncWebServerRequest* r) {
    ScanErr e = dv->requestScan();
    if (e == ScanErr::Busy) { sendError(r, 409, "busy", ""); return; }
    if (e == ScanErr::PinsInUse) { sendError(r, 409, "pin_in_use", "pin"); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });

  auto* add = new AsyncCallbackJsonWebHandler("/api/v1/devices", [dv](AsyncWebServerRequest* r, JsonVariant& json) {
    DeviceConfig c;
    DevErr e = DeviceStore::applyJson(json, c);
    if (e == DevErr::Ok) e = dv->requestAdd(c);
    if (e != DevErr::Ok) { sendDevErr(r, e); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });
  add->setMethod(HTTP_POST);
  add->setMaxContentLength(512);
  server_->addHandler(add);

  auto* command = new AsyncCallbackJsonWebHandler("/api/v1/command", [dv](AsyncWebServerRequest* r, JsonVariant& json) {
    const char* id = nullptr;
    Command c;
    CmdErr e = parseCommandJson(json, id, c);
    if (e == CmdErr::Ok) e = dv->requestCommand(id, c);
    if (e != CmdErr::Ok) {
      sendError(r, cmdErrHttp(e), cmdErrCode(e), cmdErrField(e));
      return;
    }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });
  command->setMethod(HTTP_POST);
  command->setMaxContentLength(256);
  server_->addHandler(command);

  server_->serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

  server_->onNotFound([](AsyncWebServerRequest* r) {
    if (r->url().startsWith("/api/")) {
      sendError(r, 404, "not_found");
    } else if (r->url() == "/") {
      r->send(503, "text/plain", "UI not installed. Run: pio run -t uploadfs");
    } else {
      r->send(404, "text/plain", "Not found");
    }
  });

  server_->begin();
}

}  // namespace espwf
