#include "core/Features.h"
#if ESPWF_HAS_ROLE

#include "RoleManager.h"

#include <Arduino.h>
#include <new>
#if defined(ESP8266)
#include <ESPAsyncTCP.h>
#else
#include <AsyncTCP.h>
#endif

#include "core/EventLog.h"
#include "devices/DeviceRegistry.h"
#include "hal/Hal.h"
#include "net/HttpLite.h"
#include "net/NetworkManager.h"
#include "net/RoleStore.h"

namespace espwf {

#if defined(ESP8266)
static constexpr uint32_t kMinHeapToRequest = 7000;  // a TCP connection plus its buffers: do not start one the ESP8266 cannot afford
#else
static constexpr uint32_t kMinHeapToRequest = 20000;
#endif
static constexpr uint32_t kRequestTimeoutMs = 8000;
static constexpr uint32_t kPollMs = 20;
static constexpr uint32_t kBadPasswordRetryMs = 600000;  // five wrong passwords lock the master out: do not hammer it

void RoleManager::begin(const RoleConfig& cfg, DeviceRegistry& devices, NetworkManager& net) {
  cfg_ = cfg;
  dev_ = &devices;
  net_ = &net;
}

RoleCfgError RoleManager::requestApply(const RoleConfig& cfg, int* badLink) {
  RoleCfgError e = validateRoleConfig(cfg, badLink);
  if (e != RoleCfgError::Ok) return e;
  RoleConfig* p = new (std::nothrow) RoleConfig(cfg);
  if (!p) return RoleCfgError::Format;  // no memory: say so rather than drop the request silently
  RoleConfig* old;
  {
    hal::CriticalSection cs;
    old = pending_;
    pending_ = p;
  }
  delete old;
  return RoleCfgError::Ok;
}

void RoleManager::applyPending() {
  RoleConfig* p;
  {
    hal::CriticalSection cs;
    p = pending_;
    pending_ = nullptr;
  }
  if (!p) return;
  closeClient();
  req_ = Req::None;
  cfg_ = *p;
  delete p;
  haveCur_ = false;
  hasTok_ = false;
  badPw_ = false;
  attempt_ = relogins_ = 0;
  nextTry_ = 0;
  err_[0] = 0;
  if (rt_) {
    OpQueue<RemoteCmd, kRemoteQueue> fresh;
    rt_->q = fresh;
    for (auto& s : rt_->ls) s = LinkState();
  }
  bool saved = RoleStore::save(cfg_);
  logEventf("role", "settings %s, role %s", saved ? "saved" : "NOT saved", cfg_.role == Role::Slave ? "slave" : "master");
}

void RoleManager::setError(const char* e) {
  if (strcmp(err_, e) != 0 && e[0]) logEventf("role", "master link: %s", e);  // log a change, not every retry
  snprintf(err_, sizeof err_, "%s", e);
}

void RoleManager::closeClient() {
  if (!client_) return;
  client_->close(true);
  delete client_;
  client_ = nullptr;
}

void RoleManager::failRequest(const char* why, uint32_t now) {
  setError(why);
  nextTry_ = now + roleBackoffMs(attempt_++);
}

// ---- buttons -> queue --------------------------------------------------------------------------

void RoleManager::pollButtons(uint32_t now) {
  if (int32_t(now - nextPoll_) < 0) return;
  nextPoll_ = now + kPollMs;
  uint8_t missing = 0;
  for (size_t i = 0; i < cfg_.nLinks; i++) {
    const LinkRule& l = cfg_.links[i];
    bool active;
    uint32_t longs;
    if (!dev_->inputSnapshot(l.input, active, longs)) { missing++; rt_->ls[i] = LinkState(); continue; }
    uint8_t ev = linkEvents(rt_->ls[i], active, longs);
    if (!ev || !linkFires(l, ev)) continue;
    RemoteCmd c;
    snprintf(c.id, sizeof c.id, "%s", l.remote);
    c.act = l.act;
    c.value = l.value;
    c.born = now;
    if (!remoteEnqueue(rt_->q, c)) dropped_++;
  }
  missing_ = missing;
}

// ---- requests ----------------------------------------------------------------------------------

bool RoleManager::startRequest(Req r, uint32_t now) {
  if (hal::freeHeap() < kMinHeapToRequest) { failRequest("low_heap", now); return false; }
  char body[112];
  if (r == Req::Login) {
    JsonDocument d;
    d["password"] = cfg_.pass;  // serialised, so a quote or backslash in the password is escaped
    if (serializeJson(d, body, sizeof body) >= sizeof body) { failRequest("password_too_long", now); return false; }
  } else if (!remoteBody(body, sizeof body, cur_)) {
    haveCur_ = false;
    failRequest("bad_command", now);
    return false;
  }
  size_t n = httpBuildRequest(rt_->req, sizeof rt_->req, "POST", r == Req::Login ? "/api/v1/session" : "/api/v1/command",
                              cfg_.host, r == Req::Login ? nullptr : rt_->tok, body);
  if (!n) { failRequest("request_too_long", now); return false; }

  respLen_ = 0;
  rt_->resp[0] = 0;
  done_ = false;
  client_ = new (std::nothrow) AsyncClient();
  if (!client_) { failRequest("low_heap", now); return false; }
  const bool login = r == Req::Login;
  client_->onConnect([this, n](void*, AsyncClient* c) { c->write(rt_->req, n); }, nullptr);
  client_->onData([this, login](void*, AsyncClient*, void* data, size_t len) {
    if (done_) return;
    size_t have = respLen_;
    size_t room = sizeof rt_->resp - 1 - have;
    if (len > room) len = room;
    memcpy(rt_->resp + have, data, len);
    have += len;
    respLen_ = have;
    int st = httpStatus(rt_->resp, have);
    char tmp[4];
    // a command is answered by its status line alone; a login also needs the token
    if (st >= 0 && (!login || st != 200 || httpJsonString(rt_->resp, have, "token", tmp, sizeof tmp) || have >= sizeof rt_->resp - 1))
      done_ = true;
    else if (have >= sizeof rt_->resp - 1) done_ = true;
  }, nullptr);
  client_->onDisconnect([this](void*, AsyncClient*) { done_ = true; }, nullptr);
  client_->onError([this](void*, AsyncClient*, int8_t) { done_ = true; }, nullptr);
  client_->onTimeout([](void*, AsyncClient* c, uint32_t) { c->close(); }, nullptr);

  IPAddress ip;
  bool ok = ip.fromString(cfg_.host) ? client_->connect(ip, cfg_.port) : client_->connect(cfg_.host, cfg_.port);
  if (!ok) { closeClient(); failRequest("connect_failed", now); return false; }
  req_ = r;
  reqStart_ = now;
  return true;
}

void RoleManager::finish(uint32_t now) {
  size_t n = respLen_;
  rt_->resp[n] = 0;
  int st = httpStatus(rt_->resp, n);
  closeClient();
  Req r = req_;
  req_ = Req::None;
  if (r == Req::Login) handleLogin(st, now); else handleCommand(st, now);
}

void RoleManager::handleLogin(int st, uint32_t now) {
  size_t n = respLen_;
  if (st == 200 && httpJsonString(rt_->resp, n, "token", rt_->tok, sizeof rt_->tok)) {
    if (strstr(rt_->resp, "\"must_change\":true")) {  // the master still has the factory password: every route is locked
      setError("master_default_password");
      nextTry_ = now + 30000;
      return;
    }
    hasTok_ = true;
    attempt_ = 0;
    return;  // service() sends the waiting command on its next pass
  }
  if (st == 401) {
    badPw_ = true;
    setError("wrong_password");
    nextTry_ = now + kBadPasswordRetryMs;
    return;
  }
  if (st == 429) { setError("master_locked_out"); nextTry_ = now + 30000; return; }
  char e[24];
  snprintf(e, sizeof e, "login_failed (%d)", st);
  failRequest(e, now);
}

void RoleManager::handleCommand(int st, uint32_t now) {
  bool changeNeeded = strstr(rt_->resp, "password_change_required") != nullptr;
  switch (classifyCmdReply(st, changeNeeded)) {
    case CmdReply::Done:
      haveCur_ = false;
      sent_++;
      attempt_ = relogins_ = 0;
      setError("");
      lastReject_[0] = 0;
      break;
    case CmdReply::Relogin:
      hasTok_ = false;
      if (++relogins_ > 2) failRequest("session_refused", now);  // logging in works but commands are refused: back off
      break;
    case CmdReply::DefaultPassword:
      hasTok_ = false;
      setError("master_default_password");
      nextTry_ = now + 30000;
      break;
    case CmdReply::Locked:
      setError("master_locked_out");
      nextTry_ = now + 30000;
      break;
    case CmdReply::Rejected: {
      haveCur_ = false;
      rejected_++;
      if (!httpJsonString(rt_->resp, respLen_, "error", lastReject_, sizeof lastReject_)) snprintf(lastReject_, sizeof lastReject_, "http_%d", st);
      char e[40];
      snprintf(e, sizeof e, "master_rejected: %s", lastReject_);
      setError(e);
      break;
    }
    default: {
      char e[24];
      if (st < 0) snprintf(e, sizeof e, "no_answer"); else snprintf(e, sizeof e, "master_busy (%d)", st);
      failRequest(e, now);
      break;
    }
  }
}

void RoleManager::service(uint32_t now) {
  if (req_ != Req::None) {
    if (done_) finish(now);
    else if (now - reqStart_ > kRequestTimeoutMs) { closeClient(); req_ = Req::None; failRequest("timeout", now); }
    return;
  }
  if (haveCur_ && remoteExpired(cur_, now)) { haveCur_ = false; expired_++; }  // never send a late command
  if (int32_t(now - nextTry_) < 0) return;
  while (!haveCur_) {
    if (!rt_->q.pop(cur_)) return;
    if (remoteExpired(cur_, now)) { expired_++; continue; }
    haveCur_ = true;
  }
  startRequest(hasTok_ ? Req::Command : Req::Login, now);
}

void RoleManager::loop() {
  if (!dev_) return;
  uint32_t now = millis();
  if (pending_) applyPending();
  if (cfg_.role != Role::Slave || !net_->staConnected()) {
    if (client_) { closeClient(); req_ = Req::None; }
    return;
  }
  if (!rt_) rt_ = new (std::nothrow) Rt();
  if (!rt_) return;
  pollButtons(now);
  service(now);
}

void RoleManager::status(JsonObject out) const {
  out["role"] = cfg_.role == Role::Slave ? "slave" : "master";
  if (cfg_.role != Role::Slave) return;
  out["state"] = req_ == Req::Login ? "logging_in" : req_ == Req::Command ? "sending" : hasTok_ ? "ready" : "idle";
  if (err_[0]) out["error"] = err_;
  out["sent"] = sent_;
  out["rejected"] = rejected_;
  out["expired"] = expired_;
  out["dropped"] = dropped_;
  out["missing_inputs"] = missing_;
  if (lastReject_[0]) out["last_reject"] = lastReject_;
}

}  // namespace espwf

#endif  // ESPWF_HAS_ROLE
