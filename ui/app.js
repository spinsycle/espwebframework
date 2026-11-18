"use strict";
const $ = (id) => document.getElementById(id);
const API = "/api/v1";

async function api(path, opts) {
  const r = await fetch(API + path, opts);
  const j = await r.json().catch(() => ({}));
  if (r.status === 401) authFail();                                   // the session ended: back to the login form
  else if (r.status === 403 && j.error === "password_change_required") showGate("change", true);
  if (!r.ok) throw Object.assign(new Error(j.error || r.statusText), { field: j.field, body: j });
  return j;
}

const fmtBytes = (n) => (n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : (n / 1024).toFixed(1) + " KB");
function fmtUp(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return (d ? d + "d " : "") + h + "h " + m + "m " + (s % 60) + "s";
}

// ---- tabs
document.querySelectorAll("#tabs button").forEach((b) =>
  b.addEventListener("click", () => {
    document.querySelectorAll("#tabs button").forEach((x) => x.classList.toggle("on", x === b));
    document.querySelectorAll("main > section").forEach((s) => (s.hidden = s.id !== b.dataset.tab));
    if (b.dataset.tab === "dash") dashShow();
    if (b.dataset.tab === "automation") autoShow();
    if (b.dataset.tab === "dev") showDevices();
    if (b.dataset.tab === "net") loadNet();
    if (b.dataset.tab === "sys") loadSys();
    if (b.dataset.tab === "diag") diagShow();
  })
);

// ---- dashboard status
function renderStatus(s) {
  $("uptime").textContent = fmtUp(s.uptime_s);
  $("heap").textContent = fmtBytes(s.heap.free) + " free";
  $("frag").textContent = "largest block " + fmtBytes(s.heap.max_block) + ", fragmentation " + s.heap.fragmentation_pct + "%";
  $("fs").textContent = fmtBytes(s.filesystem.used) + " / " + fmtBytes(s.filesystem.total);
  const n = s.network;
  $("ap").textContent = n.ap.active ? n.ap.ssid + " @ " + n.ap.ip + " (" + n.ap.clients + " clients)" : "off";
  $("sta").textContent = !n.sta.enabled ? "off" : n.sta.connected ? n.sta.ssid + " @ " + n.sta.ip + " (" + n.sta.rssi + " dBm)" : "connecting to " + n.sta.ssid + ": " + n.sta.reason + " (code " + n.sta.code + ")";
  $("reset").textContent = s.reset_reason;
  $("wsc").textContent = s.ws_clients + " open";
}

const setOnline = (on) => {
  $("online").className = "dot" + (on ? "" : " off");
  $("online").title = on ? "online" : "offline";
};

// ---- live connection: a WebSocket pushes status and device state; polling is only the fallback
let ws = null, wsLive = false, wsRetryMs = 1000, wsLastRx = 0, wsBuf = null;

function wsSend(obj) {
  if (!ws || ws.readyState !== 1) return false;
  ws.send(JSON.stringify(obj));
  return true;
}

function connectWs() {
  const sock = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
  ws = sock;
  sock.onmessage = (ev) => {
    wsLastRx = Date.now();
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.event === "hello") {
      wsLive = true;
      wsRetryMs = 1000;
      setOnline(true);
      wsBuf = [];  // events that arrive while the list is being fetched wait their turn
      fetchDevices().finally(() => { const b = wsBuf || []; wsBuf = null; b.forEach(handleWs); });
    } else if (wsBuf) wsBuf.push(m);
    else handleWs(m);
  };
  sock.onclose = () => {
    if (ws === sock) { ws = null; wsLive = false; wsBuf = null; }
    if (!authed) return;                                              // no session: the login form is up, do not keep knocking
    setTimeout(connectWs, wsRetryMs);
    wsRetryMs = Math.min(wsRetryMs * 2, 5000);  // back off, but never wait long: the board may be restarting
  };
  sock.onerror = () => sock.close();
}

