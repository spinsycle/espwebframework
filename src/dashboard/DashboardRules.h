#pragma once
// Dashboard rules: which component can bind to which device property, and the layout limits.
// Pure C++ (no Arduino, no JSON) so it is tested on the host. The firmware parses JSON into a DashComp
// (DashboardStore.cpp) and asks these functions whether it is acceptable; /api/v1/dashboard/schema
// publishes the same tables so the editor never offers a binding the firmware would refuse.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "devices/DeviceTypes.h"

namespace espwf {

constexpr size_t kMaxComponents = 32;
constexpr size_t kMaxTabs = 6;
constexpr size_t kMaxGroupDevices = 8;
constexpr size_t kMaxOptions = 8;
constexpr unsigned kLabelCap = 25;  // 24 chars + NUL
constexpr unsigned kTabCap = 17;    // 16 chars + NUL
constexpr unsigned kOptCap = 17;
constexpr int kGridCols = 4;
constexpr int kMaxRow = 64;
constexpr int kMaxSpan = 4;

// ---- what a device offers ------------------------------------------------------------------------

enum class Prop : uint8_t { None = 0, State, Level, Temp, Humidity, Pressure, Text, Count, Ok, kCount };

inline const char* propName(Prop p) {
  switch (p) {
    case Prop::State: return "state";
    case Prop::Level: return "level";
    case Prop::Temp: return "temp_c";
    case Prop::Humidity: return "humidity";
    case Prop::Pressure: return "pressure_hpa";
    case Prop::Text: return "text";
    case Prop::Count: return "count";
    case Prop::Ok: return "ok";
    default: return "";
  }
}
inline bool parseProp(const char* s, Prop& out) {
  if (!s) return false;
  for (uint8_t i = 1; i < uint8_t(Prop::kCount); i++)
    if (!strcmp(s, propName(Prop(i)))) { out = Prop(i); return true; }
  return false;
}

struct PropAccess {
  Prop prop;
  bool write;  // the dashboard may command it, not only read it
};

// Fills `out` (room for 5) with what the device offers.
inline size_t deviceProps(const DeviceConfig& d, PropAccess* out) {
  size_t n = 0;
  switch (d.type) {
    case DeviceType::Relay: out[n++] = {Prop::State, true}; break;
    case DeviceType::Input: out[n++] = {Prop::State, false}; out[n++] = {Prop::Count, false}; break;
    case DeviceType::Pwm:
    case DeviceType::Motor:
    case DeviceType::Servo: out[n++] = {Prop::Level, true}; break;
    case DeviceType::Sensor:
      out[n++] = {Prop::Temp, false};
      if (d.model == 1) { out[n++] = {Prop::Pressure, false}; out[n++] = {Prop::Humidity, false}; }
      out[n++] = {Prop::Ok, false};
      break;
    case DeviceType::Serial: out[n++] = {Prop::Text, false}; out[n++] = {Prop::Count, false}; break;
  }
  return n;
}

inline bool devicePropAccess(const DeviceConfig& d, Prop p, bool& write) {
  PropAccess a[5];
  size_t n = deviceProps(d, a);
  for (size_t i = 0; i < n; i++)
    if (a[i].prop == p) { write = a[i].write; return true; }
  return false;
}

// Range of a commandable level: pwm 0..100, motor -100..100, servo 0..180.
inline bool levelRange(DeviceType t, int& lo, int& hi) {
  switch (t) {
    case DeviceType::Pwm: lo = 0; hi = 100; return true;
    case DeviceType::Motor: lo = -100; hi = 100; return true;
    case DeviceType::Servo: lo = 0; hi = 180; return true;
    default: return false;
  }
}

// ---- component types -----------------------------------------------------------------------------

enum class CompType : uint8_t {
  Toggle, PushButton, Slider, Joystick, Gauge, Numeric, Status, Graph, Text, Dropdown, Progress, DeviceCard, Group, Timer, kCount
};

inline const char* compTypeName(CompType t) {
  static const char* const n[] = {"toggle", "push_button", "slider", "joystick", "gauge", "numeric", "status",
                                  "graph", "text", "dropdown", "progress", "device_card", "group_panel", "timer"};
  return uint8_t(t) < uint8_t(CompType::kCount) ? n[uint8_t(t)] : "";
}
inline bool parseCompType(const char* s, CompType& out) {
  if (!s) return false;
  for (uint8_t i = 0; i < uint8_t(CompType::kCount); i++)
    if (!strcmp(s, compTypeName(CompType(i)))) { out = CompType(i); return true; }
  return false;
}

// What a component needs from its binding.
enum class Need : uint8_t {
  Card,        // a device, no property (device_card)
  Devices,     // a list of devices (group_panel)
  Action,      // a device and a command to send (push_button)
  WriteState,  // a commandable on/off property (toggle)
  WriteLevel,  // a commandable number (slider, dropdown, joystick: two of them)
  ReadNumber,  // a readable number (gauge, numeric, progress, graph)
  ReadState,   // a readable on/off or ok flag (status)
  ReadText,    // a readable string (text)
  Timer        // an automation timer, named in binding.device (timer)
};

inline Need compNeed(CompType t) {
  switch (t) {
    case CompType::Toggle: return Need::WriteState;
    case CompType::PushButton: return Need::Action;
    case CompType::Slider:
    case CompType::Joystick:
    case CompType::Dropdown: return Need::WriteLevel;
    case CompType::Gauge:
    case CompType::Numeric:
    case CompType::Progress:
    case CompType::Graph: return Need::ReadNumber;
    case CompType::Status: return Need::ReadState;
    case CompType::Text: return Need::ReadText;
    case CompType::DeviceCard: return Need::Card;
    case CompType::Group: return Need::Devices;
    case CompType::Timer: return Need::Timer;
    default: return Need::Card;
  }
}

// May a component with this need show/use this property? `write` = the device lets the dashboard command it.
inline bool needAccepts(Need n, Prop p, bool write) {
  switch (n) {
    case Need::WriteState: return p == Prop::State && write;
    case Need::WriteLevel: return p == Prop::Level && write;
    case Need::ReadNumber:
      return p == Prop::Level || p == Prop::Temp || p == Prop::Humidity || p == Prop::Pressure || p == Prop::Count;
    case Need::ReadState: return p == Prop::State || p == Prop::Ok;
    case Need::ReadText: return p == Prop::Text;
    default: return false;
  }
}

// Smallest size a component is usable at.
inline void compMinSize(CompType t, uint8_t& w, uint8_t& h) {
  w = 1; h = 1;
  if (t == CompType::Joystick || t == CompType::Group) { w = 2; h = 2; }
  if (t == CompType::Graph || t == CompType::Timer) w = 2;
}

// ---- one component, validated --------------------------------------------------------------------

struct DashOpt {
  char label[kOptCap] = {0};
  int16_t value = 0;
};

struct DashComp {
  char id[kIdCap] = {0};
  CompType type = CompType::kCount;  // kCount = "not set": validation rejects it
  char label[kLabelCap] = {0};
  char device[kIdCap] = {0};
  char device2[kIdCap] = {0};  // joystick: the Y axis
  Prop prop = Prop::None;
  Command cmd;                 // push_button: what to send
  uint8_t col = 0, row = 0, w = 1, h = 1;
  uint8_t tab = 0;
  uint8_t nOpts = 0;
  DashOpt opts[kMaxOptions + 1];  // one spare so a count over the limit never reads past the array
  uint8_t nDevs = 0;
  char devs[kMaxGroupDevices + 1][kIdCap] = {{0}};  // one spare, as for opts
};

enum class DashErr : uint8_t {
  Ok = 0, Format, Id, Duplicate, Type, Label, Layout, Overlap, Tab, TooMany, Device, Prop, Action, Options, Mode, Tabs
};

inline const char* dashErrCode(DashErr e) {
  switch (e) {
    case DashErr::Ok: return "";
    case DashErr::Format: return "bad_format";
    case DashErr::Id: return "bad_id";
    case DashErr::Duplicate: return "duplicate_id";
    case DashErr::Type: return "bad_type";
    case DashErr::Label: return "bad_label";
    case DashErr::Layout: return "bad_layout";
    case DashErr::Overlap: return "overlap";
    case DashErr::Tab: return "bad_tab";
    case DashErr::TooMany: return "too_many";
    case DashErr::Device: return "unknown_device";
    case DashErr::Prop: return "bad_property";
    case DashErr::Action: return "bad_action";
    case DashErr::Options: return "bad_options";
    case DashErr::Mode: return "bad_mode";
    case DashErr::Tabs: return "bad_tabs";
  }
  return "";
}
inline const char* dashErrField(DashErr e) {
  switch (e) {
    case DashErr::Id: case DashErr::Duplicate: return "id";
    case DashErr::Type: return "type";
    case DashErr::Label: return "label";
    case DashErr::Layout: case DashErr::Overlap: return "layout";
    case DashErr::Tab: return "tab";
    case DashErr::Device: return "binding.device";
    case DashErr::Prop: return "binding.property";
    case DashErr::Action: return "binding.action";
    case DashErr::Options: return "options";
    case DashErr::Mode: return "mode";
    case DashErr::Tabs: return "tabs";
    default: return "";
  }
}

// Finds a registered device by id. The firmware passes the live registry; tests pass a small array.
using DeviceLookup = bool (*)(void* ctx, const char* id, DeviceConfig& out);

namespace dashdetail {
inline bool text(const char* s, unsigned cap, bool allowEmpty) {
  if (!memchr(s, 0, cap)) return false;
  return allowEmpty || s[0] != 0;
}
}  // namespace dashdetail

// Everything except the overlap and duplicate checks (those need the whole list: see DashSet).
// `nTabs` is how many tabs the dashboard has; with none, tab 0 is the single implicit page.
// `timerFind` says whether an automation timer exists; without it a timer component is refused.
inline DashErr validateComponent(const DashComp& c, DeviceLookup find, void* ctx, size_t nTabs,
                                 bool (*timerFind)(void*, const char*) = nullptr, void* tctx = nullptr) {
  using namespace dashdetail;
  if (!text(c.id, kIdCap, false) || !detail::validDeviceId(c.id)) return DashErr::Id;
  if (uint8_t(c.type) >= uint8_t(CompType::kCount)) return DashErr::Type;
  if (!text(c.label, kLabelCap, true)) return DashErr::Label;
  if (c.col < 1 || c.row < 1 || c.w < 1 || c.h < 1 || c.w > kMaxSpan || c.h > kMaxSpan) return DashErr::Layout;
  if (c.col + c.w - 1 > kGridCols || c.row > kMaxRow) return DashErr::Layout;
  uint8_t mw, mh;
  compMinSize(c.type, mw, mh);
  if (c.w < mw || c.h < mh) return DashErr::Layout;
  if (c.tab >= (nTabs ? nTabs : 1)) return DashErr::Tab;

  Need need = compNeed(c.type);
  DeviceConfig d;
  if (need == Need::Devices) {
    if (c.nDevs < 1 || c.nDevs > kMaxGroupDevices) return DashErr::Device;
    for (size_t i = 0; i < c.nDevs; i++) {
      if (!text(c.devs[i], kIdCap, false) || !find(ctx, c.devs[i], d)) return DashErr::Device;
      for (size_t j = 0; j < i; j++)
        if (!strcmp(c.devs[i], c.devs[j])) return DashErr::Device;  // the same device twice
    }
    return DashErr::Ok;
  }
  if (need == Need::Timer) return (text(c.device, kIdCap, false) && timerFind && timerFind(tctx, c.device)) ? DashErr::Ok : DashErr::Device;
  if (!text(c.device, kIdCap, false) || !find(ctx, c.device, d)) return DashErr::Device;
  if (need == Need::Card) return DashErr::Ok;

  if (need == Need::Action) {
    return checkCommand(d.type, c.cmd) == CmdErr::Ok ? DashErr::Ok : DashErr::Action;
  }

  bool write = false;
  if (!devicePropAccess(d, c.prop, write) || !needAccepts(need, c.prop, write)) return DashErr::Prop;

  if (c.type == CompType::Joystick) {  // two different devices, one per axis
    DeviceConfig d2;
    if (!text(c.device2, kIdCap, false) || !find(ctx, c.device2, d2) || !strcmp(c.device, c.device2)) return DashErr::Device;
    bool w2 = false;
    if (!devicePropAccess(d2, Prop::Level, w2) || !w2) return DashErr::Prop;
  }
  if (c.type == CompType::Dropdown) {
    if (c.nOpts < 1 || c.nOpts > kMaxOptions) return DashErr::Options;
    int lo, hi;
    if (!levelRange(d.type, lo, hi)) return DashErr::Options;
    for (size_t i = 0; i < c.nOpts; i++) {
      if (!text(c.opts[i].label, kOptCap, false)) return DashErr::Options;
      if (c.opts[i].value < lo || c.opts[i].value > hi) return DashErr::Options;
    }
  }
  return DashErr::Ok;
}

// Tab names: 0-6 of them, each 1-16 characters, no two alike.
inline DashErr validateTabNames(const char (*names)[kTabCap], size_t n) {
  if (n > kMaxTabs) return DashErr::Tabs;
  for (size_t i = 0; i < n; i++) {
    if (!dashdetail::text(names[i], kTabCap, false)) return DashErr::Tabs;
    for (size_t j = 0; j < i; j++)
      if (!strcmp(names[i], names[j])) return DashErr::Tabs;
  }
  return DashErr::Ok;
}

// The whole component list: duplicate ids, overlapping rectangles on the same tab, and the size limit.
class DashSet {
 public:
  DashErr add(const DashComp& c) {
    if (n_ >= kMaxComponents) return DashErr::TooMany;
    for (size_t i = 0; i < n_; i++) {
      if (!strcmp(ids_[i], c.id)) return DashErr::Duplicate;
      const Rect& r = r_[i];
      bool apart = r.tab != c.tab || c.col + c.w <= r.col || r.col + r.w <= c.col || c.row + c.h <= r.row || r.row + r.h <= c.row;
      if (!apart) return DashErr::Overlap;
    }
    strcpy(ids_[n_], c.id);
    r_[n_] = {c.tab, c.col, c.row, c.w, c.h};
    n_++;
    return DashErr::Ok;
  }
  size_t size() const { return n_; }

 private:
  struct Rect { uint8_t tab, col, row, w, h; };
  char ids_[kMaxComponents][kIdCap];
  Rect r_[kMaxComponents];
  size_t n_ = 0;
};

}  // namespace espwf
