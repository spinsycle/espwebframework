# Pan-Tilt Rover

DC motor (H-bridge) plus two servos on a PCA9685. Drive and aim from the dashboard; failsafes stop the motor after 3 s and re-centre the servos after 10 s without a dashboard heartbeat.

| Device | Pin / address | Notes |
|---|---|---|
| `drive` motor | D6 (GPIO12) enable/PWM, D7 (GPIO13) direction | single-direction-pin bridge; add `pin3` for two-input bridges |
| `pan`, `tilt` servo | PCA9685 0x40, channels 0 and 1, I2C D2 (SDA) / D1 (SCL) | tune `min_us`/`max_us` per servo |

Power motor and servos from a separate supply (common GND), never the NodeMCU 3.3 V rail. Use a wall adapter for the NodeMCU. Raise the drive slider slowly the first time.

Flash: `uploadfs` then `upload` (see light_sequencer).
