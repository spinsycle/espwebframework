# P7a hardware checklist: automation rules

Flash firmware and UI (`run -t upload`, then `run -t uploadfs`; uploadfs wipes saved settings, so redo the Network save once).
The board needs to be on your **home Wi-Fi** (not only its own access point) for rows 7-9: that is how it gets the time of day.
Tick each row on the NodeMCU; the ESP32 column stays open until the board arrives.

Before you start, on the Devices tab have: a relay `r1` (GPIO14, an LED with a resistor on it is ideal), a second relay `r2` (GPIO12), a PWM `p1` (GPIO13), and a button `b1` (GPIO2, pull-up, inverted). A BME280 or DS18B20 sensor makes rows 5 and 6 easier; without one use the button and the relay.

| # | Check | NodeMCU | ESP32 |
|---|-------|---------|-------|
| 1 | A new **Automation** tab shows the engine switch (on), the clock line, "No rules yet", Add rule, Interlocks and Recent events | | |
| 2 | **Add rule**: Trigger "Run by hand only", action "Send an event" named `hello`, Save. The card appears with a plain-English sentence. **Run now**: under Recent events a `fired` and an `event hello` line appear within 3 s | | |
| 3 | New rule, trigger "Run by hand only", action Turn on `r1`. **Run now**: the LED lights, the card says `ran 1x` and `last: ok`, and no "not confirmed" warning appears | | |
| 4 | Rule with trigger "A value changes" on the button's count, action Toggle `r1`. Each button press toggles the LED once (not twice, not zero times). A second rule is not needed | | |
| 5 | Rule "Conditions become true": condition `r1` on/off `==` 1, action Set `p1` to 50. Turn `r1` on from the Devices tab: `p1` goes to 50 once. Turn `r1` off and on again: it fires again. Holding `r1` on does **not** fire it repeatedly (the run count stays put) | | |
| 6 | Same rule with the condition "for at least 5 seconds": turn `r1` on, then off after 3 s: nothing happens. Turn it on and keep it on: after 5 s the action runs | | |
| 7 | Clock: on the home Wi-Fi the clock line shows the time. Set the UTC offset for your zone (UTC-5 is -300), Save: the displayed time matches your wall clock. In access-point-only mode it says the clock is not set and warns that schedules and time windows are off | | |
| 8 | Rule trigger "At a time of day" 2 minutes from now (today only), action Turn on `r2`. It fires at the minute, once (not repeatedly for the whole minute). Set a day that is not today: it does not fire | | |
| 9 | Condition "Time of day" with a window that includes now and one that excludes now (try one that wraps midnight, e.g. 22:00 to 06:00): **Run now** only runs when the window is open | | |
| 10 | Rule trigger "Every so often" 10 s, action Toggle `r2`: it toggles every 10 s. Edit it to 20 s: the new period applies. **Disable** it: it stops, and the card is greyed. **Enable**: it resumes | | |
| 11 | Cooldown: a "value changes" rule on the button with cooldown 5 s: pressing three times in 3 s runs the action once, the card shows `last: cooldown` | | |
| 12 | Startup: rule trigger "When the board starts", action Turn on `r2`. Restart the board (System tab): `r2` comes on about 3 s after boot. A startup rule added *after* boot does not run until the next boot | | |
| 13 | Wi-Fi: rule trigger "Wi-Fi connects or drops" connects, action event `wifi up`. Restart the board: after it joins the router the event appears in the log | | |
| 14 | AND / OR: a manual rule with two conditions (`r1` on and the button's count above a number). With Match "all" it only runs when both hold; switch to "any" and it runs when one holds | | |
| 15 | Interlock: Interlocks section, pair `r1` and `r2`, Save. Turn `r1` on, then **Run now** on a rule that turns `r2` on: it is blocked (card `last: blocked`, `r2` stays off). Turn `r1` off and run again: it works. The Devices tab can still turn both on (the interlock only guards rules; the page says so) | | |
| 16 | Confirmation: with `r1` on, run a rule that turns it off. No warning. Then pull the LED wire's driver (or run a rule against a relay the board cannot reach): the rule shows an unconfirmed or failed count when the device does not follow | | |
| 17 | Safeguards: the editor refuses a 4th condition, a 5th action and a 17th rule; a "Conditions become true" rule with no conditions says so; a duration on a rule that is not "Conditions become true" is refused with a message | | |
| 18 | Storm guard: make two "value changes" rules that toggle each other's relay. The board does not lock up or reset; the Recent events log shows `storm` lines and the page keeps working | | |
| 19 | Engine switch: turn **Automation engine on** off: no rule runs (try Run now and wait 5 s; nothing). Turn it on: rules resume and the earlier Run now does **not** fire late | | |
| 20 | Persistence: with 3 rules, an offset and an interlock saved, restart the board: all of it is still there. Power-cut test: pull USB right after pressing Save on a rule; reconnect: the rule list is the old or new one, never empty | | |
| 21 | Memory: with 8 rules present, System heap stays above 10 KB free and does not keep shrinking over 10 minutes with the Automation tab open | | |
| 22 | `tools\api_smoke.ps1 -Ip <ip>`: every check passes | | |
| 23 | Factory reset: afterwards the Automation tab is empty, the engine is on, the offset is 0 | | |
