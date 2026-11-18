# API reference (v1)

REST under `/api/v1/*`, JSON in and out. WebSocket at `/ws`. LAN only, plain HTTP (see [SAFETY.md](SAFETY.md)).

Everything below needs a session except `POST /api/v1/session` and the UI files. Call a route without one and you get `401 {"error":"unauthorized"}`. A board still on the factory password answers `403 password_change_required` on everything except `PUT /api/v1/password`.

Conventions:
- Errors are `{"error":"<code>","field":"<name>"}` (`field` only when one field is at fault).
- Writes that change a device or the automation engine are **queued** and applied by the main loop: they answer `202 {"ok":true}`. Read back with a GET after about a second.
- `503 busy` means the board is writing its filesystem or its queue is full: retry.
- Big replies (device list, export, automation) are chunked; read until the connection ends and parse the whole body.
- Paths for DELETE and the start/stop/enable routes take `?id=`.

## Session

| Route | Body | Reply |
|---|---|---|
| `POST /session` | `{"password":"..."}` | `200 {"ok":true,"token":"<32 hex>","must_change":false}`; also sets cookie `sid` (`HttpOnly`, `SameSite=Strict`). Wrong password `401`; five wrong in a row `429 {"error":"locked","retry_s":30}` |
| `GET /session` | | `{"authenticated":bool,"must_change":bool,"name":"..."}`. Works without a session |
| `DELETE /session` | | logs out |
| `PUT /password` | `{"current":"...","password":"..."}` | New password 8-32 printable ASCII, not the factory one. Signs every other session out |

Scripts send `Authorization: Bearer <token>`. At most 3 sessions; idle sessions end after 1 hour; a restart signs everyone out.

## System

| Route | Reply |
|---|---|
| `GET /system` | `{"device":{"id","name"},"framework_version","chip","cpu_mhz","flash_size","sdk","build"}` |
| `GET /status` | uptime, reset reason, `heap{free,max_block,fragmentation_pct}`, filesystem, `ws_clients`, network summary |
| `GET /diagnostics` | status plus `heap.min_free`, `watchdog_reset`, `fault_reset`, `loop_max_ms`, `events_total`, per-device table, last 12 `events` |
| `GET /gpio` | every pin: NodeMCU label, note (`strapping`, `flash`, `uart`, `input_only`, `no_pwm`), owning device, level now. Read only |
| `POST /heartbeat` | restarts every failsafe timer. Same as the WebSocket `heartbeat` message |
| `POST /system/restart` | restarts the board |
| `POST /system/update?md5=<hex>` | multipart upload of a firmware `.bin` (field names must be quoted). Rejected if not a firmware image, too big, or MD5 differs. Restarts on success |
| `POST /system/test-watchdog` | `{"confirm":"hang"}`. Hangs the loop so the chip resets. ESP8266 only (`501` on ESP32) |

## Network

`GET /network` returns the config (passwords as `password_set` booleans). `PUT /network` takes any subset:

```json
{"mode":"ap_sta","hostname":"espwf-550517",
 "ap":{"ssid":"ESPWF-550517","password":"min8chars","channel":1,"ip":"192.168.73.1"},
 "sta":{"ssid":"Home","password":"...","static":false,"ip":"","gateway":"","mask":""}}
```

`mode` is `ap`, `sta` or `ap_sta`. Reply `{"ok":true,"apply_in_ms":...}`: the change applies after the reply goes out. Errors: `400 invalid_network_config` with `field` (`mode`, `hostname`, `ap.ssid`, `ap.password`, `ap.channel`, `ap.ip`, `sta.*`, `format`). A bad AP password change can lock you out: after `uploadfs` the board is back at the factory settings.

## Devices

`GET /devices` -> `{"devices":[{"id","name","type","pin",...,"enabled","failsafe_s","state":{...},"props":[...]}]}`. `props` lists what a dashboard component may bind to and whether it is writable.

`POST /devices` adds, `PUT /devices` edits (send `id` plus the changed fields; editing restarts the device), `DELETE /devices?id=x` removes. All `202`. Fields per type are in [DRIVERS.md](DRIVERS.md). Errors (`400` unless noted): `bad_id`, `bad_name`, `bad_type`, `bad_pin`, `pin_reserved`, `pin_input_only`, `pin_no_pwm`, `pin_no_pullup`, `pin_in_use`, `bad_debounce`, `bad_freq`, `bad_initial`, `duplicate_id` (409), `too_many_devices`, `too_many_pwm`, `not_found` (404), `bad_model`, `bad_i2c_addr`, `bad_channel`, `bad_pulse`, `bad_interval`, `bad_baud`, `bad_long_ms`, `freq_conflict`, `too_many_serial`, `address_in_use`, `bad_failsafe`, `bad_safe_angle`, `bad_format`.

