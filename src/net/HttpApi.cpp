#include "HttpApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <LittleFS.h>

#include "core/Framework.h"
#include "devices/DeviceRegistry.h"
#include "devices/DeviceStore.h"
#include "net/NetworkStore.h"

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

void HttpApi::begin(FrameworkController& fw, NetworkManager& net, DeviceRegistry& devices) {
  server_ = new AsyncWebServer(80);
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
    const char* id = json["id"].is<const char*>() ? json["id"].as<const char*>() : nullptr;
    Command c;
    if (!id || !parseAction(json["action"].as<const char*>(), c.action)) {
      sendError(r, 400, id ? "bad_action" : "bad_id", id ? "action" : "id");
      return;
    }
    JsonVariantConst val = json["value"];
    if (!val.isNull()) {
      c.hasValue = true;
      c.isBool = val.is<bool>();
      c.value = c.isBool ? (val.as<bool>() ? 1 : 0) : val.is<double>() ? val.as<double>() : 0;
      if (!c.isBool && !val.is<double>()) c.hasValue = false;  // a string or object is "no usable value"
    }
    CmdErr e = dv->requestCommand(id, c);
    if (e != CmdErr::Ok) {
      int code = e == CmdErr::NotFound ? 404 : e == CmdErr::NotCommandable ? 409 : e == CmdErr::QueueFull ? 503 : 400;
      const char* name = e == CmdErr::NotFound ? "not_found" : e == CmdErr::NotCommandable ? "not_commandable"
                       : e == CmdErr::QueueFull ? "busy" : e == CmdErr::BadAction ? "bad_action" : "bad_value";
      sendError(r, code, name, e == CmdErr::BadValue ? "value" : e == CmdErr::BadAction ? "action" : "id");
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
