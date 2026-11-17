#pragma once
// Automation rules: the data model and the validation. Pure C++ (no Arduino, no JSON) so it is tested on
// the host. AutomationStore.cpp turns JSON into these structs; AutomationCore.h runs them.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dashboard/DashboardRules.h"  // Prop, deviceProps, DeviceLookup: what a device offers
#include "devices/DeviceTypes.h"

namespace espwf {

// Memory is the limit, not the spec: a rule is about 340 bytes and the NodeMCU has ~23 KB of heap to spare.
constexpr size_t kMaxRules = 16;
constexpr size_t kMaxConds = 3;
constexpr size_t kMaxActions = 4;
constexpr size_t kMaxInterlocks = 4;
constexpr unsigned kRuleNameCap = 25;  // 24 chars + NUL
constexpr unsigned kActTextCap = 16;   // 15 chars + NUL: an event name or a short serial line

enum class TrigType : uint8_t { Change, When, Interval, Schedule, Startup, Wifi, Manual, kCount };
inline const char* trigName(TrigType t) {
  static const char* const n[] = {"change", "when", "interval", "schedule", "startup", "wifi", "manual"};
  return uint8_t(t) < uint8_t(TrigType::kCount) ? n[uint8_t(t)] : "";
}
inline bool parseTrig(const char* s, TrigType& out) {
  if (!s) return false;
  for (uint8_t i = 0; i < uint8_t(TrigType::kCount); i++)
    if (!strcmp(s, trigName(TrigType(i)))) { out = TrigType(i); return true; }
  return false;
}

enum class Op : uint8_t { Eq, Ne, Gt, Lt, Ge, Le, kCount };
inline const char* opName(Op o) {
  static const char* const n[] = {"==", "!=", ">", "<", ">=", "<="};
  return uint8_t(o) < uint8_t(Op::kCount) ? n[uint8_t(o)] : "";
}
inline bool parseOp(const char* s, Op& out) {
  if (!s) return false;
  for (uint8_t i = 0; i < uint8_t(Op::kCount); i++)
    if (!strcmp(s, opName(Op(i)))) { out = Op(i); return true; }
  return false;
}
inline bool compare(Op o, double a, double b) {
  switch (o) {
    case Op::Eq: return fabs(a - b) < 1e-4;
    case Op::Ne: return fabs(a - b) >= 1e-4;
    case Op::Gt: return a > b;
    case Op::Lt: return a < b;
    case Op::Ge: return a >= b - 1e-4;
    case Op::Le: return a <= b + 1e-4;
    default: return false;
  }
}

enum class ActType : uint8_t { On, Off, Toggle, Set, Stop, Send, Event, kCount };
inline const char* actName(ActType t) {
  static const char* const n[] = {"on", "off", "toggle", "set", "stop", "send", "event"};
  return uint8_t(t) < uint8_t(ActType::kCount) ? n[uint8_t(t)] : "";
}
inline bool parseAct(const char* s, ActType& out) {
  if (!s) return false;
  for (uint8_t i = 0; i < uint8_t(ActType::kCount); i++)
    if (!strcmp(s, actName(ActType(i)))) { out = ActType(i); return true; }
  return false;
}

enum class CondKind : uint8_t { Compare, Time };

struct Trigger {
  TrigType type = TrigType::Manual;
  char device[kIdCap] = {0};  // change
  Prop prop = Prop::None;     // change
  uint32_t everyS = 0;        // interval
  uint16_t minute = 0;        // schedule: minute of the day
  uint8_t days = 0x7F;        // schedule: bit 0 = Sunday, bit 6 = Saturday
  bool wifiUp = true;         // wifi: true = when it connects, false = when it drops
};

struct Cond {
  CondKind kind = CondKind::Compare;
  char device[kIdCap] = {0};
  Prop prop = Prop::None;
  Op op = Op::Eq;
  float value = 0;
  uint32_t forS = 0;          // compare: must hold this long (only for "when" rules)
  uint16_t from = 0, to = 0;  // time: window in minutes of the day; from > to wraps midnight
  uint8_t days = 0x7F;        // time: which days (bit 0 = Sunday)
};

struct Action {
  ActType type = ActType::Event;
  char device[kIdCap] = {0};
  float value = 0;
  char text[kActTextCap] = {0};
};

struct Rule {
  char id[kIdCap] = {0};
  char name[kRuleNameCap] = {0};
  bool enabled = true;
  Trigger trig;
  bool matchAny = false;  // false = every condition must hold (AND), true = one is enough (OR)
  uint8_t nConds = 0;
  Cond conds[kMaxConds];
  uint8_t nActs = 0;
  Action acts[kMaxActions];
  uint32_t cooldownS = 0;
};

struct Interlock {  // two relays that must never be on together
  char a[kIdCap] = {0};
  char b[kIdCap] = {0};
};

struct AutoSettings {
  bool enabled = true;
  int16_t utcOffsetMin = 0;
  uint8_t nLocks = 0;
  Interlock locks[kMaxInterlocks + 1];  // one spare so a count over the limit never reads past the array
};

enum class AutoErr : uint8_t {
  Ok = 0, Format, Id, Duplicate, Name, Trigger, Device, Prop, Op, Value, Time, Days, Conds, Actions, Action,
  Cooldown, Duration, TooMany, Interlock, Offset, NotFound, Busy
};

inline const char* autoErrCode(AutoErr e) {
  switch (e) {
    case AutoErr::Ok: return "";
    case AutoErr::Format: return "bad_format";
    case AutoErr::Id: return "bad_id";
    case AutoErr::Duplicate: return "duplicate_id";
    case AutoErr::Name: return "bad_name";
    case AutoErr::Trigger: return "bad_trigger";
    case AutoErr::Device: return "unknown_device";
    case AutoErr::Prop: return "bad_property";
    case AutoErr::Op: return "bad_operator";
    case AutoErr::Value: return "bad_value";
    case AutoErr::Time: return "bad_time";
    case AutoErr::Days: return "bad_days";
    case AutoErr::Conds: return "bad_conditions";
    case AutoErr::Actions: return "bad_actions";
    case AutoErr::Action: return "bad_action";
    case AutoErr::Cooldown: return "bad_cooldown";
    case AutoErr::Duration: return "bad_duration";
    case AutoErr::TooMany: return "too_many";
    case AutoErr::Interlock: return "bad_interlock";
    case AutoErr::Offset: return "bad_offset";
    case AutoErr::NotFound: return "not_found";
    case AutoErr::Busy: return "busy";
  }
  return "";
}
inline const char* autoErrField(AutoErr e) {
  switch (e) {
    case AutoErr::Id: case AutoErr::Duplicate: return "id";
    case AutoErr::Name: return "name";
    case AutoErr::Trigger: return "trigger";
    case AutoErr::Device: return "device";
    case AutoErr::Prop: return "property";
    case AutoErr::Op: return "operator";
    case AutoErr::Value: return "value";
    case AutoErr::Time: return "time";
    case AutoErr::Days: return "days";
    case AutoErr::Conds: return "conditions";
    case AutoErr::Actions: return "actions";
    case AutoErr::Action: return "action";
    case AutoErr::Cooldown: return "cooldown_s";
    case AutoErr::Duration: return "for_s";
    case AutoErr::Interlock: return "interlocks";
    case AutoErr::Offset: return "utc_offset_min";
    default: return "";
  }
}

// ---- helpers -------------------------------------------------------------------------------------

// Bounded copy that always terminates (strlcpy is not in every libc).
inline void copyStr(char* dst, const char* src, size_t cap) {
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

// A property a condition or a change trigger can read as a number (text cannot).
inline bool numericProp(Prop p) { return p != Prop::None && p != Prop::Text && p != Prop::kCount; }

inline bool textOk(const char* s, unsigned cap, bool allowEmpty) {
  if (!memchr(s, 0, cap)) return false;
  if (!allowEmpty && !s[0]) return false;
  for (const char* p = s; *p; p++)
    if (uint8_t(*p) < 0x20 || uint8_t(*p) > 0x7E) return false;  // printable ASCII only
  return true;
}

// Turns an action into the command the registry understands. False if the action does not fit the device type.
inline bool buildCommand(const Action& a, DeviceType t, Command& out) {
  out = Command();
  switch (a.type) {
    case ActType::On:
    case ActType::Off:
      if (t != DeviceType::Relay) return false;
      out.action = CmdAction::Set; out.hasValue = out.isBool = true; out.value = a.type == ActType::On ? 1 : 0;
      break;
    case ActType::Toggle:
      if (t != DeviceType::Relay) return false;
      out.action = CmdAction::Toggle;
      break;
    case ActType::Set:
      if (t != DeviceType::Pwm && t != DeviceType::Motor && t != DeviceType::Servo) return false;
      out.action = CmdAction::Set; out.hasValue = true; out.value = a.value;
      break;
    case ActType::Stop:
      if (t != DeviceType::Motor) return false;
      out.action = CmdAction::Stop;
      break;
    case ActType::Send:
      if (t != DeviceType::Serial) return false;
      out.action = CmdAction::Send; out.isText = true;
      copyStr(out.text, a.text, sizeof out.text);
      break;
    default: return false;  // event has no device
  }
  return checkCommand(t, out) == CmdErr::Ok;
}

// ---- validation ----------------------------------------------------------------------------------

inline AutoErr validateCond(const Cond& c, DeviceLookup find, void* ctx, bool whenRule) {
  if (c.kind == CondKind::Time) {
    if (c.from > 1439 || c.to > 1439 || c.from == c.to) return AutoErr::Time;
    if (!(c.days & 0x7F) || (c.days & 0x80)) return AutoErr::Days;
    return AutoErr::Ok;
  }
  DeviceConfig d;
  if (!textOk(c.device, kIdCap, false) || !find(ctx, c.device, d)) return AutoErr::Device;
  bool w;
  if (!numericProp(c.prop) || !devicePropAccess(d, c.prop, w)) return AutoErr::Prop;
  if (uint8_t(c.op) >= uint8_t(Op::kCount)) return AutoErr::Op;
  if (!isfinite(c.value) || c.value < -1e6f || c.value > 1e6f) return AutoErr::Value;
  if (c.forS > 86400) return AutoErr::Duration;
  if (c.forS > 0 && !whenRule) return AutoErr::Duration;  // a duration needs the engine to watch continuously
  return AutoErr::Ok;
}

inline AutoErr validateAction(const Action& a, DeviceLookup find, void* ctx) {
  if (uint8_t(a.type) >= uint8_t(ActType::kCount)) return AutoErr::Action;
  if (a.type == ActType::Event) return textOk(a.text, kActTextCap, false) ? AutoErr::Ok : AutoErr::Value;
  DeviceConfig d;
  if (!textOk(a.device, kIdCap, false) || !find(ctx, a.device, d)) return AutoErr::Device;
  if (a.type == ActType::Send && !textOk(a.text, kActTextCap, false)) return AutoErr::Value;
  if (a.type == ActType::Set && !isfinite(a.value)) return AutoErr::Value;
  Command c;
  return buildCommand(a, d.type, c) ? AutoErr::Ok : AutoErr::Action;
}

inline AutoErr validateTrigger(const Trigger& t, size_t nConds, DeviceLookup find, void* ctx) {
  switch (t.type) {
    case TrigType::Change: {
      DeviceConfig d;
      if (!textOk(t.device, kIdCap, false) || !find(ctx, t.device, d)) return AutoErr::Device;
      bool w;
      if (!numericProp(t.prop) || !devicePropAccess(d, t.prop, w)) return AutoErr::Prop;
      return AutoErr::Ok;
    }
    case TrigType::When: return nConds >= 1 ? AutoErr::Ok : AutoErr::Conds;  // nothing to watch without a condition
    case TrigType::Interval: return (t.everyS >= 1 && t.everyS <= 86400) ? AutoErr::Ok : AutoErr::Time;
    case TrigType::Schedule:
      if (t.minute > 1439) return AutoErr::Time;
      return ((t.days & 0x7F) && !(t.days & 0x80)) ? AutoErr::Ok : AutoErr::Days;
    case TrigType::Startup:
    case TrigType::Wifi:
    case TrigType::Manual: return AutoErr::Ok;
    default: return AutoErr::Trigger;
  }
}

// Everything about one rule except "is its id already taken" (the engine checks that).
inline AutoErr validateRule(const Rule& r, DeviceLookup find, void* ctx) {
  if (!textOk(r.id, kIdCap, false) || !detail::validDeviceId(r.id)) return AutoErr::Id;
  if (!textOk(r.name, kRuleNameCap, true)) return AutoErr::Name;
  if (uint8_t(r.trig.type) >= uint8_t(TrigType::kCount)) return AutoErr::Trigger;
  if (r.nConds > kMaxConds) return AutoErr::Conds;
  if (r.nActs < 1 || r.nActs > kMaxActions) return AutoErr::Actions;
  if (r.cooldownS > 86400) return AutoErr::Cooldown;
  AutoErr e = validateTrigger(r.trig, r.nConds, find, ctx);
  if (e != AutoErr::Ok) return e;
  for (size_t i = 0; i < r.nConds; i++) {
    e = validateCond(r.conds[i], find, ctx, r.trig.type == TrigType::When);
    if (e != AutoErr::Ok) return e;
  }
  for (size_t i = 0; i < r.nActs; i++) {
    e = validateAction(r.acts[i], find, ctx);
    if (e != AutoErr::Ok) return e;
  }
  return AutoErr::Ok;
}

inline AutoErr validateSettings(const AutoSettings& s, DeviceLookup find, void* ctx) {
  if (s.utcOffsetMin < -720 || s.utcOffsetMin > 840) return AutoErr::Offset;
  if (s.nLocks > kMaxInterlocks) return AutoErr::Interlock;
  for (size_t i = 0; i < s.nLocks; i++) {
    DeviceConfig a, b;
    if (!textOk(s.locks[i].a, kIdCap, false) || !textOk(s.locks[i].b, kIdCap, false)) return AutoErr::Interlock;
    if (!strcmp(s.locks[i].a, s.locks[i].b)) return AutoErr::Interlock;
    if (!find(ctx, s.locks[i].a, a) || !find(ctx, s.locks[i].b, b)) return AutoErr::Interlock;
    if (a.type != DeviceType::Relay || b.type != DeviceType::Relay) return AutoErr::Interlock;
  }
  return AutoErr::Ok;
}

}  // namespace espwf