Limits: 16 devices (ESP8266) or 32 (ESP32), 8 PWM-timed outputs (pwm, motor, direct servo), one serial device.

### Commands

`POST /command` with `{"id":"relay1","action":"set","value":true}` -> `202`.

| Device | action | value |
|---|---|---|
| relay | `set` / `toggle` | `true`/`false` (set only) |
| pwm | `set` | 0-100 (percent) |
| motor | `set` / `stop` | -100..100 (sign = direction) |
| servo | `set` | 0-180 (degrees) |
| serial | `send` | text, 1-47 characters |
| input, sensor | not commandable (`409 not_commandable`) | |

Errors: `bad_id`, `bad_action`, `bad_value`, `not_found` (404), `not_commandable` (409), `disabled` (409), `busy` (503).

### I2C

`GET /i2c` returns the last scan; `POST /i2c/scan` starts one (`202`; poll `GET /i2c`).

## Config

| Route | Notes |
|---|---|
| `GET /config/export` | `{"schema_version":1,"devices":[...]}`. Devices only: no Wi-Fi passwords, rules, sequences or dashboard |
| `POST /config/import` | the same shape. Validated whole first; **replaces all devices** or changes nothing |
| `POST /config/factory-reset` | `{"confirm":"factory-reset"}` (`202`). Removes every file under `/config`, restarts at factory settings and the factory password |

## Dashboard

`GET /dashboard`, `PUT /dashboard` (whole layout, validated against the live devices), `GET /dashboard/schema` (component types, grid, limits). Nothing saved = `{"schema_version":1,"mode":"auto","tabs":[],"components":[]}`.

## Automation

Shapes are in [CONFIG.md](CONFIG.md). `GET /automation/schema` lists trigger, operator, action and step types and the limits (16 rules, 3 conditions, 4 actions, 4 sequences, 16 steps each, 32 steps in all, 6 timers).

| Route | Notes |
|---|---|
| `GET /automation` | settings, rules, sequences, timers |
| `GET /automation/status` | per-rule counters, sequence and timer state, last 8 events |
| `PUT /automation/settings` | `{"enabled":bool,"utc_offset_min":int,"interlocks":[["a","b"]]}` |
| `POST` / `PUT /automation/rules` | whole rule in the body (id inside). `POST` 409 if the id exists, `PUT` 404 if not |
| `DELETE /automation/rules?id=` | |
| `POST /automation/rules/enable\|disable\|run?id=` | |
| `POST` / `PUT /automation/sequences`, `DELETE ?id=` | |
| `POST /automation/sequences/start\|stop\|pause\|resume?id=` | |
| `GET /automation/timers` | light list for the dashboard |
| `POST` / `PUT /automation/timers`, `DELETE ?id=` | |
| `POST /automation/timers/start\|stop?id=` | |

All writes `202` and validated against the live devices first.

## WebSocket `/ws`

Needs a session at the handshake (cookie or `Authorization` header; a browser sends the cookie). At most 3 clients (a fourth is closed with 1013). 40 messages a second per client, 256 bytes a message.

Client to board:

```json
{"type":"ping"}
{"type":"heartbeat"}
{"type":"command","id":"relay1","action":"set","value":true}
```

Board to client:

| `event` | Meaning |
|---|---|
| `hello` | `{"event":"hello","proto":1}` on connect; then `GET /devices` |
| `pong` | answer to `ping` |
| `status` | the `/status` object, every 2 s |
| `device_state` | `{"event":"device_state","id":"...","state":{...}}` on any change |
| `devices_changed` | the list changed: refetch `GET /devices` |
| `error` | `{"event":"error","error":"<code>","field":"...","id":"..."}`; codes as for commands plus `bad_json`, `too_large`, `bad_type`, `rate_limited` |

A successful command has no reply; `device_state` follows. The board closes a socket whose session ended.

## Frozen at v1.0

Route paths and methods above, the WebSocket message and event names, device type names, `model` names, and every error code string. Additions are allowed; removals and renames are not.
