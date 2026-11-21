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
- Over a 24 hour soak with one request every 20 s the board stayed up and its heap stayed flat, but it stopped accepting new connections for minutes at a time on several occasions (it answered ping, not HTTP) and recovered by itself. Treat HTTP as best-effort on the ESP8266: a client should retry, and nothing safety-relevant may depend on a request getting through (that is what failsafes are for). Twice in two runs the board also reset from outside (reset reason `External System`, not the watchdog) in the afternoon; power it from a wall adapter rather than a PC USB port for anything long-running.
- Manual pin testing is deliberately absent: there is no endpoint that drives a raw pin. `GET /gpio` is read only.
- The watchdog test works on the ESP8266 only.
- Fully verified on ESP8266 NodeMCU hardware (every phase's hardware checklist, two 24 hour soaks). The ESP32 DevKit (WROOM-32) has been run by hand and works (dashboard, network, devices; the PING))) sensor has not been tried with a real sensor yet), but its per-phase checklists are not ticked row by row and it has had no soak run. The ESP32-S3 target compiles and passes the native tests; it has not been run on a board.
- The PING))) sensor reads 5 V logic on its signal pin: wire it through a level shifter or divider, never straight to an ESP GPIO.
- A rule's action is checked against the device 1.5 s after it fires and an unconfirmed result is counted, not retried. A sequence `do` step is not verified: follow it with an `until` step.
- Daylight saving is not automatic: the time zone is a fixed offset.
