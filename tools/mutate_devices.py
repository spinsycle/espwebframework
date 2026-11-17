"""Mutation check for the pure-C++ logic: break one thing at a time, the native tests must fail.
Run: python tools/mutate_devices.py          (needs g++ on PATH). Exit 1 if any mutant survives or a baseline fails.
Each set = (source file, test folder, mutants). Mutants are (name, old text that occurs exactly once, new text).
"""
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEVICES = [
    ("esp8266 flash pin range", "(gpio >= 6 && gpio <= 11) || gpio == 1 || gpio == 3) return PinErr::Reserved;\n    if (role == PinRole::Pwm", "(gpio >= 7 && gpio <= 11) || gpio == 1 || gpio == 3) return PinErr::Reserved;\n    if (role == PinRole::Pwm"),
    ("esp8266 gpio16 pwm", "gpio == 16) return PinErr::NoPwm", "gpio == 15) return PinErr::NoPwm"),
    ("esp8266 max gpio", "if (gpio > 16) return PinErr::Invalid;", "if (gpio > 17) return PinErr::Invalid;"),
    ("esp32 input-only", "gpio >= 34 && role != PinRole::Input", "gpio >= 35 && role != PinRole::Input"),
    ("esp32 invalid 20", "gpio == 20 || gpio == 24", "gpio == 21 || gpio == 24"),
    ("pullup gpio16", "gpio != 16 : gpio < 34", "gpio != 15 : gpio < 34"),
    ("boot sensitive 15", "gpio == 0 || gpio == 2 || gpio == 15;\n  return gpio == 0 || gpio == 2 || gpio == 5", "gpio == 0 || gpio == 2;\n  return gpio == 0 || gpio == 2 || gpio == 5"),
    ("id charset digits", "(c >= '0' && c <= '9') || c == '_'", "(c >= '1' && c <= '9') || c == '_'"),
    ("id max length", "n > kIdCap - 1", "n > kIdCap"),
    ("full check", "if (n - (skip >= 0 ? 1 : 0) >= kMaxDevices) return DevErr::Full;", "if (n - (skip >= 0 ? 1 : 0) > kMaxDevices) return DevErr::Full;"),
    ("duplicate id", "if (!strcmp(others[i].id, c.id)) return DevErr::Duplicate;", ""),
    ("pwm limit", "pwmCount >= kMaxPwm", "pwmCount > kMaxPwm"),
    ("pin in use", "if (mine[a] == theirs[b]) return DevErr::PinInUse;", ""),
    ("pullup check", "c.pullup && !pinHasPullup(chip, c.pin)", "c.pullup && pinHasPullup(chip, c.pin)"),
    ("debounce limit", "c.debounceMs > 1000", "c.debounceMs > 1001"),
    ("freq upper", "(c.freqHz < 1 || c.freqHz > 20000)) return DevErr::Freq;\n  if (c.type == DeviceType::Relay", "(c.freqHz < 1 || c.freqHz > 20001)) return DevErr::Freq;\n  if (c.type == DeviceType::Relay"),
    ("freq lower", "(c.freqHz < 1 || c.freqHz > 20000)) return DevErr::Freq;\n  if (c.type == DeviceType::Relay", "(c.freqHz < 0 || c.freqHz > 20000)) return DevErr::Freq;\n  if (c.type == DeviceType::Relay"),
    ("relay initial", "c.type == DeviceType::Relay && c.initial > 1", "c.type == DeviceType::Relay && c.initial > 2"),
    ("pwm initial", "c.initial > 100) return DevErr::Initial", "c.initial > 101) return DevErr::Initial"),
    ("input not commandable", "if (t == DeviceType::Input || t == DeviceType::Sensor) return CmdErr::NotCommandable;", ""),
    ("relay set needs bool", "return (c.hasValue && c.isBool) ? CmdErr::Ok", "return (c.hasValue) ? CmdErr::Ok"),
    ("pwm rejects bool", "return CmdErr::BadAction;\n  if (!c.hasValue || c.isBool) return CmdErr::BadValue;\n  return (c.value >= 0", "return CmdErr::BadAction;\n  if (!c.hasValue) return CmdErr::BadValue;\n  return (c.value >= 0"),
    ("pwm upper bound", "c.value <= 100)", "c.value <= 101)"),
    ("pwm lower bound", "c.value >= 0 &&", "c.value >= -1 &&"),
    ("pwm toggle", "  // Pwm\n  if (c.action != CmdAction::Set) return CmdErr::BadAction;", "  // Pwm"),
    ("relay level", "return on != invert; }\ninline bool inputActive", "return on == invert; }\ninline bool inputActive"),
    ("input level", "return pinLevel != invert; }", "return pinLevel == invert; }"),
    ("pwm invert", "(invert ? 100 - p : p) * 10", "(invert ? p : p) * 10"),
    ("pwm clamp", "percent > 100 ? 100 : percent", "percent"),
    ("debounce threshold", ">= ms) { stable", "> ms) { stable"),
    ("debounce restart", "if (level != raw) { raw = level; since = now; }", "if (level != raw) { raw = level; }"),
    ("debounce prime", "if (!primed) { primed = true; raw = stable = level; since = now; return false; }", "if (!primed) { primed = true; raw = level; since = now; return false; }"),
    ("http 404", "case CmdErr::NotFound: return 404;", "case CmdErr::NotFound: return 400;"),
    ("http 409", "case CmdErr::NotCommandable: return 409;", "case CmdErr::NotCommandable: return 400;"),
    ("http 503", "case CmdErr::QueueFull: return 503;", "case CmdErr::QueueFull: return 400;"),
    ("http ok 202", "case CmdErr::Ok: return 202;", "case CmdErr::Ok: return 200;"),
    ("err code busy", 'case CmdErr::QueueFull: return "busy";', 'case CmdErr::QueueFull: return "full";'),
    ("err code not_found", 'case CmdErr::NotFound: return "not_found";', 'case CmdErr::NotFound: return "missing";'),
    ("err field action", 'case CmdErr::BadAction: return "action";', 'case CmdErr::BadAction: return "id";'),
    ("err field value", 'case CmdErr::BadValue: return "value";', 'case CmdErr::BadValue: return "id";'),
]

