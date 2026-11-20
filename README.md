# espwebframework

Reusable web control framework for ESP8266 NodeMCU and ESP32 DevKit. One firmware library gives an app:

- Wi-Fi setup (own access point, home network, or both) and a browser dashboard built from your device list
- a device registry with drivers: relay, input, PWM, motor, servo (direct and PCA9685), DS18B20 and BME280 sensors, serial
- REST (`/api/v1`) and WebSocket (`/ws`) APIs
- JSON config on LittleFS, with export, import and factory reset
- automation: rules, timers and sequences that run on the board with no browser open
- admin password with sessions, authenticated OTA firmware update, per-device failsafes, diagnostics

LAN only. Plain HTTP, one shared password: not for the internet. MIT. Read [docs/SAFETY.md](docs/SAFETY.md) before connecting anything that can hurt.

## Use it
An app is a separate repo that pins this library by tag:

```ini
lib_deps = https://github.com/spinsycle/espwebframework.git#v1.0.0
```

```cpp
#include <Framework.h>
FrameworkController Framework;
void setup() { Framework.setDeviceName("My Device"); Framework.begin(); }
void loop()  { Framework.loop(); }
```

## Docs
- [User guide](docs/USER_GUIDE.md) (with screenshots)
- [Install and first run](docs/INSTALL.md)
- [API reference](docs/API.md)
- [Device drivers](docs/DRIVERS.md)
- [Configuration reference](docs/CONFIG.md)
- [Safety and known limits](docs/SAFETY.md)
- [Changelog](CHANGELOG.md)
- Examples: [`examples/relay_controller`](examples/relay_controller) (a complete app), [`examples/smoke`](examples/smoke) (the gate and test harness)

## Status
Verified on a NodeMCU (ESP8266). The ESP32 build compiles and passes the native tests; its hardware checklists are still open.

## Develop
`powershell -File tools/gate.ps1` builds both chips and both filesystem images and runs the native tests. `python tools/mutate_devices.py` runs the mutation check. `tools/api_smoke.ps1` tests a live board. `tools/soak.py` logs a long run.
