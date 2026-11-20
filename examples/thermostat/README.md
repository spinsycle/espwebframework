# Thermostat

DS18B20 temperature sensor + relay. Heat on below 20 C, off above 22 C, each held 30 s; heater off if the sensor fails for 60 s.

| Device | Pin (NodeMCU) | Notes |
|---|---|---|
| `temp` DS18B20 | D5 (GPIO14) | data pin with 4.7k pull-up to 3.3 V |
| `heater` relay | D7 (GPIO13) | active-low module; switch a lamp, not mains heating, for the demo |

Thresholds are in `seed/automation.json` (edit, or change on the Automation tab). `failsafe_s` is 0 on the relay: a failsafe needs a dashboard page open to send heartbeats, so it would drop the heater with no browser. Software is not electrical protection (`docs/SAFETY.md`).

Flash: `uploadfs` then `upload` (see light_sequencer).