P4 = [
    ("i2c sda", "chip == Chip::Esp8266 ? 4 : 21", "chip == Chip::Esp8266 ? 5 : 21"),
    ("i2c scl", "chip == Chip::Esp8266 ? 5 : 22", "chip == Chip::Esp8266 ? 4 : 22"),
    ("i2c device model", "c.type == DeviceType::Sensor) && c.model == 1;", "c.type == DeviceType::Sensor) && c.model == 0;"),
    ("pwm slot motor", "c.type == DeviceType::Pwm || c.type == DeviceType::Motor ||", "c.type == DeviceType::Pwm ||"),
    ("pwm slot servo", "(c.type == DeviceType::Servo && c.model == 0);", "(c.type == DeviceType::Servo && c.model == 1);"),
    ("servo 50 hz", "c.type == DeviceType::Servo ? 50 : c.freqHz", "c.type == DeviceType::Servo ? 60 : c.freqHz"),
    ("pins of motor in2", "if (c.pin3 >= 0) out[n++] = c.pin3;", ""),
    ("bus pins skip", "if (isI2cDevice(devs[i])) continue;  // those share", "  // those share"),
    ("bus pins or", "p[j] == i2cSda(chip) || p[j] == i2cScl(chip)", "p[j] == i2cSda(chip) && p[j] == i2cScl(chip)"),
    ("baud esp8266 cap", "b <= 38400", "b <= 57600"),
    ("baud list", "{1200, 2400, 4800, 9600,", "{1200, 2400, 4800, 9601,"),
    ("serial limit", "serialCount >= 1", "serialCount >= 2"),
    ("freq conflict chip", "if (chip == Chip::Esp8266 && usesPwmSlot(c))", "if (chip == Chip::Esp32 && usesPwmSlot(c))"),
    ("freq conflict cmp", "pwmFreq(others[i]) != pwmFreq(c)", "pwmFreq(others[i]) == pwmFreq(c)"),
    ("addr in use", "c.i2cAddr == others[i].i2cAddr &&", "c.i2cAddr != others[i].i2cAddr &&"),
    ("same board channel", "c.channel != others[i].channel", "c.channel == others[i].channel"),
    ("sensor role", "uses[nu++] = {c.pin, PinRole::Output}; break;  // 1-Wire", "uses[nu++] = {c.pin, PinRole::Input}; break;  // 1-Wire"),
    ("serial tx role", "uses[nu++] = {c.pin, PinRole::Input};\n      uses[nu++] = {c.pin2, PinRole::Output};", "uses[nu++] = {c.pin, PinRole::Input};\n      uses[nu++] = {c.pin2, PinRole::Input};"),
    ("motor pwm role", "uses[nu++] = {c.pin, PinRole::Pwm};\n      uses[nu++] = {c.pin2, PinRole::Output};", "uses[nu++] = {c.pin, PinRole::Output};\n      uses[nu++] = {c.pin2, PinRole::Output};"),
    ("same pin twice", "if (uses[v].pin == uses[u].pin) return DevErr::PinInUse;", ""),
    ("servo min", "c.minUs < 300", "c.minUs <= 300"),
    ("servo max", "c.maxUs > 3000", "c.maxUs >= 3000"),
    ("servo order", "c.minUs >= c.maxUs", "c.minUs > c.maxUs"),
    ("servo initial", "c.initial > 180) return DevErr::Initial", "c.initial > 181) return DevErr::Initial"),
    ("pca low", "c.i2cAddr < 0x40", "c.i2cAddr < 0x41"),
    ("pca high", "c.i2cAddr > 0x7F", "c.i2cAddr > 0x7E"),
    ("pca allcall", "|| c.i2cAddr == 0x70) return DevErr::I2cAddr", ") return DevErr::I2cAddr"),
    ("pca channel", "c.channel > 15", "c.channel > 16"),
    ("motor initial", "if (c.initial != 0) return DevErr::Initial;", "if (c.initial > 1) return DevErr::Initial;"),
    ("interval low", "c.intervalS < 1 ||", "c.intervalS < 0 ||"),
    ("interval high", "c.intervalS > 3600", "c.intervalS > 3601"),
    ("bme addr", "c.i2cAddr != 0x76 && c.i2cAddr != 0x77", "c.i2cAddr != 0x75 && c.i2cAddr != 0x77"),
    ("long low", "c.longMs < 100", "c.longMs < 99"),
    ("long high", "c.longMs > 10000", "c.longMs > 10001"),
    ("serial len min", "len >= 1 && len < kTextCap", "len > 1 && len < kTextCap"),
    ("serial len max", "len >= 1 && len < kTextCap", "len >= 1 && len <= kTextCap"),
    ("serial needs isText", "c.isText ? strnlen(c.text, kTextCap) : 0", "strnlen(c.text, kTextCap)"),
    ("servo has no stop", "t == DeviceType::Motor ? CmdErr::Ok : CmdErr::BadAction", "CmdErr::Ok"),
    ("motor lo", "t == DeviceType::Motor ? -100 : 0", "t == DeviceType::Motor ? 0 : 0"),
    ("motor hi", "t == DeviceType::Motor ? 100 : 180", "t == DeviceType::Motor ? 180 : 180"),
    ("motor range", "(c.value >= lo && c.value <= hi)", "(c.value >= lo)"),
    ("motor direction", "bool rev = (speed < 0) != invert;", "bool rev = (speed < 0) == invert;"),
    ("motor two pins", "d.dir1 = d.duty && !rev; d.dir2 = d.duty && rev;", "d.dir1 = d.duty && rev; d.dir2 = d.duty && !rev;"),
    ("motor clamp", "if (speed > 100) speed = 100;", ""),
    ("servo rounding", "(uint32_t(maxUs - minUs) * uint32_t(angle) + 90) / 180", "(uint32_t(maxUs - minUs) * uint32_t(angle)) / 180"),
    ("servo angle clamp", "if (angle > 180) angle = 180;", ""),
    ("duty rounding", "(uint32_t(us) * 50 + 50) / 100", "(uint32_t(us) * 50) / 100"),
    ("pca rounding", "(uint32_t(us) * 4096 + 10000) / 20000", "(uint32_t(us) * 4096) / 20000"),
    ("known name bme", 'if (a == 0x76 || a == 0x77) return "BME280 / BMP280";', 'if (a == 0x76) return "BME280 / BMP280";'),
    ("known name oled", "a == 0x3C || a == 0x3D", "a == 0x3C"),
    ("err code addr", 'case DevErr::AddrInUse: return "address_in_use";', 'case DevErr::AddrInUse: return "bad_i2c_addr";'),
    ("err field addr", 'case DevErr::I2cAddr: case DevErr::AddrInUse: return "i2c_addr";', 'case DevErr::I2cAddr: return "i2c_addr";'),
    ("default servo addr", "c.type == DeviceType::Servo ? 0x40 : 0x76", "c.type == DeviceType::Servo ? 0x41 : 0x76"),
]

