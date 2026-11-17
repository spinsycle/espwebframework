"use strict";
const $ = (id) => document.getElementById(id);
const API = "/api/v1";

async function api(path, opts) {
  const r = await fetch(API + path, opts);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw Object.assign(new Error(j.error || r.statusText), { field: j.field });
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
    if (b.dataset.tab === "dev") loadDevices();
    if (b.dataset.tab === "net") loadNet();
    if (b.dataset.tab === "sys") loadSys();
  })
);

// ---- dashboard (polling; WebSocket replaces this in P3)
function renderStatus(s) {
  $("uptime").textContent = fmtUp(s.uptime_s);
  $("heap").textContent = fmtBytes(s.heap.free) + " free";
  $("frag").textContent = "largest block " + fmtBytes(s.heap.max_block) + ", fragmentation " + s.heap.fragmentation_pct + "%";
  $("fs").textContent = fmtBytes(s.filesystem.used) + " / " + fmtBytes(s.filesystem.total);
  const n = s.network;
  $("ap").textContent = n.ap.active ? n.ap.ssid + " @ " + n.ap.ip + " (" + n.ap.clients + " clients)" : "off";
  $("sta").textContent = !n.sta.enabled ? "off" : n.sta.connected ? n.sta.ssid + " @ " + n.sta.ip + " (" + n.sta.rssi + " dBm)" : "connecting to " + n.sta.ssid + ": " + n.sta.reason + " (code " + n.sta.code + ")";
  $("reset").textContent = s.reset_reason;
}

async function poll() {
  try {
    renderStatus(await api("/status"));
    $("online").className = "dot";
    $("online").title = "online";
  } catch (e) {
    $("online").className = "dot off";
    $("online").title = "offline";
  }
  setTimeout(poll, 2000);
}

// ---- devices (polling; WebSocket replaces this in P3)
const cmd = (id, action, value) =>
  api("/command", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(value === undefined ? { id, action } : { id, action, value }) });

function el(tag, props, ...kids) {
  const e = Object.assign(document.createElement(tag), props || {});
  e.append(...kids);
  return e;
}

function devCard(d) {
  const top = el("div", { className: "top" }, el("strong", { textContent: d.name }),
    el("small", { textContent: d.type + " · GPIO" + d.pin + (d.invert ? " · inverted" : "") }));
  const body = el("div");
  if (d.type === "relay") {
    const on = d.state.on;
    const b = el("button", { type: "button", className: "tgl" + (on ? " on" : ""), textContent: on ? "ON" : "OFF" });
    b.onclick = async () => { await cmd(d.id, "toggle").catch(() => {}); setTimeout(loadDevices, 150); };
    body.append(b);
  } else if (d.type === "pwm") {
    const out = el("span", { textContent: d.state.duty + " %" });
    const r = el("input", { type: "range", min: 0, max: 100, value: d.state.duty });
    r.oninput = () => (out.textContent = r.value + " %");
    r.onchange = () => cmd(d.id, "set", +r.value).catch(() => {});
    body.append(out, r);
  } else {
    body.append(el("p", { textContent: d.state.active ? "ACTIVE" : "inactive" }), el("small", { textContent: d.state.edges + " changes" }));
  }
  const rm = el("button", { type: "button", className: "rm", textContent: "remove" });
  rm.onclick = async () => {
    if (confirm("Remove " + d.name + "?")) { await api("/devices?id=" + encodeURIComponent(d.id), { method: "DELETE" }).catch(() => {}); setTimeout(loadDevices, 250); }
  };
  const card = el("div", { className: "card dev" }, top, body, rm);
  if (d.boot_sensitive) card.append(el("small", { className: "warn", textContent: "Boot-sensitive pin: a wire here can stop the board booting." }));
  return card;
}

let devTimer = 0;
async function loadDevices() {
  clearTimeout(devTimer);
  if ($("dev").hidden) return;
  try {
    const { devices } = await api("/devices");
    $("devempty").hidden = devices.length > 0;
    if (!document.activeElement || document.activeElement.type !== "range") {  // don't fight a slider being dragged
      $("devlist").replaceChildren(...devices.map(devCard));
    }
  } catch (e) { /* offline dot already shows it */ }
  devTimer = setTimeout(loadDevices, 1500);
}

function syncDevForm() {
  const f = $("devform").elements, t = f.type.value;
  $("devform").querySelectorAll("[data-for]").forEach((d) => (d.hidden = !d.dataset.for.split(" ").includes(t)));
  $("invlabel").textContent = t === "input" ? "Pressed reads low (inverted)" : t === "pwm" ? "Inverted duty" : "Active low (relay on = pin low)";
  $("initlabel").textContent = t === "pwm" ? "(duty %)" : "(0 off, 1 on)";
}
$("devform").elements.type.addEventListener("change", syncDevForm);
syncDevForm();

$("devform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = $("devform").elements, t = f.type.value, msg = $("devmsg");
  const body = { id: f.id.value, name: f.name.value, type: t, pin: +f.pin.value, invert: f.invert.checked };
  if (t === "input") { body.pullup = f.pullup.checked; body.debounce_ms = +f.debounce_ms.value; }
  if (t === "pwm") body.freq_hz = +f.freq_hz.value;
  if (t !== "input") body.initial = +f.initial.value;
  msg.className = "";
  try {
    await api("/devices", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    msg.textContent = "Added.";
    $("devform").reset();
    syncDevForm();
    setTimeout(loadDevices, 250);
  } catch (err) {
    msg.className = "err";
    msg.textContent = "Rejected: " + err.message + (err.field ? " (" + err.field + ")" : "");
  }
});

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

api("/system").then((s) => ($("devname").textContent = s.device.name)).catch(() => {});
poll();
