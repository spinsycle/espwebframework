#include "MqttManager.h"

#include <Arduino.h>
#include <AsyncMqttClient.h>
#include <new>

#include "core/EventLog.h"
#include "devices/DeviceRegistry.h"
#include "hal/Hal.h"
#include "net/MqttStore.h"
#include "net/NetworkManager.h"

namespace espwf {

#if defined(ESP8266)
static constexpr uint32_t kMinHeapToConnect = 8000;  // the ESP8266 has about 15 KB free: do not start a connection it cannot afford
#else
static constexpr uint32_t kMinHeapToConnect = 20000;
#endif
static constexpr uint32_t kConnectTimeoutMs = 15000;

void MqttManager::buildNames() {
  snprintf(clientId_, sizeof clientId_, "%s", net_->config().hostname);
  mqttTopic(willTopic_, sizeof willTopic_, cfg_.base, nullptr, "status");
}

void MqttManager::begin(const MqttConfig& cfg, DeviceRegistry& devices, NetworkManager& net) {
  cfg_ = cfg;
  dev_ = &devices;
  net_ = &net;
  buildNames();
}

MqttCfgError MqttManager::requestApply(const MqttConfig& cfg) {
  MqttCfgError e = validateMqttConfig(cfg);
  if (e != MqttCfgError::Ok) return e;
  MqttConfig* p = new (std::nothrow) MqttConfig(cfg);
  if (!p) return MqttCfgError::Format;  // no memory: report it rather than drop the request silently
  MqttConfig* old;
  {
    hal::CriticalSection cs;
    old = pending_;
    pending_ = p;
  }
  delete old;  // a newer request replaces one that was never applied
  return MqttCfgError::Ok;
}

void MqttManager::applyPending() {
  MqttConfig* p;
  {
    hal::CriticalSection cs;
    p = pending_;
    pending_ = nullptr;
  }
  if (!p) return;
  stop();
  cfg_ = *p;
  delete p;
  buildNames();
  attempt_ = 0;
  nextTry_ = millis() + 1000;  // let a stale disconnect callback from the old connection arrive first
  bool saved = MqttStore::save(cfg_);
  logEventf("mqtt", "settings %s, %s", saved ? "saved" : "NOT saved", cfg_.enabled ? "enabled" : "disabled");
}

void MqttManager::ensureClient() {
  if (client_) return;
  rt_ = new Rt();
  client_ = new AsyncMqttClient();
  client_->onConnect([this](bool) { gotConn_ = true; });
  client_->onDisconnect([this](AsyncMqttClientDisconnectReason r) {
    discReason_ = int(r);
    gotDisc_ = true;
  });
  client_->onMessage([this](char* topic, char* payload, AsyncMqttClientMessageProperties pr, size_t len, size_t idx, size_t total) {
    // Async task: copy and queue, nothing else. A retained command is never obeyed (it would replay an old
    // "on" after every reconnect); a message in several pieces is longer than any command we accept.
    if (pr.retain || idx != 0 || len != total || len >= kTextCap) { dropped_++; return; }
    Msg m;
    if (!mqttParseSetTopic(cfg_.base, topic, m.id)) return;
    m.len = uint8_t(len);
    memcpy(m.payload, payload, len);
    hal::CriticalSection cs;
    if (!rt_->inbox.push(m)) dropped_++;
  });
}

void MqttManager::startConnect() {
  ensureClient();
  gotConn_ = gotDisc_ = false;
  client_->setServer(cfg_.host, cfg_.port);
  client_->setClientId(clientId_);
  client_->setCredentials(cfg_.user[0] ? cfg_.user : nullptr, cfg_.pass[0] ? cfg_.pass : nullptr);
  client_->setKeepAlive(cfg_.keepAlive);
  client_->setWill(willTopic_, 1, true, "offline");
  client_->connect();
  state_ = State::Connecting;
  connStart_ = millis();
}

void MqttManager::onConnected() {
  state_ = State::Connected;
  attempt_ = 0;
  lastError_ = "";
  char t[kMqttTopicCap];
  snprintf(t, sizeof t, "%s/+/set", cfg_.base);
  client_->subscribe(t, 0);
  client_->publish(willTopic_, 1, true, "online");
  for (size_t i = 0; i < kMax; i++) rt_->sent[i] = false;  // a fresh session: publish every state again
  structSeen_ = dev_->structureVersion();
  logEvent("mqtt", "connected to broker");
}

void MqttManager::stop() {
  if (client_ && (state_ == State::Connecting || state_ == State::Connected)) {
    client_->disconnect(true);  // forced: the broker then publishes our last will ("offline")
  }
  state_ = State::Off;
  gotConn_ = gotDisc_ = false;
  if (rt_) {
    hal::CriticalSection cs;
    Msg m;
    while (rt_->inbox.pop(m)) {}
  }
}

void MqttManager::fail(const char* why, uint32_t now) {
  state_ = State::Idle;
  lastError_ = why;
  nextTry_ = now + mqttBackoffMs(attempt_++);
}

void MqttManager::drainMessages() {
  for (int k = 0; k < 4; k++) {
    Msg m;
    {
      hal::CriticalSection cs;
      if (!rt_->inbox.pop(m)) return;
    }
    received_++;
    DeviceConfig dc;
    Command cmd;
    if (!dev_->config(m.id, dc) || !mqttParseCommand(dc.type, m.payload, m.len, cmd)) {
      cmdRejected_++;
      lastCmdError_ = "bad_command";
      continue;
    }
    CmdErr e = dev_->requestCommand(m.id, cmd);  // the same door REST, WebSocket and rules use
    if (e == CmdErr::Ok) { cmdOk_++; } else { cmdRejected_++; lastCmdError_ = cmdErrCode(e); }
  }
}

void MqttManager::publishChanged() {
  if (dev_->structureVersion() != structSeen_) {  // a device was added or removed: indexes moved, republish all
    structSeen_ = dev_->structureVersion();
    for (size_t i = 0; i < kMax; i++) rt_->sent[i] = false;
  }
  size_t n = dev_->count();
  for (size_t i = 0; i < n && i < kMax; i++) {
    DeviceView v;
    if (!dev_->view(i, v)) continue;
    MqttSig s = mqttSig(v.on, v.duty, v.val, v.edges, v.longs, v.ok, v.tripped,
                          v.cfg.type == DeviceType::Sensor && v.cfg.model == 2 ? v.dist : v.temp, v.hum, v.press);  // ping: distance rides in the temp slot
    if (rt_->sent[i] && mqttSigSame(rt_->sigs[i], s)) continue;
    char topic[kMqttTopicCap], buf[200];
    JsonDocument d;
    DeviceRegistry::describeState(v, d.to<JsonObject>());
    size_t len = serializeJson(d, buf, sizeof buf);
    if (len == 0 || len >= sizeof buf || !mqttTopic(topic, sizeof topic, cfg_.base, v.cfg.id, "state")) {
      rt_->sigs[i] = s; rt_->sent[i] = true;  // cannot be published; do not retry every pass
      continue;
    }
    if (client_->publish(topic, 0, true, buf, len)) {
      rt_->sigs[i] = s; rt_->sent[i] = true; published_++;
    }
    return;  // one message per pass keeps the heap spike small
  }
}

void MqttManager::loop() {
  if (!dev_) return;
  uint32_t now = millis();
  if (pending_) applyPending();

  bool want = cfg_.enabled && net_->staConnected();
  if (!want) {
    if (state_ != State::Off) stop();
    return;
  }
  if (state_ == State::Off) { state_ = State::Idle; if (int32_t(nextTry_ - now) < 0) nextTry_ = now; }

  if (gotDisc_ && (state_ == State::Connecting || state_ == State::Connected)) {
    gotDisc_ = false;
    gotConn_ = false;
    bool was = state_ == State::Connected;
    static char why[24];
    snprintf(why, sizeof why, "disconnected (%d)", discReason_);
    fail(why, now);
    if (was) logEventf("mqtt", "lost the broker (%d)", discReason_);
    return;
  }
  gotDisc_ = false;

  switch (state_) {
    case State::Idle:
      if (int32_t(now - nextTry_) < 0) return;
      if (hal::freeHeap() < kMinHeapToConnect) { fail("low_heap", now); return; }
      startConnect();
      break;
    case State::Connecting:
      if (gotConn_) { gotConn_ = false; onConnected(); }
      else if (now - connStart_ > kConnectTimeoutMs) { client_->disconnect(true); fail("connect_timeout", now); }
      break;
    case State::Connected:
      drainMessages();
      publishChanged();
      break;
    default: break;
  }
}

void MqttManager::status(JsonObject out) const {
  out["enabled"] = cfg_.enabled;
  out["connected"] = state_ == State::Connected;
  out["state"] = state_ == State::Connected ? "connected" : state_ == State::Connecting ? "connecting"
               : state_ == State::Idle ? "waiting" : "off";
  if (lastError_[0]) out["error"] = lastError_;
  out["published"] = published_;
  out["received"] = received_;
  out["commands_ok"] = cmdOk_;
  out["commands_rejected"] = cmdRejected_;
  out["dropped"] = dropped_;
  if (lastCmdError_[0]) out["last_command_error"] = lastCmdError_;
}

}  // namespace espwf
