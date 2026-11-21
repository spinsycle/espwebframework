# Changelog

## Unreleased
mDNS (F1 of the v1.1 plan). Additive: `network.json` gains `mdns.enabled` (default off) and `mdns.name`; old files load unchanged, schema stays 1, no route or error-code change.
- Network tab: "Name on your network (mDNS)" checkbox and name; `relays` -> `http://relays.local`. Runs only with the Wi-Fi client connected; off means no responder and nothing else changes.
- `GET /status` reports `network.mdns`.

MQTT (F2 of the v1.1 plan). New dependency `marvinroger/AsyncMqttClient@0.9.0` (rides the AsyncTCP the web server already uses; picked over `espMqttClient`, which costs about 1.5 KB more static RAM on the ESP8266).
- New `mqtt.json` (host, port, user, password, base topic, keep-alive, enabled; off by default), `GET`/`PUT /api/v1/mqtt`, an MQTT box on the Network tab, and `mqtt` in `GET /status`.
- Publishes each device's state, retained, on change (`<base>/<id>/state`); accepts `<base>/<id>/set` (on, off, toggle, stop, a number, serial text) through `DeviceRegistry::requestCommand`; retained commands are ignored; last will `<base>/status`.
- Reconnects with a 5 s to 60 s back-off; will not start a connection under about 8 KB free heap on the ESP8266.
- `network.json` now keeps a `.bak` and checks `schema_version` on load; `kConfigFiles` lists the new files so a factory reset removes them.
- ESP8266 static RAM 61.7% -> 63.3%; ESP32 flash 83.7% -> 86.6%.