SENSOR = [
    ("le16", "p[0] | (p[1] << 8)", "p[0] | (p[1] << 7)"),
    ("h4 low nibble", "(h[4] & 0x0F)", "(h[4] & 0x07)"),
    ("h5 high nibble", "(h[4] >> 4)", "(h[4] >> 3)"),
    ("h1 offset", "c.H1 = a[25];", "c.H1 = a[24];"),
    ("raw pressure", "adcP = (int32_t(d[0]) << 12) | (int32_t(d[1]) << 4)", "adcP = (int32_t(d[0]) << 12) | (int32_t(d[1]) << 3)"),
    ("raw temp", "adcT = (int32_t(d[3]) << 12) | (int32_t(d[4]) << 4)", "adcT = (int32_t(d[3]) << 11) | (int32_t(d[4]) << 4)"),
    ("raw hum", "adcH = (int32_t(d[6]) << 8) | d[7];", "adcH = (int32_t(d[6]) << 7) | d[7];"),
    ("temp scale", "(tFine * 5 + 128) >> 8", "(tFine * 5 + 128) >> 7"),
    ("temp t3", "* int32_t(c.T3)) >> 14;", "* int32_t(c.T3)) >> 13;"),
    ("press offset", "int64_t p = 1048576 - adcP;", "int64_t p = 1048575 - adcP;"),
    ("press p7", "(int64_t(c.P7) << 4)", "(int64_t(c.P7) << 3)"),
    ("press div guard", "if (v1 == 0) return 0;", ""),
    ("hum low clamp", "if (x < 0) x = 0;", ""),
    ("hum high clamp", "if (x > 419430400) x = 419430400;", "if (x > 419430400) x = 419000000;"),
    ("hum scale", "float(uint32_t(x) >> 12) / 1024.0f", "float(uint32_t(x) >> 12) / 1000.0f"),
    ("crc poly", "crc ^= 0x8C;", "crc ^= 0x8D;"),
    ("ds 85 guard", "if (raw == 0x0550) return false;", ""),
    ("ds scale", "float(raw) / 16.0f", "float(raw) / 8.0f"),
    ("ds crc check", "if (dallasCrc8(s, 8) != s[8]) return false;", ""),
]

