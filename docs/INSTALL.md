# Install and first run

## You need
- PlatformIO (`pip install platformio`, or the VS Code extension). Python 3 is already needed for it.
- A NodeMCU (ESP8266), an ESP32 DevKit (WROOM-32) or an ESP32-S3 DevKitC-1 and a USB cable.
- Nothing else: the dashboard is vanilla HTML/JS shipped inside the firmware's filesystem image, with no CDN.

## Make an app
An app is its own repo; the framework is a library pinned by tag.

```
my-app/
  platformio.ini
  src/main.cpp
  tools/build_ui.py      <- copy from the framework repo
  seed/                  <- optional: devices.json, automation.json, dashboard.json
```

`seed/` files ship as `/config/*.json` with `uploadfs`, so the app boots with its devices and rules already set (each needs `"schema_version": 1`; the build fails otherwise). Working examples: [`light_sequencer`](../examples/light_sequencer), [`thermostat`](../examples/thermostat), [`pan_tilt_rover`](../examples/pan_tilt_rover), [`relay_controller`](../examples/relay_controller). Seeds are not re-applied by a plain `upload`.

`platformio.ini` (start from [`examples/relay_controller`](../examples/relay_controller/platformio.ini)):

```ini
[env]
extra_scripts = pre:tools/build_ui.py
lib_deps =
    https://github.com/spinsycle/espwebframework.git#v1.0.1
    esp32async/ESPAsyncWebServer@3.12.1
    bblanchon/ArduinoJson@7.4.3
    paulstoffregen/OneWire@2.3.8
    marvinroger/AsyncMqttClient@0.9.0

[env:nodemcuv2]
platform = espressif8266@4.2.1
board = nodemcuv2
framework = arduino
board_build.filesystem = littlefs
lib_deps = ${env.lib_deps}
    esp32async/ESPAsyncTCP@2.0.0

[env:esp32dev]
platform = espressif32@6.9.0
board = esp32dev
framework = arduino
board_build.filesystem = littlefs
lib_deps = ${env.lib_deps}
    esp32async/AsyncTCP@3.5.0
```

`src/main.cpp`:

```cpp
#include <Framework.h>
FrameworkController Framework;
void setup() { Framework.setDeviceName("My Device"); Framework.begin(); }
void loop()  { Framework.loop(); }
```

PlatformIO fetches the framework from GitHub, so the build machine needs internet access and `git`. Pin the framework to a tag, never `main`. `tools/build_ui.py` gzips the dashboard from the installed library into the filesystem image; run `pio pkg install` once before the first build so the library is there.

## Flash
Both steps, once per board (this PC, in the app folder). The second one **wipes all saved settings** (devices, Wi-Fi, password, rules); a firmware-only `upload` keeps them.

```bash
python -m platformio run -e nodemcuv2 -t upload
python -m platformio run -e nodemcuv2 -t uploadfs
```

Use `-e esp32dev` for an ESP32 or `-e esp32s3` for an ESP32-S3 (the serial console is UART0, the port on the USB-UART connector; a module with 16 MB flash needs `board_upload.flash_size = 16MB` and `board_build.partitions = default_16MB.csv` in your `[env:esp32s3]`). Only one program can hold the serial port: close any serial monitor first.

## First sign-in
1. The board starts its own Wi-Fi network `ESPWF-xxxxxx` (password `espframework`). Join it and open `http://192.168.73.1`.
2. Sign in with the factory password `espframework`. The board makes you choose a new one (8 to 32 printable characters) before anything else works.
3. Network tab: enter your home Wi-Fi name and password, mode `ap_sta` (keeps the board's own network as a way back in). The board's address on your network shows in the status; its name is also `espwf-xxxxxx`.
4. Devices tab: add a device (see [DRIVERS.md](DRIVERS.md)); the dashboard builds itself from the device list.

Lost the password or locked out by a bad AP password: re-flash the filesystem (`uploadfs`), which returns the board to the factory settings.

## Update later
Sign in, System tab, Firmware update: choose the `.pio/build/<env>/firmware.bin` from your app build. The board checks the file and restarts. Settings are kept.

## Check your build
In the framework repo, `powershell -File tools/gate.ps1` builds all three targets (ESP8266, ESP32, ESP32-S3), their filesystem images and every example, and runs all native tests. With a board on your network, `tools/api_smoke.ps1 -Ip <address> -Password <pw>` exercises the whole API (about 4 minutes; wait 3 minutes between runs).
