# RGB LED

For an ESP32-S3 board with the onboard WS2812 RGB LED (GPIO48 on a DevKitC-1 v1.1; on v1.0 it is GPIO38, build with `-DRGB_LED_PIN=38`). No wiring.

Open `http://<board address>/rgb` (log in at `/` first). An on/off button and red, green and blue sliders, each 0 (off) to 255 (full). The sliders keep their values while the LED is off.

API (behind the framework login): `GET /api/v1/app/rgb` returns `{"on":false,"r":255,"g":255,"b":255}`; `POST /api/v1/app/rgb?on=1&r=0&g=128&b=255` sets any subset. The app adds these routes itself through `Framework.server()`; the framework dashboard is unchanged.

Flash (from this folder): `python -m platformio run -e esp32s3 -t upload --upload-port COM4`. The filesystem upload is only needed once on a fresh board: `-t uploadfs`. Other chips compile this example but only the ESP32 family drives the LED.