QUEUE = [
    ("full check", "if (next == tail_) return false;", ""),
    ("push advances", "head_ = next;\n    return true;", "return true;"),
    ("pop empty", "if (tail_ == head_) return false;", ""),
    ("pop advances", "tail_ = (tail_ + 1) % N;", ""),
    ("replace direction", "i = (i + N - 1) % N;", "i = (i + 1) % N;"),
    ("replace condition", "if (same(items_[i])) { items_[i] = v; return true; }", "if (!same(items_[i])) { items_[i] = v; return true; }"),
    ("replace always misses", "if (same(items_[i])) { items_[i] = v; return true; }", "if (same(items_[i])) { return true; }"),
    ("size", "return (head_ + N - tail_) % N;", "return (head_ - tail_) % N;"),
]

LIMITER = [
    ("window reset", "if (uint32_t(now - windowStart) >= windowMs) { windowStart = now; count = 0; }", "if (uint32_t(now - windowStart) >= windowMs) { windowStart = now; }"),
    ("window edge", "uint32_t(now - windowStart) >= windowMs", "uint32_t(now - windowStart) > windowMs"),
    ("limit edge", "if (count >= maxPerWindow) return false;", "if (count > maxPerWindow) return false;"),
    ("count", "count++;\n    return true;", "return true;"),
]