function handleWs(m) {
  if (m.event === "status") renderStatus(m);
  else if (m.event === "devices_changed") fetchDevices();
  else if (m.event === "device_state") {
    const d = devices.find((x) => x.id === m.id);
    if (d) { d.state = m.state; renderDevices(); dashRender(); }
  } else if (m.event === "error") {
    const msg = $("devmsg");
    msg.className = "err";
    msg.textContent = "Rejected: " + m.error + (m.field ? " (" + m.field + ")" : "");
    setTimeout(() => { if (msg.className === "err") msg.textContent = ""; }, 5000);
  }
}

// The controller's heartbeat: while a page is open and visible, every failsafe timer is kept from running out.
// A hidden tab or a dead connection stops it, which is what lets a robot stop when its controller goes away.
setInterval(() => {
  if (!authed || document.hidden) return;
  if (!wsSend({ type: "heartbeat" })) api("/heartbeat", { method: "POST" }).catch(() => {});
}, 1000);

setInterval(() => {  // keep-alive: a half-open socket (Wi-Fi dropped) stays "open" in the browser until we notice
  if (!wsLive) return;
  wsSend({ type: "ping" });
  if (Date.now() - wsLastRx > 25000 && ws) ws.close();
}, 10000);

async function poll() {
  if (!wsLive) {
    try { renderStatus(await api("/status")); setOnline(true); } catch (e) { setOnline(false); }
  }
  setTimeout(poll, 2000);
}

// ---- devices
let devices = [];

function el(tag, props, ...kids) {
  const e = Object.assign(document.createElement(tag), props || {});
  e.append(...kids);
  return e;
}

function command(id, action, value) {
  const m = { type: "command", id, action };
  if (value !== undefined) m.value = value;
  if (wsSend(m)) return Promise.resolve();  // the answer arrives as a device_state event
  return api("/command", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ id, action, value }) })
    .then(() => setTimeout(fetchDevices, 200));
}

function slider(d, min, max, value, unit) {
  const out = el("span", { textContent: value + unit });
  const r = el("input", { type: "range", min, max, value });
  let last = 0;
  r.oninput = () => {
    out.textContent = r.value + unit;
    const now = performance.now();
    if (now - last > 60) { last = now; command(d.id, "set", +r.value).catch(() => {}); }  // live while dragging, ~16/s
  };
  r.onchange = () => { command(d.id, "set", +r.value).catch(() => {}); r.blur(); };  // final value; blur lets updates redraw
  return [out, r];
}

function where(d) {
  if (d.type === "servo" && d.model === "pca9685") return "pca9685 0x" + d.i2c_addr.toString(16) + " ch" + d.channel;
  if (d.type === "sensor" && d.model === "bme280") return "bme280 0x" + d.i2c_addr.toString(16);
  if (d.type === "motor") return "GPIO" + d.pin + "/" + d.pin2 + (d.pin3 !== undefined ? "/" + d.pin3 : "") + (d.invert ? " · inverted" : "");
  if (d.type === "serial") return "RX GPIO" + d.pin + " TX GPIO" + d.pin2 + " · " + d.baud;
  return "GPIO" + d.pin + (d.invert ? " · inverted" : "");
}

