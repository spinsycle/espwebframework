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
  $("sta").textContent = !n.sta.enabled ? "off" : n.sta.connected ? n.sta.ssid + " @ " + n.sta.ip + " (" + n.sta.rssi + " dBm)" : "connecting to " + n.sta.ssid + "…";
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