# P5: editing a device in place (validateDevice's skip) and the config rules.
EDIT = [
    ("skip dup loop", "    if (int(i) == skip) continue;\n    if (!strcmp(others[i].id", "    if (!strcmp(others[i].id"),
    ("skip freq loop", "if (int(i) != skip && usesPwmSlot(others[i])", "if (usesPwmSlot(others[i])"),
    ("skip pin loop", "      if (int(i) == skip) continue;\n      if (isI2cDevice(c) && isI2cDevice(others[i])) {", "      if (isI2cDevice(c) && isI2cDevice(others[i])) {"),
    ("skip full discount", "n - (skip >= 0 ? 1 : 0) >= kMaxDevices", "n >= kMaxDevices"),
    ("skip wrong index", "    if (int(i) == skip) continue;\n    if (!strcmp(others[i].id", "    if (int(i) == skip + 1) continue;\n    if (!strcmp(others[i].id"),
    ("skip ignores all", "    if (int(i) == skip) continue;\n    if (!strcmp(others[i].id", "    if (skip >= 0) continue;\n    if (!strcmp(others[i].id"),
]

CONFIG = [
    ("schema none ok", "if (version < 0) return SchemaCheck::Ok;", "if (version < 0) return SchemaCheck::Bad;"),
    ("schema zero bad", "if (version < 1) return SchemaCheck::Bad;", "if (version < 0) return SchemaCheck::Bad;"),
    ("schema too new", "if (version > kConfigSchema) return SchemaCheck::TooNew;", "if (version > kConfigSchema + 1) return SchemaCheck::TooNew;"),
    ("file list network", '"/config/network.json",     "/config/network.json.tmp",', '"/config/network.json.tmp",'),
    ("file list bak", '"/config/devices.json.bak", "/config/devices.json.new",', '"/config/devices.json.new",'),
    ("file list new", '"/config/devices.json.bak", "/config/devices.json.new",', '"/config/devices.json.bak",'),
    ("is config file", "if (!strcmp(f, path)) return true;", "if (!strncmp(f, path, 8)) return true;"),
]

