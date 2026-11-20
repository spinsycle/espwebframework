# Relay Controller

One relay (D7 / GPIO13, active-low) and a push button (D5 / GPIO14 to GND). Seed rules: button toggles the lamp; lamp on at 18:00 and off at 23:00 (needs Wi-Fi client for NTP; offset 0 UTC, set `utc_offset_min` in `seed/automation.json`).

Flash: `python -m platformio run -e nodemcuv2 -t uploadfs` then `-t upload`. `uploadfs` wipes settings and resets the password to `espframework`.
