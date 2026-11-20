# Light Sequencer

Shows the automation engine: a repeating sequence, a timer, and a startup rule. No extra parts beyond one LED and a relay.

| Device | Pin (NodeMCU) | Notes |
|---|---|---|
| `lamp` relay | D7 (GPIO13) | active-low module |
| `led` PWM | D6 (GPIO12) | LED + resistor to GND |

Seed (`seed/`): sequence `show` (LED 100 -> 25 -> 0, forever) starts 3 s after boot; timer `nap` (60 s) turns the lamp off when it ends.
Try: start `nap` from the Automation tab with the lamp on; stop `show` and watch the LED hold.

Flash (run on the PC): `python -m platformio run -e nodemcuv2 -t uploadfs` then `-t upload`. `uploadfs` wipes settings and resets the password to `espframework`.