# P6: dashboard bindings and layout rules.
DASH = [
    ("relay writable", "case DeviceType::Relay: out[n++] = {Prop::State, true}; break;", "case DeviceType::Relay: out[n++] = {Prop::State, false}; break;"),
    ("input writable", "out[n++] = {Prop::State, false}; out[n++] = {Prop::Count, false}; break;", "out[n++] = {Prop::State, true}; out[n++] = {Prop::Count, false}; break;"),
    ("input count", "out[n++] = {Prop::State, false}; out[n++] = {Prop::Count, false}; break;", "out[n++] = {Prop::State, false}; break;"),
    ("sensor bme extras", "if (d.model == 1) { out[n++] = {Prop::Pressure, false}; out[n++] = {Prop::Humidity, false}; }", "if (d.model == 0) { out[n++] = {Prop::Pressure, false}; out[n++] = {Prop::Humidity, false}; }"),
    ("sensor ok", "      out[n++] = {Prop::Ok, false};\n", ""),
    ("serial text", "case DeviceType::Serial: out[n++] = {Prop::Text, false}; out[n++] = {Prop::Count, false}; break;", "case DeviceType::Serial: out[n++] = {Prop::Count, false}; break;"),
    ("level motor", "case DeviceType::Motor: lo = -100; hi = 100; return true;", "case DeviceType::Motor: lo = 0; hi = 100; return true;"),
    ("level servo", "case DeviceType::Servo: lo = 0; hi = 180; return true;", "case DeviceType::Servo: lo = 0; hi = 181; return true;"),
    ("level pwm", "case DeviceType::Pwm: lo = 0; hi = 100; return true;", "case DeviceType::Pwm: lo = 0; hi = 99; return true;"),
    ("need toggle", "case CompType::Toggle: return Need::WriteState;", "case CompType::Toggle: return Need::WriteLevel;"),
    ("need push", "case CompType::PushButton: return Need::Action;", "case CompType::PushButton: return Need::Card;"),
    ("need dropdown", "    case CompType::Joystick:\n    case CompType::Dropdown: return Need::WriteLevel;", "    case CompType::Joystick: return Need::WriteLevel;\n    case CompType::Dropdown: return Need::ReadNumber;"),
    ("need graph", "    case CompType::Progress:\n    case CompType::Graph: return Need::ReadNumber;", "    case CompType::Progress: return Need::ReadNumber;\n    case CompType::Graph: return Need::ReadState;"),
    ("need status", "case CompType::Status: return Need::ReadState;", "case CompType::Status: return Need::ReadNumber;"),
    ("need text", "case CompType::Text: return Need::ReadText;", "case CompType::Text: return Need::ReadNumber;"),
    ("need group", "case CompType::Group: return Need::Devices;", "case CompType::Group: return Need::Card;"),
    ("accept state write", "case Need::WriteState: return p == Prop::State && write;", "case Need::WriteState: return p == Prop::State;"),
    ("accept level write", "case Need::WriteLevel: return p == Prop::Level && write;", "case Need::WriteLevel: return p == Prop::Level;"),
    ("accept number count", "p == Prop::Pressure || p == Prop::Count;", "p == Prop::Pressure;"),
    ("accept number level", "return p == Prop::Level || p == Prop::Temp ||", "return p == Prop::Temp ||"),
    ("accept state ok", "case Need::ReadState: return p == Prop::State || p == Prop::Ok;", "case Need::ReadState: return p == Prop::State;"),
    ("accept text", "case Need::ReadText: return p == Prop::Text;", "case Need::ReadText: return p == Prop::Text || p == Prop::State;"),
    ("min joystick", "if (t == CompType::Joystick || t == CompType::Group) { w = 2; h = 2; }", "if (t == CompType::Group) { w = 2; h = 2; }"),
    ("min group", "if (t == CompType::Joystick || t == CompType::Group) { w = 2; h = 2; }", "if (t == CompType::Joystick) { w = 2; h = 2; }"),
    ("min graph", "if (t == CompType::Graph) w = 2;", ""),
    ("comp id", "if (!text(c.id, kIdCap, false) || !detail::validDeviceId(c.id)) return DashErr::Id;", "if (!text(c.id, kIdCap, false)) return DashErr::Id;"),
    ("comp label", "if (!text(c.label, kLabelCap, true)) return DashErr::Label;", ""),
    ("layout zero", "if (c.col < 1 || c.row < 1 || c.w < 1 || c.h < 1 || c.w > kMaxSpan || c.h > kMaxSpan) return DashErr::Layout;", "if (c.w > kMaxSpan || c.h > kMaxSpan) return DashErr::Layout;"),
    ("layout col", "if (c.col < 1 || c.row < 1", "if (c.col < 0 || c.row < 1"),
    ("layout span h", "c.w > kMaxSpan || c.h > kMaxSpan) return DashErr::Layout;\n  if (c.col", "c.w > kMaxSpan || c.h > kMaxSpan + 1) return DashErr::Layout;\n  if (c.col"),
    ("layout right edge", "if (c.col + c.w - 1 > kGridCols || c.row > kMaxRow) return DashErr::Layout;", "if (c.col + c.w > kGridCols || c.row > kMaxRow) return DashErr::Layout;"),
    ("layout max row", "c.row > kMaxRow) return DashErr::Layout;", "c.row >= kMaxRow) return DashErr::Layout;"),
    ("layout min size", "if (c.w < mw || c.h < mh) return DashErr::Layout;", ""),
    ("tab bound", "if (c.tab >= (nTabs ? nTabs : 1)) return DashErr::Tab;", "if (c.tab > (nTabs ? nTabs : 1)) return DashErr::Tab;"),
    ("tab implicit", "if (c.tab >= (nTabs ? nTabs : 1)) return DashErr::Tab;", "if (c.tab >= nTabs) return DashErr::Tab;"),
    ("group empty", "if (c.nDevs < 1 || c.nDevs > kMaxGroupDevices) return DashErr::Device;", "if (c.nDevs > kMaxGroupDevices) return DashErr::Device;"),
    ("group max", "c.nDevs > kMaxGroupDevices) return DashErr::Device;", "c.nDevs > kMaxGroupDevices + 1) return DashErr::Device;"),
    ("group dup", "if (!strcmp(c.devs[i], c.devs[j])) return DashErr::Device;", ""),
    ("group exists", "if (!text(c.devs[i], kIdCap, false) || !find(ctx, c.devs[i], d)) return DashErr::Device;", "if (!text(c.devs[i], kIdCap, false)) return DashErr::Device;"),
    ("device exists", "if (!text(c.device, kIdCap, false) || !find(ctx, c.device, d)) return DashErr::Device;", "if (!text(c.device, kIdCap, false)) return DashErr::Device;"),
    ("action check", "return checkCommand(d.type, c.cmd) == CmdErr::Ok ? DashErr::Ok : DashErr::Action;", "return DashErr::Ok;"),
    ("prop check", "if (!devicePropAccess(d, c.prop, write) || !needAccepts(need, c.prop, write)) return DashErr::Prop;", "if (!devicePropAccess(d, c.prop, write)) return DashErr::Prop;"),
    ("prop offered", "if (!devicePropAccess(d, c.prop, write) || !needAccepts(need, c.prop, write)) return DashErr::Prop;", "if (!needAccepts(need, c.prop, true)) return DashErr::Prop;"),
    ("joystick same", "|| !strcmp(c.device, c.device2)) return DashErr::Device;", ") return DashErr::Device;"),
    ("joystick axis2", "if (!devicePropAccess(d2, Prop::Level, w2) || !w2) return DashErr::Prop;", ""),
    ("joystick exists", "if (!text(c.device2, kIdCap, false) || !find(ctx, c.device2, d2)", "if (!text(c.device2, kIdCap, false)"),
    ("dropdown count", "if (c.nOpts < 1 || c.nOpts > kMaxOptions) return DashErr::Options;", "if (c.nOpts > kMaxOptions) return DashErr::Options;"),
    ("dropdown max", "c.nOpts > kMaxOptions) return DashErr::Options;", "c.nOpts > kMaxOptions + 1) return DashErr::Options;"),
    ("dropdown range", "if (c.opts[i].value < lo || c.opts[i].value > hi) return DashErr::Options;", "if (c.opts[i].value > hi) return DashErr::Options;"),
    ("dropdown range hi", "if (c.opts[i].value < lo || c.opts[i].value > hi) return DashErr::Options;", "if (c.opts[i].value < lo) return DashErr::Options;"),
    ("dropdown label", "if (!text(c.opts[i].label, kOptCap, false)) return DashErr::Options;", ""),
    ("tabs max", "if (n > kMaxTabs) return DashErr::Tabs;", "if (n > kMaxTabs + 1) return DashErr::Tabs;"),
    ("tabs empty", "if (!dashdetail::text(names[i], kTabCap, false)) return DashErr::Tabs;", "if (!dashdetail::text(names[i], kTabCap, true)) return DashErr::Tabs;"),
    ("tabs dup", "if (!strcmp(names[i], names[j])) return DashErr::Tabs;", ""),
    ("set limit", "if (n_ >= kMaxComponents) return DashErr::TooMany;", "if (n_ > kMaxComponents) return DashErr::TooMany;"),
    ("set dup", "if (!strcmp(ids_[i], c.id)) return DashErr::Duplicate;", ""),
    ("set tab", "bool apart = r.tab != c.tab ||", "bool apart = "),
    ("set right", "c.col + c.w <= r.col ||", "c.col + c.w < r.col ||"),
    ("set left", "r.col + r.w <= c.col ||", "r.col + r.w < c.col ||"),
    ("set below", "c.row + c.h <= r.row ||", "c.row + c.h < r.row ||"),
    ("set above", "r.row + r.h <= c.row;", "r.row + r.h < c.row;"),
    ("err overlap", 'case DashErr::Overlap: return "overlap";', 'case DashErr::Overlap: return "overlaps";'),
    ("err device", 'case DashErr::Device: return "unknown_device";', 'case DashErr::Device: return "bad_device";'),
    ("field device", 'case DashErr::Device: return "binding.device";', 'case DashErr::Device: return "device";'),
]

