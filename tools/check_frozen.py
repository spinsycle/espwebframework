#!/usr/bin/env python3
"""Freeze check: the v1 API surface may grow but never lose or rename anything.

Extracts from src/ every route path, WebSocket message and event name, device type / model / trigger /
operator / action / step / component / property name, every error code string and the config schema
version, and compares with tools/frozen_api.txt.
  - a frozen line missing from the source (removed or renamed)  -> FAIL
  - a source line not in the manifest (an addition)             -> FAIL until frozen on purpose with --update
Exit 0 = intact. Run from anywhere:  python tools/check_frozen.py [--update] [--root DIR]

Limits: routes are checked by path, not by HTTP method; error codes are checked where they are
defined (the *ErrCode functions) and where HttpApi.cpp passes a literal to sendError.
"""
import argparse, os, re, sys

ap = argparse.ArgumentParser()
ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ap.add_argument("--manifest", default="tools/frozen_api.txt")
ap.add_argument("--update", action="store_true")
a = ap.parse_args()
os.chdir(a.root)


def read(p):
    return open(p, encoding="utf-8-sig").read()


# function name -> file; every non-empty string literal in its body is a frozen string
FUNCS = {
    "deviceTypeName": "src/devices/DeviceTypes.h", "modelName": "src/devices/DeviceTypes.h",
    "devErrCode": "src/devices/DeviceTypes.h", "cmdErrCode": "src/devices/DeviceTypes.h",
    "autoErrCode": "src/automation/AutomationRules.h", "trigName": "src/automation/AutomationRules.h",
    "opName": "src/automation/AutomationRules.h", "actName": "src/automation/AutomationRules.h",
    "stepName": "src/automation/AutomationRules.h", "dashErrCode": "src/dashboard/DashboardRules.h",
    "compTypeName": "src/dashboard/DashboardRules.h", "propName": "src/dashboard/DashboardRules.h",
    "authErrCode": "src/security/AuthCore.h", "netModeName": "src/net/NetworkConfig.h",
}
# parse functions hold the accepted spellings as strcmp(x, "literal")
PARSERS = {"parseDeviceType": "src/devices/DeviceTypes.h", "parseAction": "src/devices/DeviceTypes.h",
           "parseNetMode": "src/net/NetworkConfig.h"}

lines = []
problems = []


def body(text, name, path):
    m = re.search(r"\b" + name + r"\s*\([^)]*\)\s*\{", text)
    if not m:
        problems.append(f"function {name} not found in {path}")
        return ""
    i, depth = m.end(), 1
    while i < len(text) and depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i]


for fn, path in FUNCS.items():
    for lit in re.findall(r'"([^"]*)"', body(read(path), fn, path)):
        if lit:
            lines.append(f"{fn}: {lit}")
for fn, path in PARSERS.items():
    for lit in re.findall(r'strcmp\(\s*\w+\s*,\s*"([^"]+)"', body(read(path), fn, path)):
        lines.append(f"{fn}: {lit}")

http = read("src/net/HttpApi.cpp")
ws = read("src/net/WsHub.cpp")
for r in re.findall(r'"(/api/v1[^"]*|/ws)"', http + ws):
    lines.append(f"route: {r}")
for m in re.findall(r'strcmp\(\s*type\s*,\s*"([^"]+)"', ws):
    lines.append(f"ws message: {m}")
for e in re.findall(r'\["event"\]\s*=\s*"([^"]+)"|\\"event\\":\\"([^"\\]+)\\"', ws):
    lines.append("ws event: " + (e[0] or e[1]))
for c in re.findall(r'sendError\(\s*r\s*,\s*\d+\s*,\s*"([^"]+)"', http):
    lines.append(f"http error: {c}")
m = re.search(r"kConfigSchema\s*=\s*(\d+)", read("src/config/ConfigRules.h"))
if not m:
    problems.append("kConfigSchema not found")
else:
    lines.append(f"config schema_version: {m.group(1)}")

# sorted unique: the order of the source is not part of the contract
lines = sorted(set(lines))
if problems or len(lines) < 100:
    print("FROZEN CHECK: extraction problem: " + "; ".join(problems or [f"only {len(lines)} items found"]))
    sys.exit(1)

if a.update:
    open(a.manifest, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(f"wrote {a.manifest} ({len(lines)} items)")
    sys.exit(0)

frozen = [l for l in read(a.manifest).splitlines() if l]
gone = [l for l in frozen if l not in set(lines)]
new = [l for l in lines if l not in set(frozen)]
if gone:
    print("FROZEN API CHANGED (removed or renamed):\n" + "\n".join("  " + l for l in gone))
if new:
    print("API additions not frozen yet (run: python tools/check_frozen.py --update, read the diff):\n"
          + "\n".join("  " + l for l in new))
if gone or new:
    sys.exit(1)
print(f"frozen API intact ({len(frozen)} items)")
