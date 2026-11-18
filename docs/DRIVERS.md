# Device drivers

A device is a JSON object posted to `/api/v1/devices` (see [API.md](API.md)). Common fields: `id` (1-16 chars), `name` (up to 24), `type`, `pin`, `enabled` (default true), `failsafe_s`. Pins are GPIO numbers. The board refuses reserved pins (flash pins, UART0) and a pin used twice. Strapping pins (ESP8266 0, 2, 15; ESP32 0, 2, 5, 12, 15) are allowed but flagged `boot_sensitive`: what you wire to them can stop the board booting.

**Failsafe** (`failsafe_s`, relay/pwm/motor/servo only, 0 = off, up to 3600): the device goes to its safe state if no command and no heartbeat arrive for that long. Relay off, PWM 0, motor stopped, servo holds or moves to `safe_angle`. It latches until the next command. Open, visible dashboard pages send the heartbeat; a hidden tab or closed page does not.

**Disabled** (`enabled:false`): the device sits in its safe state, refuses commands (`409 disabled`), and an input, sensor or serial device stops reading.

## Relay
`{"id":"pump","type":"relay","pin":5,"invert":true,"initial":0}`
- `invert`: active-low modules (most relay boards). `initial`: 0 off / 1 on at boot.
- Commands: `set` true/false, `toggle`. State `{"on":bool}`.

## Input
`{"id":"door","type":"input","pin":12,"pullup":true,"invert":false,"debounce_ms":30,"long_ms":0}`
- `invert`: pressed reads low. `debounce_ms` 0-1000. `long_ms` 0 (off) or 100-10000: the hold time that counts as a long press.
- Not commandable. State `{"active":bool,"edges":n}`. `pullup` is refused on pins without one (ESP8266 GPIO16, ESP32 34 and up).

## PWM
`{"id":"led","type":"pwm","pin":4,"freq_hz":1000,"initial":0,"invert":false}`
- `freq_hz` 1-20000. `initial` is a percent at boot. At most 8 PWM-timed outputs (pwm, motor, direct servo) in total. ESP8266 GPIO16 has no PWM.
- Command: `set` 0-100. State `{"duty":n}`.

## Motor (H-bridge)
`{"id":"drive","type":"motor","pin":4,"pin2":5,"pin3":-1,"freq_hz":1000}`
- `pin` is the PWM/enable pin, `pin2` the direction pin (IN1), `pin3` an optional second direction pin (IN2) for two-input bridges (-1 = single direction pin). Always boots stopped.
- Commands: `set` -100..100 (sign = direction), `stop`.

## Servo
- Direct: `{"id":"pan","type":"servo","model":"direct","pin":13,"min_us":500,"max_us":2400,"initial":90}`
- PCA9685: `{"id":"arm","type":"servo","model":"pca9685","i2c_addr":64,"channel":0,"min_us":500,"max_us":2400}` (address 0x40-0x7F except 0x70, channel 0-15; several channels share one board address).
- `min_us` / `max_us` (300-3000, min below max) are the calibration knobs: they set the pulse at 0 and 180 degrees. Tune them per servo. `safe_angle` is 0-180 or -1 (hold).
- Command: `set` 0-180.

## Sensor
- DS18B20: `{"id":"temp","type":"sensor","model":"ds18b20","pin":14,"interval_s":5}` (1-Wire, needs a 4.7k pull-up to 3.3 V).
- BME280/BMP280: `{"id":"air","type":"sensor","model":"bme280","i2c_addr":118,"interval_s":5}` (0x76 = 118 or 0x77 = 119).
- `interval_s` 1-3600. State has `temp_c`, `humidity` and `pressure_hpa` (humidity only on a BME280) and `ok` (false after a failed read; the diagnostics table counts read errors since boot). Not commandable.

## Serial
`{"id":"link","type":"serial","pin":13,"pin2":15,"baud":9600}`: `pin` is RX, `pin2` is TX. One serial device per board. Baud 1200-115200 (ESP8266 software serial stops at 38400).
- Command: `send` with text (1-47 chars). Received lines show in the state (`text`, `count`).

## I2C
Fixed pins: ESP8266 SDA GPIO4 (D2), SCL GPIO5 (D1); ESP32 SDA 21, SCL 22. I2C devices share the bus and cannot share an address (`address_in_use`), except PCA9685 channels on one board. `POST /api/v1/i2c/scan` lists what answers.

## Adding a driver

1. `src/devices/DeviceTypes.h`: append a `DeviceType` (never reorder), its name in `deviceTypeName` / `parseDeviceType`, its fields in `DeviceConfig`, rules in `validateDevice`, command rules in `checkCommand`. Pure logic only (no Arduino includes) so the native tests cover it.
2. `src/devices/DeviceStore.cpp`: read and write the new fields (optional, additive, schema stays 1).
3. `src/devices/Drivers.cpp` and `DeviceRegistry.cpp`: start and stop the hardware (`startHw` / `stopHw`), the drive or poll function, the state in `describeState`, the safe state in `applySafe`.
4. Add native tests under `test/` and mutants in `tools/mutate_devices.py`; run `tools/gate.ps1`.
5. Add the type to the UI device form (`ui/app.js`) and the dashboard property table.

Rule: web handlers only validate and queue. Hardware and file work run in `loop()`.
