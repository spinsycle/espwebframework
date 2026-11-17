"""Mutation check for src/devices/DeviceTypes.h: break one thing at a time, the native tests must fail.
Run: python tools/mutate_devices.py   (needs g++ on PATH). Exit 1 if any mutant survives or the baseline fails.
"""
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE = os.path.join(ROOT, "src", "devices", "DeviceTypes.h")

MUTANTS = [
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
]

def run_tests():
    r = subprocess.run([sys.executable, "-m", "platformio", "test", "-e", "native", "-f", "test_devices"],
                       cwd=ROOT, capture_output=True, text=True)
    return r.returncode == 0

original = open(FILE, encoding="utf-8", newline="").read()
if not run_tests():
    print("BASELINE FAILS: fix the tests first"); sys.exit(1)

survivors = []
try:
    for name, old, new in MUTANTS:
        if original.count(old) != 1:
            print(f"BAD MUTANT (pattern found {original.count(old)}x): {name}"); survivors.append(name); continue
        with open(FILE, "w", encoding="utf-8", newline="") as f:
            f.write(original.replace(old, new))
        caught = not run_tests()
        print(("caught   " if caught else "SURVIVED ") + name)
        if not caught: survivors.append(name)
finally:
    with open(FILE, "w", encoding="utf-8", newline="") as f:
        f.write(original)
    os.utime(FILE, (time.time(), time.time()))  # restored file must look newer or the build reuses the mutant

if not run_tests():
    print("RESTORE FAILED"); sys.exit(1)
print(f"{len(MUTANTS) - len(survivors)}/{len(MUTANTS)} caught")
sys.exit(1 if survivors else 0)
