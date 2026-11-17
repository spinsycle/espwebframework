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
    ("full check", "if (n >= kMaxDevices) return DevErr::Full;", "if (n > kMaxDevices) return DevErr::Full;"),
    ("duplicate id", "if (!strcmp(others[i].id, c.id)) return DevErr::Duplicate;", ""),
    ("pwm limit", "pwmCount >= kMaxPwm", "pwmCount > kMaxPwm"),
    ("pin in use", "if (others[i].pin == c.pin) return DevErr::PinInUse;", ""),
    ("pullup check", "c.pullup && !pinHasPullup(chip, c.pin)", "c.pullup && pinHasPullup(chip, c.pin)"),
    ("debounce limit", "c.debounceMs > 1000", "c.debounceMs > 1001"),
    ("freq upper", "c.freqHz > 20000", "c.freqHz > 20001"),
    ("freq lower", "c.freqHz < 1 ||", "c.freqHz < 0 ||"),
    ("relay initial", "c.type == DeviceType::Relay && c.initial > 1", "c.type == DeviceType::Relay && c.initial > 2"),
    ("pwm initial", "c.initial > 100) return DevErr::Initial", "c.initial > 101) return DevErr::Initial"),
    ("input not commandable", "if (t == DeviceType::Input) return CmdErr::NotCommandable;", ""),
    ("relay set needs bool", "return (c.hasValue && c.isBool) ? CmdErr::Ok", "return (c.hasValue) ? CmdErr::Ok"),
    ("pwm rejects bool", "if (!c.hasValue || c.isBool) return CmdErr::BadValue;", "if (!c.hasValue) return CmdErr::BadValue;"),
    ("pwm upper bound", "c.value <= 100)", "c.value <= 101)"),
    ("pwm lower bound", "c.value >= 0 &&", "c.value >= -1 &&"),
    ("pwm toggle", "if (c.action != CmdAction::Set) return CmdErr::BadAction;", ""),
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

SETS = [
    ("devices", "src/devices/DeviceTypes.h", "test_devices", DEVICES),
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