Master / slave over REST (F3 of the v1.1 plan). Additive; a board that never sets a role behaves exactly as before (master).
- New `role.json` (role, master address and password, up to 8 button links), `GET`/`PUT /api/v1/role`, a Role box on the Network tab, `role` in `GET /status`.
- A slave watches its own input devices (`DeviceRegistry::inputSnapshot`) and sends `toggle` / `on` / `off` / `set` to the master's `POST /api/v1/command` on a press, long press or release. Logs in once, keeps the session token, logs in again on a `401`; one request at a time.
- A command not delivered within 10 s is dropped, never sent late (an irrigation valve must not open minutes after the button press). A wrong password stops login attempts for 10 minutes; a master on the factory password is refused.
- The master needs nothing new: the slave uses the existing REST API.
- Remote state mirroring (showing the master's relay state on the slave) is not in this release.
- **ESP32 family only.** The ESP8266 build compiles the role code out (`src/core/Features.h`, `ESPWF_HAS_ROLE`): the slave's TCP client and buffers did not leave enough heap for the web server on a NodeMCU. An ESP8266 is always a master, `/api/v1/role` answers 404 there, and the Role box stays hidden. ESP8266 static RAM is 63.4%, back where F2 left it.

ESP32-S3 support (new `esp32s3` environment, board `esp32-s3-devkitc-1`). Compile-checked and covered by native tests; not yet run on a board.
- New `Chip::Esp32S3` with its own pin rules: GPIO 22-25 do not exist; 26-37 (flash and octal PSRAM), 19-20 (native USB) and 43-44 (UART0) are reserved; 46 is input-only; strapping pins 0, 3, 45, 46; I2C on GPIO 8 / 9; the GPIO inspector lists 0-48.
- PWM: the S3's 8 LEDC channels share 4 timers, so all PWM devices on an S3 must use one frequency (like the ESP8266). The classic ESP32 is unchanged.
- The classic ESP32 and ESP8266 behaviour is unchanged (the `Chip` enum only gains a value at the end).

Four demo apps and a seed mechanism. Tooling and examples only; no API, config, device or firmware change.
- `tools/build_ui.py`: an app's `seed/{devices,automation,dashboard}.json` ship in the filesystem image as `/config/*.json`, so a fresh `uploadfs` boots with the app's devices and rules. The build fails on invalid JSON or a `schema_version` other than 1; the firmware still validates entries at boot (a bad entry is skipped).
- `examples/`: `relay_controller` (seeded), `light_sequencer`, `thermostat`, `pan_tilt_rover`, each with a README and wiring table.
- `tools/gate.ps1` builds every example for both chips and builds their filesystem images.

## v1.0.1
Dashboard fixes and a user guide. Only the Devices tab and the Automation tab text change; no API, config, device or firmware behaviour change.
- Devices tab: only the first device card had disable, edit and remove (`devices.map(devCard)` passed the index as the `bare` flag). All cards have them now.
- Automation tab: six double-encoded middle dots showed as `Â·`.
- New `docs/USER_GUIDE.md` with screenshots.

## v1.0.0
First stable release: the full spec on the ESP8266 NodeMCU (every phase P0-P8b passed its hardware checklist), the ESP32 build compiling and passing the native tests with its hardware checklists still open. From here the `/api/v1` surface, the `schema_version` 1 config format, device type and model names and error codes are frozen (`tools/check_frozen.py`): additions only. See `docs/SAFETY.md` for the known limits.

Soak result, stated plainly: two 24 hour runs on the NodeMCU. The heap stayed flat (no leak, no watchdog or fault reset, lowest low-water mark 6.4 KB on the second run, with a gentler poller). Neither run was clean: the board was reset from outside three times each run (reset reason `External System`, in the afternoon; cause not pinned down, suspected USB power) and it stopped accepting new HTTP connections for minutes at a time while still answering ping, recovering by itself. v1.0 is tagged with those open. The install-guide dry run (checklist Part B) has not been done yet.

## P9b (docs and examples)
- Docs: `docs/API.md` (every route and WebSocket message), `DRIVERS.md` (every device type and how to add one), `CONFIG.md` (the config files and the automation JSON), `INSTALL.md` (PlatformIO, flashing, first sign-in), `SAFETY.md` (what the software does not protect and the known limits). README rewritten.
- `examples/relay_controller`: a complete app (one `main.cpp`) built by the gate-style build; the framework comes in as a library.
- `tools/build_ui.py` now also works in an app repo: with no `ui/` of its own it uses the one inside the installed framework library (including a `symlink://` install).
- Freeze check: `tools/check_frozen.py` compares 222 API items (routes, WebSocket messages and events, device types and models, trigger, operator, action, step and component names, error codes, config schema version) with `tools/frozen_api.txt`; a removal or rename fails the gate, an addition fails until frozen on purpose with `--update`. Routes are checked by path, not method.
- `tools/soak.py`: the 24 hour polling logger used for the memory acceptance run.

## P8b
- Failsafe per device (relay, PWM, motor, servo): `failsafe_s` (0 = off, up to 3600) is how long the device may go without a command **or a heartbeat** before it goes to its safe state: relay off, PWM 0 %, motor stopped, servo holds or moves to `safe_angle` (0-180, -1 / absent = hold). The board does this on its own in `loop()`, with no browser involved. It latches: the device stays safe until the next command to it (a heartbeat does not resume it), the state shows `failsafe_tripped`, and an event is logged. Inputs, sensors and the serial port have nothing to switch off and refuse a failsafe (`bad_failsafe`).
- Heartbeat: `POST /api/v1/heartbeat` or the WebSocket message `{"type":"heartbeat"}` from any signed-in client restarts every failsafe timer. The UI sends one a second while a page is open **and visible**, so a hidden tab, a locked phone or a closed page lets the failsafe fire. Commands from automation count as commands. A device given a failsafe on a board with no controller connected goes safe that many seconds after boot.
- Enable / disable: `enabled` (default true) on every device. A disabled device sits in its safe state (a relay set to "at boot = on" stays off), refuses commands with `409 disabled`, and an input, sensor or serial device stops being read. Toggled from the device card (a partial `PUT /api/v1/devices` with `{"id":..., "enabled":...}`); enabling restarts the device like any edit.
- System event log: the last 12 events (boot with its reset reason, `boot_fault`, failsafe, device enabled/disabled, failed login, lock-out (one event, not one per refused attempt), password change, firmware update, restart, factory reset, watchdog test). RAM only: a restart starts it again. Each has the uptime and, once the clock is set, the wall-clock time.
- `GET /api/v1/diagnostics` (chunked): the status plus reset reason, `watchdog_reset` / `fault_reset`, the longest gap between loop passes, the heap low-water mark, a per-device table (enabled, failsafe, tripped, sensor read errors) and the event log.
- `GET /api/v1/gpio`: every pin of the chip with its NodeMCU label, a note (`strapping`, `flash`, `uart`, `input_only`, `no_pwm`), the device that uses it and its level now (flash and UART pins are never read). **Read only**: there is no pin-writing endpoint and no test mode; manual pin testing is left out on purpose so it cannot operate connected hardware.
- Watchdog testing: `POST /api/v1/system/test-watchdog` with `{"confirm":"hang"}` hangs the loop on purpose so the chip resets itself and the next boot reports it. ESP8266 only (the Arduino loop task is not watched on the ESP32 by default: it answers 501 there).
- UI: a **Diagnostics** tab (health cards, device table, events, automation history, GPIO table refreshing every 3 s, watchdog test), Enabled / Failsafe / Failsafe angle fields on the device form, enable/disable on each card, a "Failsafe tripped" note.
- WebSocket: `device_state` now also goes out when a motor speed, servo angle, sensor `ok` or a failsafe state changes (the comparison used to look at relay, PWM and edge count only).
- Native tests 253 (new: failsafe timing across the millis wrap, safe targets, config limits, event ring, reset-reason parser, GPIO facts), mutation check 74 more mutants, live API test extended (`tools/api_smoke.ps1` section P8b: failsafe fires and is held off by heartbeats, disable/enable, diagnostics, GPIO, auth on the new routes). The smoke needs every other page closed: their heartbeat would keep the failsafe from firing.
- Memory: static RAM 61.3% (about 50 KB of 80 KB), 2 KB more than P8a (the event ring and the new per-device fields); ESP32 flash 81.5% of its slot. P9 trims.
- Test note: `tools/api_smoke.ps1` makes hundreds of short connections and the ESP8266 cannot accept new ones until its lingering connections time out (about 2 minutes). Start a run at least 2 to 3 minutes after the previous one (or after a reboot), or the next run fails in a cascade that looks like a firmware bug but is not (shown by repeated clean 324/324 runs on a rested board).
- Found on the way: the smoke's OTA section failed because .NET sends multipart names unquoted and the board's parser wants them quoted; the smoke now quotes them. A LittleFS overlap crash seen once during testing did not recur; the write guard stays.

## P8a
- Login. Everything under `/api/` and the WebSocket needs a session; only the UI files and `/api/v1/session` are public (the login page has to load). A fresh or factory-reset board uses the password `espframework` and refuses everything except changing it until you do (the UI forces it). The change needs the current password, takes 8 to 32 printable ASCII characters, refuses the factory password, and signs every other session out.
- Passwords are stored as a salted hash (SHA-256, 500 rounds, a 16-byte hardware-random salt) in `/config/security.json`; the password itself is never kept. Comparison takes the same time whatever is wrong. The SHA-256 is our own small implementation, tested against the published vectors including the one-million-`a` message.
- Sessions: 16-byte random tokens, at most 3 at once (the longest idle is replaced), ending after an hour without use (any request renews it). Carried in an `HttpOnly`, `SameSite=Strict` cookie `sid` (the browser) or `Authorization: Bearer <token>` (scripts). Held in RAM: a restart signs everyone out.
- Lock-out: five wrong passwords in a row lock the login for 30 s (even the right password is refused during it); existing sessions are not affected.
- WebSocket: the session is read at the handshake and re-checked every 3 s and on every message (messages renew it), so a logged-out, expired or password-changed session loses its socket. A busy socket keeps its session alive.
- Firmware update: `POST /api/v1/system/update?md5=<hex>` multipart upload of a `.bin`, written with the Arduino updater; the image is rejected if it is not a firmware image, does not fit, or does not match the checksum, and the board keeps running the old firmware. The upload callback checks the session itself (it runs before the route-level check). The UI computes the MD5 in the browser (MD5 is only the updater's integrity check; the login protects who may update). The update restarts the board when it succeeds.
- UI: a sign-in gate that covers the page until the board says there is a session, the forced password change, a Log out button, a Security card (change password) and a Firmware update card with a progress bar on the System tab; the page stops reconnecting its socket when signed out.
- `tools/api_smoke.ps1` signs in first (`-Password`, and `-NewPassword` for a fresh board), checks that every route refuses a request with no or a bad session (including with harmless bodies on the write routes), tests sessions, logout, the cookie, the password change and its restore, refused firmware uploads and the lock-out; `-Ota` also installs the built firmware and checks the board comes back.
- Limits: no HTTPS (the password and token cross the LAN in clear text: this is a trusted-LAN device, never expose it to the internet); one shared admin password, no per-user accounts; a lost password is recovered by re-flashing the filesystem (`uploadfs`) or a factory reset from a signed-in page.
- Memory: the first live test of P8a showed the ceiling: an automation status reply (about 1.9 KB) came back cut off, because `AsyncResponseStream` keeps the whole body in one growing buffer and silently truncates when the fragmented heap cannot grow it. Every large JSON reply (device list, config export, automation config, automation status, timers) now goes out as a chunked response that holds one list item at a time. To get RAM back: the ESP8266 now allows 16 devices (32 on the ESP32, where there is room), the command queue is 6 deep, and the SHA-256 round constants live in flash. Static RAM is 58.8% (about 48 KB of 80 KB). On the ESP32 the default partition table already has two app slots, so OTA works there without a custom table (the image is 80% of a slot).
- Native tests 227, mutation check extended with 85 login and SHA-256 mutants; live API test extended.

## P7b
- Sequences: a list of steps run by the board with no browser open. A step is **do** (one action, optionally only if a condition holds, otherwise skip the step or stop the sequence), **wait** (1 s to 24 h) or **until** (wait for a device condition, with a timeout that either carries on or stops the sequence). Start, stop, pause, resume; a sequence repeats a number of times or until stopped (a repeating sequence must contain a wait or until step). Status per sequence: idle / running / paused, current step, pass, seconds left to wait, finished and stopped counts. The engine does at most 8 steps in a pass and never uses `delay()`.
- Timers: named countdowns (1 s to 24 h), optionally repeating. Started and stopped from a rule, a sequence step, the Automation tab, the REST API or a dashboard Timer. Editing a timer stops it. Timers and sequences are runtime state: a restart leaves them stopped and idle.
- New rule triggers: a timer finishes, and a sequence finishes (or, as a separate choice, is stopped early by stop, a guard or a timeout). New rule actions: start / stop a sequence, start / stop a timer. A sequence cannot start or stop a sequence (no recursion); it can start and stop timers.
- Dashboard: the 14th component, **Timer**, completing the spec's list: shows the time left, a progress bar and Start / Stop; at least 2 wide; it refuses a timer that does not exist.
- Limits: 4 sequences, 16 steps each and 32 steps in all (about 80 bytes a step, allocated only for steps that exist), 6 timers; the earlier rule limits are unchanged. Interlocks also guard sequence steps. A do-step is not verified afterwards (unlike a rule action): follow it with an until step to wait for the result. A failed step is logged and the sequence carries on.
- Switching the engine off freezes evaluation, but the clock does not stop: a timer or wait that came due while it was off completes right after it is switched on again.
- REST: `GET/POST/PUT/DELETE /api/v1/automation/sequences` and `/timers` (whole definition in the body, `?id=` for delete), `POST /automation/sequences/start|stop|pause|resume?id=`, `POST /automation/timers/start|stop?id=`, `GET /automation/timers` (light, for the dashboard). `GET /automation` and `/automation/status` now include sequences and timers.
- Known limits: deleting a sequence or timer that a rule uses is allowed (the rule reports `no_device` when it runs); the sequence and timer definitions are not part of the export / import bundle; no authentication until P8.
- Memory: static RAM rose again (60.0% of the ESP8266's 80 KB). If heap gets tight the engine's fixed tables can be trimmed in P9.
- Native tests 198, mutation check extended with 256 sequence, timer and dashboard-timer mutants; live API test extended.

## P7a
- Automation engine, rules half. A rule is a trigger, optional conditions and up to four actions; rules are stored in `/config/automation.json` (temp file then rename, previous copy as `.bak`, removed by factory reset) and run entirely on the board, with no browser open.
- Triggers: a value changes, conditions become true (fires once each time they go from false to true; the only trigger that accepts "for N seconds"), every N seconds, at a time of day on chosen weekdays, board startup, Wi-Fi connects or drops, and run by hand. The spec's "sequence completes" and timer events arrive with P7b.
- Conditions: ==, !=, >, <, >=, <= on any numeric device property, and time-of-day windows (a window may cross midnight); up to three, combined with AND or OR (one operator per rule, no nesting).
- Actions: turn on / off / toggle a relay, set a PWM, motor or servo, stop a motor, send a line to the serial device, send an event (logged). Spec items not built: "set motor direction" (the sign of the speed does it), "change device mode" (no device has one), "update dashboard status".
- Safeguards: at most 3 conditions and 4 actions, 16 rules (about 340 bytes each, allocated only for rules that exist), per-rule cooldown, a 200 ms minimum between firings, an engine-wide limit of 30 firings a second against rules that trigger each other, interlocks (pairs of relays a rule may not turn on together; they guard rules only, not the dashboard or the API), and a bounded amount of work per pass.
- A rule "firing" is not a device "done": every on / off / set / stop is checked against the device 1.5 s later and an unconfirmed command is counted and logged. Per rule: fired, failed, unconfirmed, last result.
- Clock: NTP on both chips once the Wi-Fi client is connected; the time zone is a fixed UTC offset in minutes set on the Automation tab (no automatic daylight saving). Without a clock, schedule triggers never fire and time windows read as closed, and the tab says so.
- REST: `GET /api/v1/automation`, `/automation/status`, `/automation/schema`; `PUT /automation/settings` (engine switch, offset, interlocks); `POST` / `PUT` / `DELETE /automation/rules` (the id of a rule goes in the body, or `?id=` for delete); `POST /automation/rules/enable|disable|run?id=`. Changes are validated against the live devices, queued, and applied by the main loop.
- UI: an Automation tab with a rule list in plain English, live per-rule status, an editor, the clock and interlock settings, and the last eight events.
- Known limits: no authentication until P8; the rule list is not part of the export / import bundle; rules whose device was removed stay and report `no_device` when they run; a stored rule is not re-validated at boot.
- Native tests 131, mutation check extended with 148 automation mutants; live API test extended.

## P6
- Dashboard engine. `GET /api/v1/dashboard` returns the saved layout (automatic mode with no components when nothing is saved); `PUT /api/v1/dashboard` validates a whole layout against the registered devices and stores it in `/config/dashboard.json` (temp file then rename, previous copy kept as `.bak`, removed by factory reset); `GET /api/v1/dashboard/schema` publishes the component types, grid size and limits so the editor offers only bindings the firmware will accept.
- Device list entries now carry `props`: what a component may bind to (relay `state` read/write, pwm / motor / servo `level` read/write, sensor `temp_c` / `humidity` / `pressure_hpa` / `ok`, input `state` / `count`, serial `text` / `count`).
- 13 component types: toggle, push button, slider, joystick (two devices), gauge, number display, status light, graph, text display, dropdown, progress bar, device card, group panel. The spec's Timer waits for P7 (automation), where it has something to bind to.
- Layout: 4-column grid, up to 64 rows, components 1-4 wide and tall (joystick and group panel at least 2x2, graph at least 2 wide), up to 6 tabs and 32 components, no overlap on a page. On a phone the page stacks into one column in reading order.
- Automatic mode builds a layout from the device list in the browser (nothing stored). Manual mode is edited with arrow and size buttons (no drag), a Preview toggle, Save and Cancel. A refused save names the component and the reason and changes nothing.
- Automatic mode keeps a stored manual layout untouched and unchecked; switching to manual checks it again, so a layout that points at a removed device is refused with the component named.
- Limits to know: the layout body is at most 6 KB (the editor shows a counter); graphs keep the last 60 points in the browser only; the dashboard is not part of the export / import bundle.
- Native tests 93 (dashboard rules), mutation check extended; live API test extended.

## P5
- Config system: every saved file carries `schema_version` (files from before P5 count as version 1; a file from a newer framework is refused, never guessed at). `devices.json` keeps the previous good copy as `devices.json.bak` and loading falls back to it.
- Export `GET /api/v1/config/export` (device list, no live state, no Wi-Fi passwords; the network settings are not part of it). Import `POST /api/v1/config/import` replaces every device: the whole file is validated first (each device against the ones before it), a failure names the device (`index`, `id`, `reason`, `field`) and changes nothing; a good import is written to disk and the board restarts.
- Factory reset `POST /api/v1/config/factory-reset` with `{"confirm":"factory-reset"}` deletes every saved config file and restarts; the UI files are kept.
- Edit a device in place `PUT /api/v1/devices` (body merged onto the device named by `id`; the type cannot change). Validation ignores the device's old self, so keeping your own pin or changing the only PWM device's frequency is allowed. The device restarts with its new settings (a relay returns to its boot value).
- UI: edit button on each device card, Configuration card on the System tab (export, import, factory reset with confirmations).
- Known limits: import body at most 6 KB, 32 devices; network settings stay on the Network tab; no auth until P8, so anyone on the LAN can export, import or reset.
- Native tests 72, mutation check extended (device-edit and config rules sets); live API test extended.

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
