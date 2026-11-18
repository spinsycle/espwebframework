# Safety and known limits

## Read this first
- **Software settings are not electrical protection.** Failsafes, interlocks, limits and the enable switch are conveniences. A relay driving mains, a motor or a heater needs its own fuse, thermal cut-out and hardware interlock. The firmware can hang, restart or lose Wi-Fi.
- **Trusted LAN only.** There is no HTTPS: the password and session token cross the network in clear text. Never forward a port to this board or put it on the internet.
- One shared admin password, no per-user accounts. The factory password (`espframework`) must be changed at first sign-in; the board forces it.
- Strapping pins (flagged `boot_sensitive`) can stop the board booting if the wiring pulls them the wrong way. Outputs float or glitch during boot and reset: do not put anything dangerous on a pin that has not been through a power-up test.
- A restart (crash, watchdog, OTA, power) puts relays to their `initial` state. A relay with `initial` 1 switches on at boot.

## What the board does on its own
- Failsafe: a device with `failsafe_s` goes safe if no command or heartbeat arrives in time. Only a visible open page sends heartbeats; a script that wants a device kept alive must `POST /api/v1/heartbeat` or send commands.
- Watchdog: a hung loop resets the chip; the reason shows in `/diagnostics` and the event log.
- Lock-out: five wrong passwords lock the login for 30 s.

## Known limits
- No HTTPS (above). MD5 is used only as the firmware updater's integrity check.
- Sessions (3 at most) and the 12-entry event log live in RAM; a restart clears them.
- `config/export` carries devices only: no Wi-Fi passwords, rules, sequences or dashboard.
- Timers and running sequences are not restored after a restart.
- A lost password is recovered by re-flashing the filesystem (`uploadfs`, which also wipes every setting) or a factory reset from a signed-in page.
- The ESP8266 has about 15 KB of free heap and refuses new connections for roughly two minutes after a burst of hundreds. Normal use (a few browser tabs, a script polling every few seconds) is fine; a stress script is not.
- Manual pin testing is deliberately absent: there is no endpoint that drives a raw pin. `GET /gpio` is read only.
- The watchdog test works on the ESP8266 only.
- Verified on ESP8266 NodeMCU hardware. The ESP32 build compiles and passes the native tests, but its hardware checklists have not been run.
- A rule's action is checked against the device 1.5 s after it fires and an unconfirmed result is counted, not retried. A sequence `do` step is not verified: follow it with an `until` step.
- Daylight saving is not automatic: the time zone is a fixed offset.