function devCard(d, bare) {
  const top = el("div", { className: "top" }, el("strong", { textContent: d.name }),
    el("small", { textContent: d.type + " · " + where(d) }));
  const body = el("div");
  if (d.type === "relay") {
    const on = d.state.on;
    const b = el("button", { type: "button", className: "tgl" + (on ? " on" : ""), textContent: on ? "ON" : "OFF" });
    b.onclick = () => command(d.id, "toggle").catch(() => {});
    body.append(b);
  } else if (d.type === "pwm") {
    body.append(...slider(d, 0, 100, d.state.duty, " %"));
  } else if (d.type === "motor") {
    const stop = el("button", { type: "button", className: "tgl", textContent: "STOP" });
    stop.onclick = () => command(d.id, "stop").catch(() => {});
    body.append(...slider(d, -100, 100, d.state.speed, " %"), stop);
  } else if (d.type === "servo") {
    body.append(...slider(d, 0, 180, d.state.angle, "°"));
  } else if (d.type === "sensor") {
    const s = d.state;
    if (!s.ok) body.append(el("p", { textContent: "no reading" }));
    else {
      body.append(el("p", { textContent: s.temp_c.toFixed(1) + " °C" }));
      if (s.humidity !== undefined) body.append(el("small", { textContent: s.humidity.toFixed(1) + " % RH · " }));
      if (s.pressure_hpa !== undefined) body.append(el("small", { textContent: s.pressure_hpa.toFixed(1) + " hPa" }));
    }
  } else if (d.type === "serial") {
    const txt = el("input", { type: "text", maxLength: 47, placeholder: "line to send" });
    const send = el("button", { type: "button", className: "tgl", textContent: "Send" });
    const go = () => { if (txt.value) command(d.id, "send", txt.value).then(() => (txt.value = "")).catch(() => {}); };
    send.onclick = go;
    txt.onkeydown = (ev) => { if (ev.key === "Enter") go(); };
    body.append(el("p", { textContent: d.state.last || "(nothing received)" }), el("small", { textContent: d.state.rx_lines + " lines received" }), txt, send);
  } else {
    body.append(el("p", { textContent: d.state.active ? "ACTIVE" : "inactive" }),
      el("small", { textContent: d.state.edges + " changes" + (d.long_ms ? " · " + d.state.long_presses + " long presses" : "") }));
  }
  const rm = el("button", { type: "button", className: "rm", textContent: "remove" });
  rm.onclick = async () => {
    if (!confirm("Remove " + d.name + "?")) return;
    await api("/devices?id=" + encodeURIComponent(d.id), { method: "DELETE" }).catch(() => {});
    if (!wsLive) setTimeout(fetchDevices, 250);
  };
  const ed = el("button", { type: "button", className: "rm", textContent: "edit" });
  ed.onclick = () => startEdit(d);
  const off = d.enabled === false;
  if (off) body.querySelectorAll("button,input").forEach((x) => (x.disabled = true));
  const card = el("div", { className: "card dev" + (off ? " off" : "") }, top, body);
  if (off) card.append(el("small", { className: "warn", textContent: "Disabled: in its safe state, commands are refused." }));
  else if (d.state && d.state.failsafe_tripped) card.append(el("small", { className: "warn", textContent: "Failsafe tripped: it is in its safe state. Send a command to resume." }));
  const en = el("button", { type: "button", className: "rm", textContent: off ? "enable" : "disable" });
  en.onclick = () => api("/devices", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ id: d.id, enabled: off }) })
    .then(() => { if (!wsLive) setTimeout(fetchDevices, 400); }).catch(() => {});
  if (!bare) card.append(en, ed, rm);
  if (d.boot_sensitive) card.append(el("small", { className: "warn", textContent: "Boot-sensitive pin: a wire here can stop the board booting." }));
  return card;
}

const sliderInUse = () => document.activeElement && (document.activeElement.type === "range" || document.activeElement.type === "text");

function renderDevices() {
  if ($("dev").hidden) return;
  $("devempty").hidden = devices.length > 0;
  if (sliderInUse()) return;  // don't redraw under a slider being dragged
  $("devlist").replaceChildren(...devices.map(devCard));
}

async function fetchDevices() {
  try { devices = (await api("/devices")).devices; renderDevices(); dashRender(); } catch (e) { /* the offline dot shows it */ }
}

let devTimer = 0;
function showDevices() {
  clearTimeout(devTimer);
  if ($("dev").hidden) return;
  if (wsLive) { renderDevices(); return; }  // already live from the socket
  fetchDevices().finally(() => (devTimer = setTimeout(showDevices, 1500)));  // fallback: poll
}


