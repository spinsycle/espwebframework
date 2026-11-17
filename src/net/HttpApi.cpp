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

void HttpApi::begin(FrameworkController& fw, NetworkManager& net, DeviceRegistry& devices, WsHub& ws) {
  server_ = new AsyncWebServer(80);
  ws.attach(*server_, fw, devices);  // before the static-file handler, which would otherwise answer /ws
  FrameworkController* f = &fw;
  NetworkManager* n = &net;
  DeviceRegistry* dv = &devices;

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
