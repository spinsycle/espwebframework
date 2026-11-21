# Configuration reference

All settings live as JSON under `/config/` on LittleFS. Every file has `"schema_version": 1`; a file with a higher version is refused, not guessed at. Writes go to a `.tmp` file and are renamed; the previous copy is kept as `.bak` and used if the main file is damaged.

| File | Holds | In `config/export`? |
|---|---|---|
| `network.json` | mode, hostname, mDNS (`enabled`, `name`), AP and STA settings, Wi-Fi passwords | no |
| `mqtt.json` | broker host and port, user, password, base topic, keep-alive, enabled | no (holds a password) |
| `role.json` | role (master / slave), the master's address and password, the slave's button links | no (holds a password) |
| `devices.json` | the device list | yes |
| `dashboard.json` | dashboard layout | no |
| `automation.json` | settings, rules, sequences, timers | no |
| `security.json` | salted password hash (never the password) | no |

Factory reset (`POST /config/factory-reset`) deletes exactly these files. `uploadfs` replaces the whole filesystem and wipes them too.

After v1.0 the format changes only by **adding optional fields**. Unknown fields are ignored on read.

## devices.json
`{"schema_version":1,"devices":[ {device}, ... ]}`. A device is the object in [DRIVERS.md](DRIVERS.md); omitted fields take their defaults.

## network.json
Same shape as `GET /network`, with the passwords included. See [API.md](API.md#network).

## automation.json

```json
{"schema_version":1,"enabled":true,"utc_offset_min":0,"interlocks":[["relay_a","relay_b"]],
 "rules":[{"id":"night","name":"night","enabled":true,
   "trigger":{"type":"schedule","time":"22:00","days":[1,2,3,4,5]},
   "match":"all",
   "conditions":[{"device":"temp","property":"temp_c","operator":">","value":30,"for_s":0}],
   "actions":[{"type":"off","device":"pump"},{"type":"event","text":"night"}],
   "cooldown_s":0}],
 "sequences":[{"id":"cycle","name":"cycle","repeat":0,"steps":[
   {"type":"do","action":{"type":"on","device":"pump"}},
   {"type":"wait","seconds":30},
   {"type":"until","condition":{"device":"temp","property":"temp_c","operator":"<","value":25},"timeout_s":600,"on_timeout":"continue"}]}],
 "timers":[{"id":"t1","name":"t1","duration_s":60,"repeat":true}]}
```

- **Trigger types:** `change` (device + property), `when` (conditions go from false to true; only this one takes `for_s`), `interval` (`every_s`), `schedule` (`time` HH:MM, `days` 0-6 with 0 = Sunday; needs a synced clock), `startup`, `wifi` (`event`: `connected` or `disconnected`), `manual`, `timer` (`device` = timer id), `sequence` (`device` = sequence id, `event`: `done` or `stopped`).
- **Conditions:** `{"device","property","operator","value","for_s"}` with operators `==  !=  >  <  >=  <=`, or a time window `{"type":"time","from":"HH:MM","to":"HH:MM","days":[...]}` (may cross midnight). `match` is `all` or `any`, one operator per rule.
- **Actions:** `on off toggle set stop send event seq_start seq_stop timer_start timer_stop`. `set` takes `value`; `send` and `event` take `text` (15 chars).
- **Steps:** `do` (an `action`, optional `guard` condition and `else`: `skip` or `stop`), `wait` (`seconds`, up to 24 h), `until` (a `condition`, `timeout_s`, `on_timeout`: `continue` or `stop`). `repeat` 0 = forever, and then the sequence must contain a wait or until.
- The time zone is a fixed offset in minutes (`utc_offset_min`, -720..840), no automatic daylight saving. NTP runs once the Wi-Fi client is connected. Without a clock, schedules never fire.
- Limits: 16 rules, 3 conditions, 4 actions, 4 interlocks, 4 sequences, 16 steps each and 32 in all, 6 timers. Timers and sequence progress are runtime state: a restart leaves them stopped.

The authoritative list is always `GET /api/v1/automation/schema`.

## dashboard.json
`{"schema_version":1,"mode":"auto"|"manual","tabs":[...],"components":[...]}`. `GET /api/v1/dashboard/schema` returns the component types, the grid (4 columns, 64 rows, spans 1-4), the limits (32 components, 6 tabs) and which device properties each component may bind to. The firmware rejects a layout that binds to a missing device or to a property it cannot read or write. Easiest is to build a layout in the editor and read it back with `GET /api/v1/dashboard`.

## security.json
Salt and SHA-256 hash of the admin password. Do not edit by hand.
