# P8b hardware checklist: failsafes, enable/disable, diagnostics

Flash firmware and UI (`run -t upload`, then `run -t uploadfs`). `uploadfs` wipes saved settings, **including the password**: the board comes back on the factory password `espframework` and makes you change it. Redo the Network save after.
Tick each row on the NodeMCU; the ESP32 column stays open until the board arrives. Have an LED or a meter on the relay pin, and a second device (phone) handy.

How the failsafe works: a device with a failsafe time (seconds, 0 = off) goes to its safe state when it has had **no command and no heartbeat** for that long. A page that is open **and visible** sends a heartbeat every second; a hidden tab, a locked phone or a closed page sends none. Safe state: relay off, PWM 0 %, motor stopped, servo holds (or moves to its failsafe angle if one is set). It stays safe until the **next command** to that device (a heartbeat does not resume it).

| # | Check | NodeMCU | ESP32 |
|---|-------|---------|-------|
| 1 | Devices tab, add a relay `fs1` with **Failsafe after 5**. Save. Edit it: Enabled is ticked, Failsafe shows 5. Restart the board (System tab): the edit form still shows 5 | | |
| 2 | Heartbeat keeps it alive: with the page open and visible, switch `fs1` ON and watch for 20 s. It stays ON and no "Failsafe tripped" note appears | | |
| 3 | Switch `fs1` ON, then hide the page (switch to another browser tab, or lock the phone). About 5 s later the relay goes OFF by itself. Come back: the card says "Failsafe tripped: it is in its safe state. Send a command to resume." and the relay is still OFF | | |
| 4 | Press the toggle: the relay turns ON, the note goes away. Close the page entirely and wait 6 s: the relay goes OFF (the board does this on its own, no browser involved) | | |
| 5 | Second viewer: open the page on the phone and the PC, switch `fs1` ON, hide the PC tab but keep the phone page visible. The relay stays ON (the phone's heartbeat counts). Lock the phone too: it goes OFF after about 5 s | | |
| 6 | Motor: add a motor with Failsafe 3 and drive it to 60 %. Hide the page: the motor stops after about 3 s and the card reads speed 0 | | |
| 7 | Servo: add a servo with Failsafe 3 and **Failsafe angle 30**; move it to 150. Hide the page: it swings to 30 after about 3 s. Edit the servo, leave the angle **blank**, move it to 150, hide the page: it **stays at 150** (hold) but the card still shows "Failsafe tripped" | | |
| 8 | Form checks: the Failsafe field is not shown for a digital input, sensor or serial device; Failsafe 3601 is refused ("bad_failsafe"); a failsafe angle of 181 is refused ("bad_safe_angle") | | |
| 9 | Disable: press **disable** on a relay that is ON. The card dims with "Disabled: in its safe state, commands are refused.", its buttons are greyed, and the relay goes OFF at once. A command from a second browser (or `curl -X POST .../api/v1/command`) answers `409 {"error":"disabled"}` | | |
| 10 | Press **enable**: the card is normal again and the toggle works. A relay with "At boot = 1" that is **disabled** stays OFF after a board restart (System tab) and still shows disabled | | |
| 11 | Disable a digital input or a sensor: its value stops updating (a sensor shows "no reading"); enable it and it resumes within one read interval | | |
| 12 | An automation rule whose action targets a disabled relay does nothing to the relay; the Automation history shows the action as failed. Enable it and the rule works again | | |
| 13 | Diagnostics tab: Uptime, Last reset, Longest loop gap, Memory (free, lowest, largest block), Storage and Live pages all show numbers. The Devices table lists every device with enabled/disabled, its failsafe seconds and **TRIPPED** when it fired, and sensor read errors | | |
| 14 | Recent system events shows, newest first: the boot line (with the reset reason), `failsafe` lines for the devices above (in red), `device` lines for each enable/disable. Wrong password three times on the sign-in form, then the right one: three `login_failed` lines appear. Five wrong in a row: one `login_locked` line (not one per attempt) | | |
| 15 | GPIO table: every NodeMCU pin is listed with its D-label (GPIO 5 = D1, 4 = D2, 16 = D0...), a note (`strapping` on 0, 2, 15; `flash` on 6-11; `uart` on 1 and 3; `no_pwm` on 16) and **Used by** for pins that belong to a device. Flash and UART pins show `-` for the level. There is no button anywhere that writes a pin | | |
| 16 | Toggle the relay and watch the table (it refreshes every 3 s): the relay pin's level flips HIGH / low and matches the LED or meter (an inverted relay reads the opposite of "ON") | | |
| 17 | Automation history on the Diagnostics tab lists the last rule and sequence events and matches the Automation tab | | |
| 18 | Watchdog test: Diagnostics tab, **Test the watchdog**, confirm. The board stops answering for a few seconds, then comes back by itself. **Last reset** reads a watchdog reason with the note "the watchdog reset the board: the loop stalled" in amber; the events show `boot_fault`. Outputs are back at their boot state (a relay with "At boot = 0" is OFF) | | |
| 19 | `curl -X POST -H "Content-Type: application/json" -d "{\"confirm\":\"hang\"}" http://<ip>/api/v1/system/test-watchdog` **without** signing in answers 401 and does not hang the board. With a session but a wrong word it answers 400 `confirm_required` | | |
| 20 | Everything from P8a still works: sign in, change password, log out, live updates, and the firmware update (install the built `.bin`: the board restarts and the events show `ota` then `boot`) | | |
| 21 | Memory: Diagnostics heap "lowest" stays above 3 KB after rows 1-19, and saving a rule and a sequence still works (no `out_of_memory`) | | |
| 22 | `tools\api_smoke.ps1 -Ip <ip> -Password <yours>` with **every other page closed** (their heartbeat would keep the failsafe from firing): all checks pass. It ends with the 32-second lock-out wait. Wait 3 minutes after any earlier test run first (the board refuses new connections for about 2 minutes after a burst) | | |
| 23 | `tools\api_smoke.ps1 -Ip <ip> -Password <yours> -Ota`: also installs the built firmware; all checks pass | | |

ESP32: the watchdog test answers 501 `not_supported` there (the Arduino loop task is not watched by default), so row 18 stays open for that chip. Everything else applies.
