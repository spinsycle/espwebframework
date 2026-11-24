// RGB LED controller for an ESP32-S3 board with the onboard WS2812 RGB LED (GPIO48 on a DevKitC-1 v1.1; GPIO38 on v1.0:
// build with -DRGB_LED_PIN=38). A page at http://<board>/rgb has an on/off button and red, green and blue sliders, 0-255.
// The framework still serves its normal dashboard, login, OTA and API at /.
//
// Routes (under /api/, so they need the framework login like everything else):
//   GET  /api/v1/app/rgb                     -> {"on":false,"r":255,"g":255,"b":255}
//   POST /api/v1/app/rgb?on=1&r=0&g=128&b=255   any subset of on (0/1), r, g, b (0-255)
// Rule from the framework: handlers only validate and store; the LED is written in loop().
#include <ESPAsyncWebServer.h>
#include <Framework.h>

#ifndef RGB_LED_PIN
#define RGB_LED_PIN 48
#endif

FrameworkController Framework;

static volatile bool gOn = false;                  // button state; the LED shows black while off
static volatile uint8_t gR = 255, gG = 255, gB = 255;  // slider values, kept while the LED is off
static volatile bool gDirty = true;

static const char kPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>RGB LED</title>
<style>
body{font:16px system-ui,sans-serif;max-width:420px;margin:2em auto;padding:0 1em;background:#111;color:#eee}
button{width:100%;padding:.8em;font-size:1.2em;border:0;border-radius:8px;background:#444;color:#fff}
button.on{background:#2a8a3a}
label{display:flex;align-items:center;gap:.6em;margin:1.2em 0}
label b{width:1.2em}input[type=range]{flex:1}output{width:2.5em;text-align:right}
#sw{height:40px;border-radius:8px;border:1px solid #555;margin-top:1em}
#msg{color:#e88;min-height:1.4em}
</style></head><body>
<h2>RGB LED</h2>
<button id="t">LED OFF</button>
<label><b style="color:#f55">R</b><input type="range" id="r" min="0" max="255" value="255"><output></output></label>
<label><b style="color:#5f5">G</b><input type="range" id="g" min="0" max="255" value="255"><output></output></label>
<label><b style="color:#59f">B</b><input type="range" id="b" min="0" max="255" value="255"><output></output></label>
<div id="sw"></div><p id="msg"></p>
<script>
const U='/api/v1/app/rgb',$=i=>document.getElementById(i),ch=['r','g','b'];
let on=false,busy=false,again=null;
function draw(){
  $('t').textContent=on?'LED ON':'LED OFF';$('t').className=on?'on':'';
  ch.forEach(c=>$(c).nextElementSibling.textContent=$(c).value);
  $('sw').style.background=on?'rgb('+ch.map(c=>$(c).value)+')':'#000';
}
async function send(q){
  if(busy){again=q;return}          // keep only the newest change while a request is in flight
  busy=true;
  try{
    const r=await fetch(U+'?'+q,{method:'POST'});
    if(r.status==401){$('msg').innerHTML='Not logged in. <a href="/">Log in</a>, then come back.';return}
    $('msg').textContent=r.ok?'':'Error '+r.status;
  }catch(e){$('msg').textContent='Board unreachable'}
  busy=false;
  if(again){const n=again;again=null;send(n)}
}
$('t').onclick=()=>{on=!on;draw();send('on='+(on?1:0))};
ch.forEach(c=>$(c).oninput=()=>{draw();send('r='+$('r').value+'&g='+$('g').value+'&b='+$('b').value)});
fetch(U).then(r=>{if(r.status==401)throw 0;return r.json()}).then(j=>{
  on=j.on;ch.forEach(c=>$(c).value=j[c]);draw();
}).catch(()=>{$('msg').innerHTML='Not logged in. <a href="/">Log in</a>, then come back.';draw()});
</script></body></html>)HTML";

// A value that is only digits and 0-255. Returns false (and leaves out alone) for anything else.
static bool parseByte(AsyncWebServerRequest* r, const char* name, int max, int& out) {
  const AsyncWebParameter* p = r->getParam(name);
  if (!p) return true;  // not sent: keep the current value
  const String& s = p->value();
  if (s.length() < 1 || s.length() > 3) return false;
  for (size_t i = 0; i < s.length(); i++)
    if (s[i] < '0' || s[i] > '9') return false;
  int v = s.toInt();
  if (v > max) return false;
  out = v;
  return true;
}

static void sendState(AsyncWebServerRequest* r) {
  char buf[64];
  snprintf(buf, sizeof buf, "{\"on\":%s,\"r\":%u,\"g\":%u,\"b\":%u}", gOn ? "true" : "false", gR, gG, gB);
  AsyncWebServerResponse* res = r->beginResponse(200, "application/json", buf);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

void setup() {
  Framework.setDeviceName("RGB LED");
  Framework.begin();  // starts the server; routes added after this are served too

  AsyncWebServer* s = Framework.server();
  // Longer path first: this server matches a route as a prefix of longer paths.
  s->on("/api/v1/app/rgb", HTTP_GET, sendState);
  s->on("/api/v1/app/rgb", HTTP_POST, [](AsyncWebServerRequest* r) {
    int on = gOn ? 1 : 0, red = gR, green = gG, blue = gB;
    if (!parseByte(r, "on", 1, on) || !parseByte(r, "r", 255, red) || !parseByte(r, "g", 255, green) || !parseByte(r, "b", 255, blue)) {
      r->send(400, "application/json", "{\"error\":\"bad_value\"}");
      return;
    }
    gOn = on != 0;
    gR = red;
    gG = green;
    gB = blue;
    gDirty = true;
    sendState(r);
  });
  s->on("/rgb", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html", kPage); });
}

void loop() {
  Framework.loop();
  if (gDirty) {
    gDirty = false;
#ifdef ARDUINO_ARCH_ESP32
    if (gOn) neopixelWrite(RGB_LED_PIN, gR, gG, gB);
    else neopixelWrite(RGB_LED_PIN, 0, 0, 0);
#endif
    // ponytail: the ESP8266 build only compiles (the gate builds every example on every chip); it has no WS2812 call here.
  }
}
