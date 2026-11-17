# P8a hardware checklist: login, sessions and firmware update

Flash firmware and UI (`run -t upload`, then `run -t uploadfs`). `uploadfs` wipes saved settings, **including the password**: the board comes back on the factory password `espframework` and makes you change it. Redo the Network save after.
Tick each row on the NodeMCU; the ESP32 column stays open until the board arrives. Keep a second device (phone) handy.

| # | Check | NodeMCU | ESP32 |
|---|-------|---------|-------|
| 1 | Open the board's page: only a sign-in form shows (the device name as its title). No device names, values or tabs are visible behind it | | |
| 2 | A wrong password says "Wrong password." Five wrong tries in a row: the sixth (even with the **right** password) says "Too many wrong passwords. Try again in N s." and N counts down from about 30. After that the right password works | | |
| 3 | Sign in with `espframework` on the fresh board: a "Change password" form appears with **no Cancel** button and the current password already filled. The page behind it stays locked (reload the page: you land on the same form, not the dashboard) | | |
| 4 | The form refuses: a new password that differs from the repeat; fewer than 8 characters; 33 or more characters; `espframework` itself ("That is the factory password"). It accepts a valid one and the dashboard loads | | |
| 5 | Header now shows **Log out**. Press it: you are back at the sign-in form. Reload and press the browser Back button: no data appears; you need the password again | | |
| 6 | On a phone, sign in at the same time. Both work. On the phone change the password (System tab, Security, Change password): within a few seconds the other browser drops to the sign-in form, the phone stays signed in. Sign in on the PC with the new password; the old one no longer works | | |
| 7 | Live updates (relay toggle on one browser appearing on the other) still work after signing in. After **Log out** on one browser, that browser stops getting live updates and stops trying to reconnect (no flashing) | | |
| 8 | From a terminal (no browser): `curl http://<ip>/api/v1/devices` answers `401 {"error":"unauthorized"}`; `curl -X POST http://<ip>/api/v1/system/restart` answers 401 and does **not** restart the board; `curl -X POST -H "Content-Type: application/json" -d "{\"password\":\"<yours>\"}" http://<ip>/api/v1/session` returns a token, and `curl -H "Authorization: Bearer <token>" http://<ip>/api/v1/devices` then returns the devices | | |
| 9 | Idle expiry: leave a signed-in page untouched for over an hour (close the laptop lid or use another tab, nothing on that page), then use it: it drops to the sign-in form. A page you keep using does **not** drop out | | |
| 10 | Restart the board (System tab): your password still works afterwards (it is saved), and every browser has to sign in again | | |
| 11 | System tab, **Factory reset** (confirm): the board restarts on its own access point; sign in with `espframework` and you are forced to change it again; devices, Wi-Fi settings, rules, sequences, timers and the dashboard are all gone | | |
| 12 | Firmware update, refusals: choose a `.txt` file and Install: "That is not a .bin file." Rename any text file to `x.bin` and Install: "That file does not look like a firmware image." Nothing changes on the board | | |
| 13 | Firmware update, good: rebuild so the build time changes (`python -m platformio run -e nodemcuv2`), choose `.pio\build\nodemcuv2\firmware.bin`, Install, confirm: a progress bar fills, "Installed. The board is restarting...", the page reloads by itself (about 30 s), you sign in, and System tab **Built** shows the new build time | | |
| 14 | Firmware update, damaged file: copy `firmware.bin`, cut it to about half its size (e.g. `head -c 200000 firmware.bin > half.bin` in Git Bash), install it: it is refused (checksum or size) and the board keeps running the **old** build (Built unchanged). Closing the browser tab half way through an upload also leaves the old firmware running | | |
| 15 | Still working after the update: devices, dashboard layout, automation rules, sequences and timers are all intact, and your password is unchanged | | |
| 16 | Memory: System heap stays above 6 KB free after rows 6-15, and the page still saves a rule and a sequence without `out_of_memory` | | |
| 17 | `tools\api_smoke.ps1 -Ip <ip> -Password <yours>` (on a fresh board add `-NewPassword <8-32 chars>`): every check passes. It ends with a 32-second wait for the lock-out test | | |
| 18 | `tools\api_smoke.ps1 -Ip <ip> -Password <yours> -Ota`: also installs the built firmware, waits for the restart and signs in again; every check passes | | |
