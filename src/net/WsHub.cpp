#include "WsHub.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "core/Framework.h"
#include "devices/CommandJson.h"
#include "devices/DeviceRegistry.h"
#include "net/Token.h"
#include "security/AuthManager.h"

namespace espwf {

size_t WsHub::clientCount() const { return ws_ ? ws_->count() : 0; }

WsHub::Slot* WsHub::slotFor(uint32_t id) {
  for (Slot& s : slots_)
    if (s.id == id) return &s;
  return nullptr;
}

void WsHub::attach(AsyncWebServer& server, FrameworkController& fw, DeviceRegistry& devices, AuthManager& auth) {
  auth_ = &auth;
  fw_ = &fw;
  dev_ = &devices;
  ws_ = new AsyncWebSocket("/ws");  // lives for the whole program, like the server
  ws_->onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
      case WS_EVT_CONNECT:
        if (ws_->count() > kMaxClients) { c->close(1013, "busy"); break; }  // the newest client is the one refused
        if (Slot* s = slotFor(0)) {
          *s = Slot();
          s->id = c->id();
          if (arg) tokenFrom(static_cast<AsyncWebServerRequest*>(arg), s->tok);  // the handshake carried the session
        }
        c->text("{\"event\":\"hello\",\"proto\":1}");
        break;
      case WS_EVT_DISCONNECT:
        if (Slot* s = slotFor(c->id())) s->id = 0;
        break;
      case WS_EVT_DATA: {
        Slot* s = slotFor(c->id());
        if (!s || !auth_->valid(s->tok)) { c->close(1008, "login required"); break; }  // valid() also renews: a busy socket keeps its session
        onData(c, arg, data, len);
        break;
      }
        break;
      default: break;
    }
  });
  server.addHandler(ws_);
  seenState_ = dev_->stateVersion();
  seenStruct_ = dev_->structureVersion();
  scanStates(false);
}

void WsHub::reply(AsyncWebSocketClient* c, const char* error, const char* field, const char* id) {
  JsonDocument d;
  d["event"] = "error";
  d["error"] = error;
  if (field && *field) d["field"] = field;
  if (id) d["id"] = id;
  String s;
  serializeJson(d, s);
  c->text(s);
}

void WsHub::onData(AsyncWebSocketClient* c, void* arg, uint8_t* data, size_t len) {
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) return;  // single text frames only

  Slot* slot = slotFor(c->id());
  if (slot && !slot->limiter.allow(millis(), kMaxMsgsPerSec, 1000)) {
    if (!slot->warned) { slot->warned = true; reply(c, "rate_limited", nullptr, nullptr); }  // one warning, not a flood
    return;
  }
  if (slot) slot->warned = false;

  if (len > kMaxMsgBytes) { reply(c, "too_large", nullptr, nullptr); return; }
  JsonDocument d;
  if (deserializeJson(d, data, len)) { reply(c, "bad_json", nullptr, nullptr); return; }

  const char* type = d["type"].is<const char*>() ? d["type"].as<const char*>() : "";
  if (!strcmp(type, "ping")) {
    c->text("{\"event\":\"pong\"}");
  } else if (!strcmp(type, "command")) {
    const char* id = nullptr;
    Command cmd;
    CmdErr e = parseCommandJson(d, id, cmd);
    if (e == CmdErr::Ok) e = dev_->requestCommand(id, cmd);
    if (e != CmdErr::Ok) reply(c, cmdErrCode(e), cmdErrField(e), id);  // success needs no reply: device_state follows
  } else {
    reply(c, "bad_type", "type", nullptr);
  }
}

// Compares every device with what was last sent. send=false only refreshes the baseline.
void WsHub::scanStates(bool send) {
  size_t n = dev_->count();
  DeviceView v;
  for (size_t i = 0; i < n && i < kMaxDevices; i++) {
    if (!dev_->view(i, v)) break;
    Last now;
    now.on = v.on; now.duty = v.duty; now.edges = v.edges;
    Last& was = last_[i];
    if (now.on == was.on && now.duty == was.duty && now.edges == was.edges) continue;
    if (send && !ws_->availableForWriteAll()) { dirty_ = true; return; }  // queue full: keep the old baseline, retry next pass
    was = now;
    if (!send) continue;
    JsonDocument d;
    d["event"] = "device_state";
    d["id"] = v.cfg.id;
    DeviceRegistry::describeState(v, d["state"].to<JsonObject>());
    String s;
    serializeJson(d, s);
    ws_->textAll(s);
  }
}

void WsHub::loop() {
  if (!ws_) return;
  ws_->cleanupClients(kMaxClients);
  uint32_t t = millis();
  if (int32_t(t - nextAuthCheck_) >= 0) {  // a session that ended (expired, logged out, password changed) closes its sockets
    nextAuthCheck_ = t + 3000;
    for (Slot& s : slots_)
      if (s.id && !auth_->alive(s.tok)) {
        if (AsyncWebSocketClient* c = ws_->client(s.id)) c->close(1008, "session ended");
      }
  }
  bool anyone = ws_->count() > 0;
  if (anyone && !ws_->availableForWriteAll()) return;  // a client's send queue is full: try again next pass, lose nothing

  uint32_t st = dev_->structureVersion();
  if (st != seenStruct_) {
    seenStruct_ = st;
    seenState_ = dev_->stateVersion();
    scanStates(false);  // new baseline: the list refetch brings clients up to date
    if (anyone) ws_->textAll("{\"event\":\"devices_changed\"}");
  } else {
    uint32_t sv = dev_->stateVersion();
    if (sv != seenState_ || dirty_) {
      seenState_ = sv;
      dirty_ = false;
      scanStates(anyone);
    }
  }

  uint32_t now = millis();
  if (anyone && int32_t(now - nextStatus_) >= 0) {
    nextStatus_ = now + kStatusEveryMs;
    JsonDocument d;
    JsonObject o = d.to<JsonObject>();
    o["event"] = "status";
    fw_->getStatus(o);
    String s;
    serializeJson(d, s);
    ws_->textAll(s);
  }
}

}  // namespace espwf
