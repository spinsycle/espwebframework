# Changelog

## 0.0.1 - P0
- Repo scaffold, three PlatformIO envs (nodemcuv2, esp32dev, native), smoke blink, gate script.

## P2 (unreleased until hardware checklist passes)
- Device registry and state manager: relay, digital input (debounce, edge count), PWM; up to 32 devices, 8 PWM.
- Pin policy for both chips (flash, UART0, input-only, no-PWM, no-pull-up, boot-sensitive warnings, pin-in-use).
- REST: GET/POST/DELETE /api/v1/devices, POST /api/v1/command. Handlers validate and queue; loop() applies.
- Devices persisted to /config/devices.json; relays always start off unless configured.
- HAL: CriticalSection and PWM (ESP8266 analogWrite, ESP32 LEDC).
- UI: Devices tab, show/hide button on password fields.
- Radio power-save off; Wi-Fi join progress logged every 10 s.
- Native tests (19) mutation-checked 32/32 (`tools/mutate_devices.py`); live API test `tools/api_smoke.ps1`.
- Changed: default AP address 192.168.73.1 (192.168.4.1 overlapped common home LANs).

## P1 (unreleased until hardware checklist passes)
- HAL subset (chip facts, heap, restart, LittleFS) for ESP8266 and ESP32.
- Network manager: AP / STA / AP+STA, static IP, auto-reconnect, recovery AP after 30 s of lost STA.
- ESPAsyncWebServer REST: /api/v1/system, /status, /system/restart, /network (GET, PUT).
- Vanilla JS dashboard shell in ui/, gzipped into the LittleFS image by tools/build_ui.py.
- Native tests for network config validation (mutation-checked).