const PINLABEL = { motor: "Speed (PWM / enable) pin", serial: "RX pin", servo: "Signal pin", sensor: "1-Wire data pin" };

function syncDevForm() {
  const f = $("devform").elements, t = f.type.value;
  const pca = t === "servo" && f.smodel.value === "pca9685", bme = t === "sensor" && f.xmodel.value === "bme280";
  const keys = [t, pca ? "pca" : "", bme ? "bme" : ""];
  $("devform").querySelectorAll("[data-for]").forEach((d) => (d.hidden = !d.dataset.for.split(" ").some((k) => keys.includes(k))));
  $("pinbox").hidden = $("pinbox").hidden || pca || bme;  // those two sit on the I2C bus, not a pin
  $("pinlabel").textContent = PINLABEL[t] || "GPIO pin";
  $("pin2label").textContent = t === "serial" ? "TX pin" : "Direction pin (IN1)";
  $("invlabel").textContent = t === "input" ? "Pressed reads low (inverted)" : t === "pwm" ? "Inverted duty" : t === "motor" ? "Reverse direction" : "Active low (relay on = pin low)";
  $("initlabel").textContent = t === "pwm" ? "(duty %)" : t === "servo" ? "(angle, degrees)" : "(0 off, 1 on)";
  f.initial.value = t === "servo" ? 90 : 0;
}
$("devform").elements.type.addEventListener("change", syncDevForm);
$("devform").elements.smodel.addEventListener("change", syncDevForm);
$("devform").elements.xmodel.addEventListener("change", syncDevForm);
syncDevForm();

let editing = null;  // id of the device being edited, or null when the form adds a new one

function setEditMode(id) {
  editing = id;
  const f = $("devform").elements;
  f.id.readOnly = f.type.disabled = !!id;
  $("devtitle").textContent = id ? "Edit " + id : "Add device";
  $("devsubmit").textContent = id ? "Save changes" : "Add device";
  $("devcancel").hidden = !id;
}

function startEdit(d) {
  const f = $("devform").elements, num = (v) => (v === undefined || v < 0 ? "" : v);
  $("devform").reset();
  setEditMode(d.id);
  f.type.value = d.type;
  f.smodel.value = d.type === "servo" ? d.model : "direct";
  f.xmodel.value = d.type === "sensor" ? d.model : "ds18b20";
  syncDevForm();
  f.id.value = d.id; f.name.value = d.name; f.invert.checked = !!d.invert;
  f.pin.value = num(d.pin); f.pin2.value = num(d.pin2); f.pin3.value = num(d.pin3);
  if (d.pullup !== undefined) f.pullup.checked = d.pullup;
  if (d.debounce_ms !== undefined) f.debounce_ms.value = d.debounce_ms;
  if (d.long_ms !== undefined) f.long_ms.value = d.long_ms;
  if (d.freq_hz !== undefined) f.freq_hz.value = d.freq_hz;
  if (d.initial !== undefined) f.initial.value = d.initial;
  if (d.min_us !== undefined) { f.min_us.value = d.min_us; f.max_us.value = d.max_us; }
  if (d.channel !== undefined) f.channel.value = d.channel;
  if (d.interval_s !== undefined) f.interval_s.value = d.interval_s;
  f.enabled.checked = d.enabled !== false;
  f.failsafe_s.value = d.failsafe_s || 0;
  f.safe_angle.value = d.safe_angle === undefined ? "" : d.safe_angle;
  if (d.baud !== undefined) f.baud.value = d.baud;
  if (d.i2c_addr !== undefined) { f.saddr.value = "0x" + d.i2c_addr.toString(16); f.xaddr.value = d.i2c_addr; }
  $("devmsg").textContent = "";
  $("devform").scrollIntoView();
}
$("devcancel").onclick = () => { $("devform").reset(); setEditMode(null); syncDevForm(); $("devmsg").textContent = ""; };

