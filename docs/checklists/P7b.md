# P7b hardware checklist: sequences and timers

Flash firmware and UI (`run -t upload`, then `run -t uploadfs`; the UI changed so `uploadfs` is needed, and it wipes saved settings, so redo the Network save once).
Tick each row on the NodeMCU; the ESP32 column stays open until the board arrives.

Before you start, on the Devices tab have: a relay `r1` (GPIO14, an LED with a resistor), a relay `r2` (GPIO12), a PWM `p1` (GPIO13), a button `b1` (GPIO2, pull-up, inverted). A BME280 or DS18B20 sensor helps rows 7 and 8; without one use the button or a relay as the condition.

| # | Check | NodeMCU | ESP32 |
|---|-------|---------|-------|
| 1 | Automation tab: new **Sequences** and **Timers** sections with "No sequences yet" / "No timers yet" and Add buttons | | |
| 2 | **Add timer**: id `tm`, length 20 s. The card shows `0:20` and "stopped". **Start**: it counts down once a second on the card (0:19, 0:18...), the Stop button appears, and at zero it stops by itself with `finished 1x` and a `timer_done` line under Recent events | | |
| 3 | Start the timer again and press **Stop** half way: it stops and does not finish. Edit its length to 5 s while it is running: the edit stops it and the new length shows | | |
| 4 | Tick "Start again by itself" on a 5 s timer: it finishes every 5 s until you press Stop | | |
| 5 | **Add sequence** `startup`: step 1 do Turn on `r1`, step 2 Wait 3 s, step 3 do Turn on `r2`, step 4 Wait 2 s, step 5 do Set `p1` to 50, step 6 do Send an event `ready`. Save. The card reads in plain English. **Start**: `r1` lights at once, `r2` 3 s later, `p1` 2 s after that, the card shows `running · step 2 of 6 · N s` while it waits, then `idle · finished 1x` and a `sequence_done` event | | |
| 6 | **Pause** during the first wait: nothing happens for 10 s. **Resume**: the rest of the wait is honoured (not a fresh 3 s, not skipped). **Stop** mid-way: it goes idle, `stopped 1x`, and the later steps never run | | |
| 7 | Wait-until step: a sequence with "Wait until" `r2` on/off `==` 1, give up after 10 s, then stop the sequence. Start it and turn `r2` on from the Devices tab within 10 s: it carries on. Start it again and do nothing: after 10 s it stops (`sequence_stopped · timeout`). Switch the option to "carry on": after 10 s it continues with the next step instead | | |
| 8 | Guard: a do-step "Turn on `r1`" with "Only if a condition holds" `r2` on/off `==` 1, otherwise skip. With `r2` off the step is skipped and the sequence still finishes; with `r2` on it runs. Change "otherwise" to stop: with `r2` off the sequence stops with `guard` | | |
| 9 | Repeat: set Repeat to 3 on a sequence with a 2 s wait in it: it runs three times and finishes. Repeat 0 runs until you press Stop. Saving a repeating sequence with no wait step is refused with a message about repeats | | |
| 10 | Starting a running sequence does nothing (it does not restart); after it finishes Start runs it again from the top | | |
| 11 | Editor limits: the editor refuses a 17th step; the hint line shows steps used of 32 across all sequences; a second 16-step sequence is accepted and then no more steps anywhere; a 5th sequence is refused | | |
| 12 | A failing step is logged and the sequence carries on: add a step on a device you then remove from the Devices tab. The sequence finishes and Recent events shows a `failed` line naming the device | | |
| 13 | Interlock: pair `r1` and `r2` on the Automation tab. A sequence step that turns `r2` on while `r1` is on is blocked (`blocked` in the log) and the sequence goes on | | |
| 14 | Rules: new rule "A timer finishes" on `tm`, action Turn on `r1`. Start the timer: when it ends the LED comes on. Edit the rule action to "Start or stop a sequence": it starts `startup` when the timer ends | | |
| 15 | Rule trigger "A sequence finishes or stops" on `startup` event "finishes all its passes", action event `all done`: it fires once when the sequence completes normally and **not** when you Stop it. Change the event to "is stopped early": it fires on Stop, a guard stop or a timeout and **not** on a normal finish | | |
| 16 | A rule action "Start or stop a timer" starts and stops `tm` from a button press (rule: value changes on the button count). A sequence step "Start or stop a timer" also works | | |
| 17 | A sequence cannot start or stop a sequence: the step editor offers no sequence action; and a rule's action list does | | |
| 18 | Delete a timer or sequence that a rule uses: the delete asks first and warns; afterwards the rule shows `no_device` / failed counts when it runs and does not crash the board | | |
| 19 | Dashboard: Manual, add a **Timer** component on `tm`. It is at least 2 wide, shows the length, **Start** counts down once a second in step with the Automation tab, and shows Stop while running. The list in the form only offers timers. Saving a layout whose timer was deleted is refused with the component named | | |
| 20 | Persistence: with 2 timers and 2 sequences saved, restart the board: all are back, the timers are stopped, sequences idle. Pull the USB cable right after Save on a sequence: on reconnect the old or the new list is there, never empty | | |
| 21 | Engine switch off: a running sequence stops advancing and a running timer does not finish. Switch it on again: time kept passing, so a timer or wait that came due while it was off completes right after (nothing is lost, nothing is replayed twice) | | |
| 22 | Memory: with 4 sequences, 6 timers and 8 rules saved, System heap stays above 8 KB free and does not keep shrinking over 10 minutes with the Automation tab open. Saving a 16-step sequence does not fail with `out_of_memory` | | |
| 23 | `tools\api_smoke.ps1 -Ip <ip>`: every check passes | | |
| 24 | Factory reset: afterwards no sequences, timers or rules remain | | |
