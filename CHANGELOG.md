# Changelog

## P4
- New device types: DC motor (H-bridge, one or two direction pins, signed speed, stop), servo (direct pin or PCA9685 channel, calibrated pulse range), sensor (DS18B20 on 1-Wire, BME280 / BMP280 on I2C), serial device (send a line, last received line).
- Input gains an optional long-press time (`long_ms`) and a long-press counter.
- I2C bus on fixed pins (ESP8266 GPIO4/5, ESP32 GPIO21/22), a scan (`POST /api/v1/i2c/scan`, `GET /api/v1/i2c`) and address-conflict checks.
- Sensors are read without blocking loop(); three misses in a row show "no reading". The 85.0 C power-on value is never shown as a temperature.
- HAL: PWM duty now in 1/10000 steps (servo pulses need it); ESP32 gives each PWM device its own LEDC timer; a second UART (software serial on ESP8266, UART2 on ESP32).
- Limits to know: ESP8266 has one PWM frequency, so a direct-pin servo (50 Hz) cannot share the chip with a 1000 Hz PWM or motor (`freq_conflict`); one DS18B20 per pin; one serial device, 38400 baud at most on the ESP8266; PCA9685 pulses assume its nominal 25 MHz clock, so use the servo min/max pulse fields to calibrate.
- Native tests 61 (sensor math checked against the Bosch datasheet example), mutation check `tools/mutate_devices.py` extended; live API test `tools/api_smoke.ps1` extended.
- ESP32 app image is now 73 % of the default 1.3 MB partition: P8 (OTA) needs a custom partition table.

## 0.0.1 - P0
- Repo scaffold, three PlatformIO envs (nodemcuv2, esp32dev, native), smoke blink, gate script.

## P3
- WebSocket /ws: hello, devices_changed, device_state, status (2 s), error events; commands and ping from the client.
- Max 3 live pages (4th is refused and the page falls back to polling), 40 messages per second per page.
- Slider bursts coalesce to the latest value per device (shared OpQueue); REST and WS share one command parser and error map.
- UI: live updates, automatic reconnect with backoff, heartbeat, polling fallback, "Live pages" card.
- Native tests 38, mutation check 52/52 (`tools/mutate_devices.py`); `tools/ws_smoke.py` live test written but not yet run against the board.

## P2
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
