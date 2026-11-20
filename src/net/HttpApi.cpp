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
#include "net/MqttManager.h"
#include "net/MqttStore.h"
#include "net/NetworkStore.h"
#include "core/EventLog.h"
#include "devices/PinInfo.h"
#include "net/Chunker.h"
#include "net/Ota.h"
#include "net/Token.h"
#include "net/WsHub.h"
#include "security/AuthManager.h"

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


// State of the firmware upload in progress (one at a time; the upload callback and the final one share it).
static bool gOtaFailed = false, gOtaStarted = false, gOtaDone = false;
static char gOtaErr[56] = {0};
static void otaFail(const char* why) {
  gOtaFailed = true;
  copyStr(gOtaErr, why, sizeof gOtaErr);
  if (gOtaStarted) { espwf::Ota::abort(); gOtaStarted = false; }
}


// ---- big replies are built a piece at a time (see Chunker.h): one JSON document per list item

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

void HttpApi::begin(FrameworkController& fw, NetworkManager& net, DeviceRegistry& devices, WsHub& ws, AutomationEngine& automation, AuthManager& auth,
                    MqttManager& mqtt) {
  server_ = new AsyncWebServer(80);
  MqttManager* mq = &mqtt;
  ws.attach(*server_, fw, devices, auth);  // before the static-file handler, which would otherwise answer /ws
  FrameworkController* f = &fw;
  NetworkManager* n = &net;
  DeviceRegistry* dv = &devices;
  AutomationEngine* at = &automation;
  AuthManager* au = &auth;

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

  // ---- P8b: the controller's heartbeat, a read-only look at the board, and the system event log.
  // Any logged-in client may say "I am still here": every failsafe timer starts over. Nothing is queued or saved.
  server_->on("/api/v1/heartbeat", HTTP_POST, [dv](AsyncWebServerRequest* r) {
    dv->heartbeat();
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d);
  });

  server_->on("/api/v1/diagnostics", HTTP_GET, [f, dv](AsyncWebServerRequest* r) {
    String head;
    {
      JsonDocument d;
      JsonObject o = d.to<JsonObject>();
      f->getStatus(o);
      f->getHealth(o);
      o.remove("network");  // the Network tab already has it, and the reply stays small
      serializeJson(d, head);
      head.remove(head.length() - 1);  // without the closing brace: the lists follow
    }
    auto c = std::make_shared<Chunker>(head);
    c->add(",\"devices\":[", [dv](size_t i, String& out) {
      DeviceView v;
      if (!dv->view(i, v)) return false;
      JsonDocument d;
      JsonObject o = d.to<JsonObject>();
      o["id"] = v.cfg.id;
      o["type"] = deviceTypeName(v.cfg.type);
      o["enabled"] = v.cfg.enabled;
      if (v.cfg.failsafeS) { o["failsafe_s"] = v.cfg.failsafeS; o["tripped"] = v.tripped; }
      if (v.cfg.type == DeviceType::Sensor) { o["ok"] = v.ok; o["errors"] = v.errs; }
      serializeJson(d, out);
      return true;
    });
    c->add(",\"events\":[", [](size_t i, String& out) {
      SysEvent e;
      if (!eventNewest(i, e)) return false;
      JsonDocument d;
      eventToJson(e, d.to<JsonObject>());
      serializeJson(d, out);
      return true;
    });
    sendChunked(r, c);
  });

  // Every pin the chip has: its label, why it needs care, which device uses it, and its level right now.
  // Reading a pin never changes it. There is no way to write a pin from here.
  server_->on("/api/v1/gpio", HTTP_GET, [dv](AsyncWebServerRequest* r) {
#if defined(ESP8266)
    const Chip chip = Chip::Esp8266;
    const int last = 16;
#else
    const Chip chip = Chip::Esp32;
    const int last = 39;
#endif
    char head[64];
    snprintf(head, sizeof head, "{\"chip\":\"%s\",\"read_only\":true", hal::chipName());
    auto c = std::make_shared<Chunker>(head);
    auto next = std::make_shared<int>(0);  // the next GPIO number to look at: unlisted numbers are skipped
    c->add(",\"pins\":[", [dv, chip, last, next](size_t, String& out) {
      while (*next <= last && !pinListed(chip, *next)) (*next)++;
      if (*next > last) return false;
      int g = (*next)++;
      JsonDocument d;
      JsonObject o = d.to<JsonObject>();
      o["gpio"] = g;
      o["label"] = pinLabel(chip, g);
      o["note"] = pinNote(chip, g);
      DeviceView v;
      for (size_t k = 0; dv->view(k, v); k++) {
        int8_t p[5];
        size_t np = pinsOf(v.cfg, chip, p);
        bool mine = false;
        for (size_t j = 0; j < np; j++) mine = mine || p[j] == g;
        if (mine) { o["use"] = v.cfg.id; break; }
      }
      if (pinReadable(chip, g)) o["level"] = digitalRead(g) ? 1 : 0;
      serializeJson(d, out);
      return true;
    });
    sendChunked(r, c);
  });

  // Hangs the main loop on purpose so the chip's watchdog resets the board: proves the reset is reported.
  auto* wdt = new AsyncCallbackJsonWebHandler("/api/v1/system/test-watchdog", [f](AsyncWebServerRequest* r, JsonVariant& json) {
#if defined(ESP8266)
    if (!json["confirm"].is<const char*>() || strcmp(json["confirm"].as<const char*>(), "hang") != 0) { sendError(r, 400, "confirm_required", "confirm"); return; }
    JsonDocument d;
    d["ok"] = true;
    d["note"] = "the board hangs now and the watchdog restarts it in a few seconds";
    sendJson(r, d);
    f->requestWatchdogTest();
#else
    (void)json;
    (void)f;
    sendError(r, 501, "not_supported", "chip");
#endif
  });
  wdt->setMethod(HTTP_POST);
  wdt->setMaxContentLength(64);
  server_->addHandler(wdt);

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

  // ---- MQTT. The password is never returned (password_set); a PUT without one keeps the stored one.
  server_->on("/api/v1/mqtt", HTTP_GET, [mq](AsyncWebServerRequest* r) {
    JsonDocument d;
    MqttStore::toJson(mq->config(), d.to<JsonObject>(), false);
    mq->status(d["status"].to<JsonObject>());
    sendJson(r, d);
  });

  auto* mput = new AsyncCallbackJsonWebHandler("/api/v1/mqtt", [mq](AsyncWebServerRequest* r, JsonVariant& json) {
    MqttConfig c = mq->config();
    MqttCfgError e = MqttStore::applyJson(json, c);
    if (e == MqttCfgError::Ok) e = mq->requestApply(c);
    if (e != MqttCfgError::Ok) { sendError(r, 400, "invalid_mqtt_config", mqttCfgErrorField(e)); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d);
  });
  mput->setMethod(HTTP_PUT);
  mput->setMaxContentLength(512);
  server_->addHandler(mput);

  // ---- devices. Config changes and commands are queued (202); loop() applies them.
  server_->on("/api/v1/devices", HTTP_GET, [dv](AsyncWebServerRequest* r) {
    auto c = std::make_shared<Chunker>("{");
    c->add("\"devices\":[", [dv](size_t i, String& out) {
      DeviceView v;
      if (!dv->view(i, v)) return false;
      JsonDocument d;
      DeviceRegistry::describe(v, d.to<JsonObject>());
      serializeJson(d, out);
      return true;
    });
    sendChunked(r, c);
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
    char v[16];
    versionString(v, sizeof v);
    char head[120];
    snprintf(head, sizeof head, "{\"schema_version\":%d,\"framework_version\":\"%s\",\"chip\":\"%s\"", kConfigSchema, v, hal::chipName());
    auto c = std::make_shared<Chunker>(head);
    c->add(",\"devices\":[", [dv](size_t i, String& out) {  // config only, no live state, no Wi-Fi passwords (network is not part of it)
      DeviceView dvw;
      if (!dv->view(i, dvw)) return false;
      JsonDocument d;
      DeviceStore::toJson(dvw.cfg, d.to<JsonObject>());
      serializeJson(d, out);
      return true;
    });
    sendChunked(r, c, "Content-Disposition", "attachment; filename=\"espwf-config.json\"");
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

  auto* dash = new AsyncCallbackJsonWebHandler("/api/v1/dashboard", [dv, at](AsyncWebServerRequest* r, JsonVariant& json) {
    DashboardStore::Result res = DashboardStore::validate(json, *dv, at);
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
    if (hal::fsBusy()) { sendError(r, 503, "busy"); return; }  // the main loop is writing a file: try again in a moment
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
    l["sequences"] = kMaxSeqs;
    l["steps"] = kMaxSteps;
    l["steps_total"] = kMaxStepsTotal;
    l["timers"] = kMaxTimers;
    JsonArray st = d["step_types"].to<JsonArray>();
    for (uint8_t i = 0; i < uint8_t(StepType::kCount); i++) st.add(stepName(StepType(i)));
    sendJson(r, d);
  });

  // The status route is registered before the config route: this server matches a route as a prefix of longer paths.
  // ---- login. Everything under /api/ and the WebSocket needs a session, except /api/v1/session itself.
  // The UI files stay public: they hold no secrets, and the login page has to load.
  server_->addMiddleware([au](AsyncWebServerRequest* r, ArMiddlewareNext next) {
    const String& u = r->url();
    bool guarded = u.startsWith("/api/") || u == "/ws";
    if (!guarded || u == "/api/v1/session") { next(); return; }
    char tok[kTokenChars + 1];
    if (!tokenFrom(r, tok) || !au->valid(tok)) { sendError(r, 401, "unauthorized"); return; }
    if (au->mustChange() && u != "/api/v1/password") { sendError(r, 403, "password_change_required"); return; }
    next();
  });

  server_->on("/api/v1/session", HTTP_GET, [au, f](AsyncWebServerRequest* r) {
    char tok[kTokenChars + 1];
    bool ok = tokenFrom(r, tok) && au->valid(tok);
    JsonDocument d;
    d["authenticated"] = ok;
    d["must_change"] = ok && au->mustChange();
    d["name"] = f->deviceName();
    sendJson(r, d);
  });

  server_->on("/api/v1/session", HTTP_DELETE, [au](AsyncWebServerRequest* r) {
    char tok[kTokenChars + 1];
    if (tokenFrom(r, tok)) au->logout(tok);
    AsyncResponseStream* s = r->beginResponseStream("application/json");
    s->addHeader("Cache-Control", "no-store");
    s->addHeader("Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    s->print("{\"ok\":true}");
    r->send(s);
  });

  auto* login = new AsyncCallbackJsonWebHandler("/api/v1/session", [au](AsyncWebServerRequest* r, JsonVariant& json) {
    const char* pw = json["password"].is<const char*>() ? json["password"].as<const char*>() : "";
    char tok[kTokenChars + 1];
    AuthErr e = au->login(pw, tok);
    if (e == AuthErr::Locked) {
      JsonDocument d;
      d["error"] = "locked";
      d["retry_s"] = (au->lockedForMs() + 999) / 1000;
      sendJson(r, d, 429);
      return;
    }
    if (e != AuthErr::Ok) { sendError(r, 401, authErrCode(e)); return; }
    AsyncResponseStream* s = r->beginResponseStream("application/json");
    s->addHeader("Cache-Control", "no-store");
    s->addHeader("Set-Cookie", String("sid=") + tok + "; Path=/; HttpOnly; SameSite=Strict");  // a session cookie: the board enforces the idle time
    JsonDocument d;
    d["ok"] = true;
    d["token"] = tok;  // for scripts; the browser uses the cookie
    d["must_change"] = au->mustChange();
    serializeJson(d, *s);
    r->send(s);
  });
  login->setMethod(HTTP_POST);
  login->setMaxContentLength(160);
  server_->addHandler(login);

  auto* pwchange = new AsyncCallbackJsonWebHandler("/api/v1/password", [au](AsyncWebServerRequest* r, JsonVariant& json) {
    const char* cur = json["current"].is<const char*>() ? json["current"].as<const char*>() : "";
    const char* nxt = json["password"].is<const char*>() ? json["password"].as<const char*>() : "";
    char tok[kTokenChars + 1];
    tokenFrom(r, tok);
    if (hal::fsBusy()) { sendError(r, 503, "busy"); return; }  // see hal::FsWrite
    AuthErr e = au->changePassword(cur, nxt, tok);
    if (e == AuthErr::Ok) {
      JsonDocument d;
      d["ok"] = true;
      sendJson(r, d);
    } else if (e == AuthErr::Locked) {
      sendError(r, 429, "locked");
    } else if (e == AuthErr::Wrong) {
      sendError(r, 403, "wrong_password", "current");
    } else {
      sendError(r, 400, authErrCode(e), "password");
    }
  });
  pwchange->setMethod(HTTP_PUT);
  pwchange->setMaxContentLength(220);
  server_->addHandler(pwchange);

  // ---- firmware update: multipart upload of the .bin; ?md5=<32 hex> is checked when the last byte arrives.
  // The upload callback runs before the middleware's final check, so it asks for a session itself.
  server_->on(
      "/api/v1/system/update", HTTP_POST,
      [f](AsyncWebServerRequest* r) {
        JsonDocument d;
        if (!gOtaFailed && gOtaDone) {
          d["ok"] = true;
          d["restarting"] = true;
          sendJson(r, d);
          logEvent("ota", "installed, restarting");
          f->restart();
        } else {
          logEventf("ota", "failed: %s", gOtaErr[0] ? gOtaErr : "no firmware");
          d["error"] = "update_failed";
          d["detail"] = gOtaErr[0] ? gOtaErr : "no firmware received";
          sendJson(r, d, 400);
        }
        gOtaFailed = gOtaStarted = gOtaDone = false;
      },
      [au](AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          gOtaFailed = gOtaStarted = gOtaDone = false;
          gOtaErr[0] = 0;
          char tok[kTokenChars + 1];
          if (!tokenFrom(r, tok) || !au->valid(tok) || au->mustChange()) otaFail("unauthorized");
          else if (espwf::Ota::busy()) otaFail("another update is running");
          else if (hal::fsBusy()) otaFail("the board is busy saving: try again in a moment");
          else if (!filename.endsWith(".bin")) otaFail("not a .bin file");
          else {
            String md5 = r->hasParam("md5") ? r->getParam("md5")->value() : String();
            if (!espwf::Ota::begin(r->contentLength(), md5.c_str())) otaFail(espwf::Ota::errorText());
            else gOtaStarted = true;
          }
        }
        if (gOtaFailed) return;
        if (!espwf::Ota::write(data, len)) { otaFail(espwf::Ota::errorText()); return; }
        if (final) {
          gOtaStarted = false;
          if (!espwf::Ota::end()) otaFail(espwf::Ota::errorText());
          else gOtaDone = true;
        }
      });

  // ---- sequences and timers. Longer paths first: this server matches a route as a prefix of longer ones.
  server_->on("/api/v1/automation/timers", HTTP_GET, [at](AsyncWebServerRequest* r) {  // light: the dashboard polls it
    auto c = std::make_shared<Chunker>("{");
    c->add("\"timers\":[", [at](size_t i, String& out) {
      JsonDocument d;
      if (!at->describeTimer(i, d.to<JsonObject>())) return false;
      serializeJson(d, out);
      return true;
    });
    sendChunked(r, c);
  });

  struct CtlRoute { const char* path; bool seq; AutomationCore::Ctl op; };
  static const CtlRoute ctlRoutes[] = {
      {"/api/v1/automation/sequences/start", true, AutomationCore::Ctl::SeqStart},
      {"/api/v1/automation/sequences/stop", true, AutomationCore::Ctl::SeqStop},
      {"/api/v1/automation/sequences/pause", true, AutomationCore::Ctl::SeqPause},
      {"/api/v1/automation/sequences/resume", true, AutomationCore::Ctl::SeqResume},
      {"/api/v1/automation/timers/start", false, AutomationCore::Ctl::TimerStart},
      {"/api/v1/automation/timers/stop", false, AutomationCore::Ctl::TimerStop},
  };
  for (const CtlRoute& c : ctlRoutes) {
    bool seq = c.seq;
    AutomationCore::Ctl op = c.op;
    server_->on(c.path, HTTP_POST, [at, seq, op](AsyncWebServerRequest* r) {
      if (!r->hasParam("id")) { sendError(r, 400, "bad_id", "id"); return; }
      const char* id = r->getParam("id")->value().c_str();
      AutoErr e = seq ? at->requestSeqCtl(op, id) : at->requestTimerCtl(op, id);
      if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
      JsonDocument d;
      d["ok"] = true;
      sendJson(r, d, 202);
    });
  }

  server_->on("/api/v1/automation/sequences", HTTP_DELETE, [at](AsyncWebServerRequest* r) {
    if (!r->hasParam("id")) { sendError(r, 400, "bad_id", "id"); return; }
    AutoErr e = at->requestSeqRemove(r->getParam("id")->value().c_str());
    if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });
  server_->on("/api/v1/automation/timers", HTTP_DELETE, [at](AsyncWebServerRequest* r) {
    if (!r->hasParam("id")) { sendError(r, 400, "bad_id", "id"); return; }
    AutoErr e = at->requestTimerRemove(r->getParam("id")->value().c_str());
    if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
    JsonDocument d;
    d["ok"] = true;
    sendJson(r, d, 202);
  });

  // Create (POST) and replace (PUT) take the whole sequence or timer.
  for (int replace = 0; replace < 2; replace++) {
    auto* sh = new AsyncCallbackJsonWebHandler("/api/v1/automation/sequences", [at, replace](AsyncWebServerRequest* r, JsonVariant& json) {
      SeqDef* q = new (std::nothrow) SeqDef;  // about 1.5 KB: off the small stack
      if (!q) { sendError(r, 503, "out_of_memory"); return; }
      AutoErr e = AutomationStore::parseSeq(json, *q);
      if (e == AutoErr::Ok) e = replace ? at->requestSeqReplace(*q) : at->requestSeqAdd(*q);
      delete q;
      if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
      JsonDocument d;
      d["ok"] = true;
      sendJson(r, d, 202);
    });
    sh->setMethod(replace ? HTTP_PUT : HTTP_POST);
    sh->setMaxContentLength(3072);
    server_->addHandler(sh);

    auto* th = new AsyncCallbackJsonWebHandler("/api/v1/automation/timers", [at, replace](AsyncWebServerRequest* r, JsonVariant& json) {
      TimerDef t;
      AutoErr e = AutomationStore::parseTimer(json, t);
      if (e == AutoErr::Ok) e = replace ? at->requestTimerReplace(t) : at->requestTimerAdd(t);
      if (e != AutoErr::Ok) { sendAutoErr(r, e); return; }
      JsonDocument d;
      d["ok"] = true;
      sendJson(r, d, 202);
    });
    th->setMethod(replace ? HTTP_PUT : HTTP_POST);
    th->setMaxContentLength(256);
    server_->addHandler(th);
  }

  server_->on("/api/v1/automation/status", HTTP_GET, [at](AsyncWebServerRequest* r) {
    String head;
    {
      JsonDocument d;
      at->describeStatusHead(d.to<JsonObject>());
      serializeJson(d, head);
      head.remove(head.length() - 1);  // without the closing brace: the lists follow
    }
    auto c = std::make_shared<Chunker>(head);
    c->add(",\"status\":[", [at](size_t i, String& out) { JsonDocument d; if (!at->describeRule(i, d.to<JsonObject>())) return false; serializeJson(d, out); return true; });
    c->add(",\"events\":[", [at](size_t i, String& out) { JsonDocument d; if (!at->describeEvent(i, d.to<JsonObject>())) return false; serializeJson(d, out); return true; });
    c->add(",\"sequences\":[", [at](size_t i, String& out) { JsonDocument d; if (!at->describeSeq(i, d.to<JsonObject>())) return false; serializeJson(d, out); return true; });
    c->add(",\"timers\":[", [at](size_t i, String& out) { JsonDocument d; if (!at->describeTimer(i, d.to<JsonObject>())) return false; serializeJson(d, out); return true; });
    sendChunked(r, c);
  });

  server_->on("/api/v1/automation", HTTP_GET, [at](AsyncWebServerRequest* r) {
    String head;
    {
      JsonDocument d;
      AutomationStore::settingsToJson(at->settings(), d.to<JsonObject>());
      d["schema_version"] = kConfigSchema;
      serializeJson(d, head);
      head.remove(head.length() - 1);  // without the closing brace: the lists follow
    }
    auto c = std::make_shared<Chunker>(head);
    c->add(",\"rules\":[", [at](size_t i, String& out) {
      std::unique_ptr<Rule> rule(new (std::nothrow) Rule);  // about 340 bytes, only for the moment it is written
      if (!rule || !at->ruleAt(i, *rule)) return false;
      JsonDocument d;
      AutomationStore::ruleToJson(*rule, d.to<JsonObject>());
      serializeJson(d, out);
      return true;
    });
    c->add(",\"timers\":[", [at](size_t i, String& out) {  // the stored definitions, plus the live state a timer card needs
      JsonDocument d;
      if (!at->describeTimer(i, d.to<JsonObject>())) return false;
      serializeJson(d, out);
      return true;
    });
    c->add(",\"sequences\":[", [at](size_t i, String& out) {
      std::unique_ptr<SeqDef> q(new (std::nothrow) SeqDef);  // about 1.5 KB, only for the moment it is written
      if (!q || !at->seqAt(i, *q)) return false;
      JsonDocument d;
      AutomationStore::seqToJson(*q, d.to<JsonObject>());
      serializeJson(d, out);
      return true;
    });
    sendChunked(r, c);
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