SETS = [
    ("devices", "src/devices/DeviceTypes.h", "test_devices", DEVICES),
    ("p4", "src/devices/DeviceTypes.h", "test_p4", P4),
    ("sensor math", "src/devices/SensorMath.h", "test_p4", SENSOR),
    ("edit", "src/devices/DeviceTypes.h", "test_p5", EDIT),
    ("config rules", "src/config/ConfigRules.h", "test_p5", CONFIG),
    ("dashboard", "src/dashboard/DashboardRules.h", "test_p6", DASH),
    ("queue", "src/devices/OpQueue.h", "test_ws", QUEUE),
    ("limiter", "src/net/RateLimiter.h", "test_ws", LIMITER),
]

def run_tests(folder):
    r = subprocess.run([sys.executable, "-m", "platformio", "test", "-e", "native", "-f", folder],
                       cwd=ROOT, capture_output=True, text=True)
    return r.returncode == 0

survivors, total = [], 0
for label, rel, folder, mutants in SETS:
    path = os.path.join(ROOT, rel)
    original = open(path, encoding="utf-8", newline="").read()
    if not run_tests(folder):
        print(f"BASELINE FAILS for {label}: fix the tests first"); sys.exit(1)
    try:
        for name, old, new in mutants:
            total += 1
            if original.count(old) != 1:
                print(f"BAD MUTANT {label}/{name} (pattern found {original.count(old)}x)"); survivors.append(f"{label}/{name}"); continue
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(original.replace(old, new))
            caught = not run_tests(folder)
            print(("caught   " if caught else "SURVIVED ") + f"{label}/{name}")
            if not caught: survivors.append(f"{label}/{name}")
    finally:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(original)
        os.utime(path, (time.time(), time.time()))  # restored file must look newer or the build reuses the mutant
    if not run_tests(folder):
        print(f"RESTORE FAILED for {label}"); sys.exit(1)

print(f"{total - len(survivors)}/{total} caught")
sys.exit(1 if survivors else 0)