$("devform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = $("devform").elements, t = f.type.value, msg = $("devmsg");
  const body = { id: f.id.value, name: f.name.value, type: t, invert: f.invert.checked };
  const num = (x) => (x.value === "" ? -1 : +x.value);
  const onBus = (t === "servo" && f.smodel.value === "pca9685") || (t === "sensor" && f.xmodel.value === "bme280");
  body.pin = onBus ? -1 : num(f.pin);
  if (t === "input") { body.pullup = f.pullup.checked; body.debounce_ms = +f.debounce_ms.value; body.long_ms = +f.long_ms.value; }
  if (t === "pwm" || t === "motor") body.freq_hz = +f.freq_hz.value;
  if (t === "relay" || t === "pwm" || t === "servo") body.initial = +f.initial.value;
  if (t === "motor") { body.pin2 = num(f.pin2); if (f.pin3.value !== "") body.pin3 = +f.pin3.value; }
  if (t === "serial") { body.pin2 = num(f.pin2); body.baud = +f.baud.value; }
  if (t === "servo") {
    body.model = f.smodel.value; body.min_us = +f.min_us.value; body.max_us = +f.max_us.value;
    if (onBus) { body.i2c_addr = Number(f.saddr.value); body.channel = +f.channel.value; }
  }
  if (t === "sensor") {
    body.model = f.xmodel.value; body.interval_s = +f.interval_s.value;
    if (onBus) body.i2c_addr = +f.xaddr.value;
  }
  body.enabled = f.enabled.checked;
  if (t === "relay" || t === "pwm" || t === "motor" || t === "servo") body.failsafe_s = +f.failsafe_s.value;
  if (t === "servo") body.safe_angle = f.safe_angle.value === "" ? -1 : +f.safe_angle.value;
  msg.className = "";
  try {
    await api("/devices", { method: editing ? "PUT" : "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    msg.textContent = editing ? "Saved." : "Added.";
    $("devform").reset();
    setEditMode(null);
    syncDevForm();
    if (!wsLive) setTimeout(fetchDevices, 250);
  } catch (err) {
    msg.className = "err";
    msg.textContent = "Rejected: " + err.message + (err.field ? " (" + err.field + ")" : "");
  }
});

// ---- config: import, factory reset
$("cfgimport").onclick = async () => {
  const file = $("cfgfile").files[0], msg = $("cfgmsg");
  msg.className = "";
  if (!file) { msg.textContent = "Choose a file first."; return; }
  if (!confirm("Replace ALL devices with the ones in " + file.name + "? The board restarts.")) return;
  try {
    const r = await api("/config/import", { method: "POST", headers: { "Content-Type": "application/json" }, body: await file.text() });
    msg.textContent = "Imported " + r.devices + " device(s). Restarting...";
  } catch (err) {
    msg.className = "err";
    const b = err.body || {};
    msg.textContent = b.reason ? "Rejected at device " + (b.index + 1) + (b.id ? " '" + b.id + "'" : "") + ": " + b.reason + (b.field ? " (" + b.field + ")" : "") + ". Nothing was changed."
      : "Rejected: " + err.message + (err.field ? " (" + err.field + ")" : "");
  }
};

$("cfgreset").onclick = async () => {
  if (!confirm("Delete ALL saved settings (devices and Wi-Fi) and restart? The board returns to its own access point with the default password.")) return;
  try {
    await api("/config/factory-reset", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ confirm: "factory-reset" }) });
    $("cfgmsg").textContent = "Reset. The board is restarting in its own access point.";
  } catch (err) { $("cfgmsg").className = "err"; $("cfgmsg").textContent = "Failed: " + err.message; }
};

// ---- I2C scan
async function showScan() {
  const res = $("i2cres");
  try {
    const s = await api("/i2c");
    $("i2cpins").textContent = "SDA GPIO" + s.sda + " · SCL GPIO" + s.scl;
    if (s.scanning) { res.textContent = "Scanning..."; setTimeout(showScan, 400); return; }
    if (!s.done) return;
    res.textContent = s.found.length ? s.found.map((x) => "0x" + x.addr.toString(16).padStart(2, "0") + (x.name ? " " + x.name : "")).join("\n") : "Nothing answered. Check wiring and pull-ups.";
    res.style.whiteSpace = "pre-line";
  } catch (e) { res.textContent = "Scan failed: " + e.message; }
}
$("i2cscan").onclick = async () => {
  try {
    await api("/i2c/scan", { method: "POST" });
    $("i2cres").textContent = "Scanning...";
    setTimeout(showScan, 400);
  } catch (e) {
    $("i2cres").textContent = e.message === "pin_in_use" ? "A device is using the I2C pins. Remove it first." : "Scan refused: " + e.message;
  }
};

// ---- network
const F = () => $("netform").elements;
function syncStatic() { $("staticbox").hidden = !F().sta_static.checked; }

async function loadNet() {
  const c = await api("/network");
  const f = F();
  f.mode.value = c.mode;
  f.hostname.value = c.hostname;
  f.ap_ssid.value = c.ap.ssid;
  f.ap_channel.value = c.ap.channel;
  f.ap_ip.value = c.ap.ip;
  f.sta_ssid.value = c.sta.ssid;
  f.sta_static.checked = c.sta.static;
  f.sta_ip.value = c.sta.ip;
  f.sta_gateway.value = c.sta.gateway;
  f.sta_mask.value = c.sta.mask;
  f.ap_password.value = f.sta_password.value = "";
  syncStatic();
}

// Show/hide button on every password box, so a typo is visible before saving.
$("netform").querySelectorAll("input[type=password]").forEach((inp) => {
  const b = el("button", { type: "button", className: "showbtn", textContent: "Show" });
  b.onclick = () => {
    const hide = inp.type === "text";
    inp.type = hide ? "password" : "text";
    b.textContent = hide ? "Show" : "Hide";
  };
  inp.parentElement.append(b);
});

$("netform").elements.sta_static.addEventListener("change", syncStatic);
$("netform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = F(), msg = $("netmsg");
  const ap = { ssid: f.ap_ssid.value, channel: +f.ap_channel.value, ip: f.ap_ip.value };
  const sta = { ssid: f.sta_ssid.value, static: f.sta_static.checked, ip: f.sta_ip.value, gateway: f.sta_gateway.value, mask: f.sta_mask.value };
  if (f.ap_password.value) ap.password = f.ap_password.value;     // blank = keep current
  if (f.sta_password.value) sta.password = f.sta_password.value;
  msg.className = "";
  try {
    await api("/network", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ mode: f.mode.value, hostname: f.hostname.value, ap, sta }) });
    msg.textContent = "Applied. The device is switching networks; reconnect to it and reopen this page.";
  } catch (err) {
    msg.className = "err";
    msg.textContent = "Rejected: " + err.message + (err.field ? " (" + err.field + ")" : "");
  }
});

// ---- system
async function loadSys() {
  const s = await api("/system");
  const rows = [["Device", s.device.name], ["Device ID", s.device.id], ["Chip", s.chip + " @ " + s.cpu_mhz + " MHz"],
    ["Flash", fmtBytes(s.flash_size)], ["Framework", s.framework_version], ["SDK", s.sdk], ["Built", s.build]];
  $("sysinfo").innerHTML = "";
  rows.forEach(([k, v]) => {
    const dt = document.createElement("dt"), dd = document.createElement("dd");
    dt.textContent = k; dd.textContent = v;
    $("sysinfo").append(dt, dd);
  });
  $("devname").textContent = s.device.name;
}

$("restart").addEventListener("click", async () => {
  if (confirm("Restart the device?")) await api("/system/restart", { method: "POST" }).catch(() => {});
});

authBoot(() => {
  api("/system").then((s) => ($("devname").textContent = s.device.name)).catch(() => {});
  connectWs();
  poll();
});
