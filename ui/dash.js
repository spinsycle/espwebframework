"use strict";
// Dashboard: an automatic layout made from the device list, or a manual layout the user edits and the board stores.
// The firmware validates and stores dashboard.json; everything drawn here is built in the browser from it.
// Uses from app.js: $, el, api, command, slider, devCard, devices, sliderInUse.

let dash = { schema_version: 1, mode: "auto", tabs: [], components: [] };
let dschema = null;
let draft = null;        // working copy while editing
let dEditing = false, previewing = false;
let curTab = 0, editId = null, joyActive = false;
let dtimers = [];       // automation timers, for the Timer component
const hist = {};         // "device:prop" -> { t, a: [recent values] } for graphs
const GRAPH_POINTS = 60;
const MAX_BODY = 6000;   // the board refuses more than 6 KB

const clone = (x) => JSON.parse(JSON.stringify(x));
const dev = (id) => devices.find((d) => d.id === id);
const typeInfo = (name) => (dschema ? dschema.types.find((t) => t.name === name) : null);

const NAMES = { toggle: "Toggle", push_button: "Push button", slider: "Slider", joystick: "Joystick", gauge: "Gauge", numeric: "Number display", status: "Status light", graph: "Graph", text: "Text display", dropdown: "Dropdown", progress: "Progress bar", device_card: "Device card", group_panel: "Group panel", timer: "Timer" };
const PROPNAMES = { state: "on / off", level: "level", temp_c: "temperature", humidity: "humidity", pressure_hpa: "pressure", text: "text", count: "count", ok: "reading ok" };
const UNITS = { temp_c: " °C", humidity: " %", pressure_hpa: " hPa" };

// ---- reading a device -----------------------------------------------------------------------------

function level(d) { return d.type === "pwm" ? [0, 100] : d.type === "motor" ? [-100, 100] : [0, 180]; }

function propVal(d, p) {
  const s = d.state;
  switch (p) {
    case "state": return d.type === "relay" ? s.on : s.active;
    case "level": return d.type === "pwm" ? s.duty : d.type === "motor" ? s.speed : s.angle;
    case "temp_c": return s.temp_c;
    case "humidity": return s.humidity;
    case "pressure_hpa": return s.pressure_hpa;
    case "text": return s.last;
    case "count": return d.type === "serial" ? s.rx_lines : s.edges;
    case "ok": return s.ok;
  }
}

function propRange(d, p) {
  if (p === "level") return level(d);
  if (p === "temp_c") return [-10, 50];
  if (p === "humidity") return [0, 100];
  if (p === "pressure_hpa") return [950, 1050];
  return [0, 100];
}

const fmt = (v, p) => (typeof v === "number" ? (Number.isInteger(v) ? v : v.toFixed(1)) + (UNITS[p] || (p === "level" ? "" : "")) : "-");

// ---- the automatic layout --------------------------------------------------------------------------

function pack(list) {  // places components left to right, top to bottom on the 4-column grid
  let col = 1, row = 1, rowH = 0;
  for (const c of list) {
    const w = c.layout.width, h = c.layout.height;
    if (col + w - 1 > 4) { row += rowH; col = 1; rowH = 0; }
    c.layout.column = col; c.layout.row = row;
    col += w; rowH = Math.max(rowH, h);
  }
  return list;
}

function mk(id, type, label, binding, w, h) {
  return { id, type, label, tab: 0, binding, layout: { column: 1, row: 1, width: w, height: h } };
}

function autoLayout() {
  const out = [];
  for (const d of devices) {
    const id = "auto_" + d.id;
    if (d.type === "relay") out.push(mk(id, "toggle", d.name, { device: d.id, property: "state" }, 1, 1));
    else if (d.type === "pwm" || d.type === "servo") out.push(mk(id, "slider", d.name, { device: d.id, property: "level" }, 2, 1));
    else if (d.type === "motor") {
      out.push(mk(id, "slider", d.name, { device: d.id, property: "level" }, 2, 1));
      out.push(mk(id + "_stop", "push_button", "Stop " + d.name, { device: d.id, action: "stop" }, 1, 1));
    } else if (d.type === "sensor") {
      out.push(mk(id + "_t", "numeric", d.name, { device: d.id, property: "temp_c" }, 1, 1));
      if (d.model === "bme280") {
        out.push(mk(id + "_h", "numeric", d.name + " humidity", { device: d.id, property: "humidity" }, 1, 1));
        out.push(mk(id + "_p", "numeric", d.name + " pressure", { device: d.id, property: "pressure_hpa" }, 1, 1));
      }
    } else if (d.type === "input") out.push(mk(id, "status", d.name, { device: d.id, property: "state" }, 1, 1));
    else out.push(mk(id, "device_card", d.name, { device: d.id }, 2, 2));
  }
  return pack(out);
}

// ---- drawing one component ------------------------------------------------------------------------

function sample(c, d, p) {  // keep a short history for graphs, one point a second at most
  const k = d.id + ":" + p, now = Date.now(), v = propVal(d, p);
  const h = hist[k] || (hist[k] = { t: 0, a: [] });
  if (typeof v === "number" && now - h.t > 1000) {
    h.t = now; h.a.push(v);
    if (h.a.length > GRAPH_POINTS) h.a.shift();
  }
  return h.a;
}

function graph(a, lo, hi) {
  const ns = "http://www.w3.org/2000/svg", svg = document.createElementNS(ns, "svg");
  svg.setAttribute("viewBox", "0 0 100 40"); svg.setAttribute("class", "graph"); svg.setAttribute("preserveAspectRatio", "none");
  if (a.length > 1) {
    let mn = Math.min(...a), mx = Math.max(...a);
    if (mx - mn < 1e-9) { mn -= 1; mx += 1; }
    const pts = a.map((v, i) => (i * 100) / (GRAPH_POINTS - 1) + "," + (38 - ((v - mn) / (mx - mn)) * 36)).join(" ");
    const pl = document.createElementNS(ns, "polyline");
    pl.setAttribute("points", pts); pl.setAttribute("fill", "none"); pl.setAttribute("stroke", "currentColor"); pl.setAttribute("stroke-width", "1.5"); pl.setAttribute("vector-effect", "non-scaling-stroke");
    svg.append(pl);
  }
  return svg;
}

function joystick(c, dx, dy) {
  const pad = el("div", { className: "joy" }), knob = el("div", { className: "knob" });
  pad.append(knob);
  const [xl, xh] = level(dx), [yl, yh] = level(dy);
  const map = (v, lo, hi) => Math.round(lo + ((v + 1) / 2) * (hi - lo));
  const mid = (lo, hi) => Math.round((lo + hi) / 2);
  let last = 0;
  const send = (x, y, force) => {
    const now = performance.now();
    if (!force && now - last < 60) return;
    last = now;
    command(dx.id, "set", map(x, xl, xh)).catch(() => {});
    command(dy.id, "set", map(y, yl, yh)).catch(() => {});
  };
  const move = (ev) => {
    const r = pad.getBoundingClientRect();
    const x = Math.max(-1, Math.min(1, ((ev.clientX - r.left) / r.width) * 2 - 1));
    const y = Math.max(-1, Math.min(1, 1 - ((ev.clientY - r.top) / r.height) * 2));
    knob.style.left = (x + 1) * 50 + "%"; knob.style.top = (1 - y) * 50 + "%";
    send(x, y, false);
  };
  pad.onpointerdown = (ev) => { joyActive = true; pad.setPointerCapture(ev.pointerId); move(ev); };
  pad.onpointermove = (ev) => { if (joyActive) move(ev); };
  const end = () => {  // springs back to centre; a motor stops
    joyActive = false;
    knob.style.left = knob.style.top = "50%";
    command(dx.id, "set", mid(xl, xh)).catch(() => {});
    command(dy.id, "set", mid(yl, yh)).catch(() => {});
  };
  pad.onpointerup = pad.onpointercancel = end;
  return pad;
}

function missing(c, what) {
  return el("div", { className: "dc bad" }, el("strong", { textContent: c.label || c.id }), el("small", { textContent: "needs " + what + " (removed?)" }));
}

function mmss(s) { return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); }

function renderTimerComp(c, box) {
  const t = dtimers.find((x) => x.id === (c.binding || {}).device);
  if (!t) return Object.assign(missing(c, "timer '" + (c.binding || {}).device + "'"), { style: box.style.cssText });
  box.append(el("h3", { textContent: c.label || t.name || t.id }));
  box.append(el("p", { className: "big", textContent: t.running ? mmss(t.remaining_s) : mmss(t.duration_s) }));
  const bar = el("div", { className: "bar" }, el("i"));
  bar.firstChild.style.width = (t.running && t.duration_s ? Math.max(0, Math.min(100, (t.remaining_s / t.duration_s) * 100)) : 0) + "%";
  const go = el("button", { type: "button", className: "tgl" + (t.running ? " on" : ""), textContent: t.running ? "Stop" : "Start" });
  go.onclick = () => api("/automation/timers/" + (t.running ? "stop" : "start") + "?id=" + t.id, { method: "POST" }).then(() => setTimeout(fetchTimers, 400)).catch(() => {});
  box.append(bar, go);
  return box;
}

async function fetchTimers() {
  try { dtimers = (await api("/automation/timers")).timers; dashRender(); } catch (e) { /* offline */ }
}

function renderComp(c) {
  const b = c.binding || {};
  if (c.type === "timer") {
    const tb = el("div", { className: "dc" });
    tb.style.gridColumn = c.layout.column + " / span " + c.layout.width;
    tb.style.gridRow = c.layout.row + " / span " + c.layout.height;
    tb.style.setProperty("--h", c.layout.height);
    return renderTimerComp(c, tb);
  }
  const d = dev(b.device);
  const title = c.label || (d && d.name) || c.id;
  const box = el("div", { className: "dc" });
  box.style.gridColumn = c.layout.column + " / span " + c.layout.width;
  box.style.gridRow = c.layout.row + " / span " + c.layout.height;
  box.style.setProperty("--h", c.layout.height);
  if (c.type === "group_panel") {
    box.append(el("h3", { textContent: title }));
    const inner = el("div", { className: "grp" });
    for (const id of b.devices || []) { const g = dev(id); inner.append(g ? devCard(g, true) : el("small", { textContent: id + " missing" })); }
    box.append(inner);
    return box;
  }
  if (!d) return Object.assign(missing(c, "device '" + b.device + "'"), { style: box.style.cssText });
  box.append(el("h3", { textContent: title }));
  const p = b.property;
  switch (c.type) {
    case "toggle": {
      const on = propVal(d, "state");
      const t = el("button", { type: "button", className: "tgl" + (on ? " on" : ""), textContent: on ? "ON" : "OFF" });
      t.onclick = () => command(d.id, "toggle").catch(() => {});
      box.append(t);
      break;
    }
    case "push_button": {
      const t = el("button", { type: "button", className: "tgl", textContent: b.action === "toggle" ? "Toggle" : b.action === "stop" ? "Stop" : b.action === "send" ? "Send" : "Set " + b.value });
      t.onclick = () => command(d.id, b.action, b.value).catch(() => {});
      box.append(t);
      break;
    }
    case "slider": { const [lo, hi] = level(d); box.append(...slider(d, lo, hi, propVal(d, "level"), d.type === "servo" ? "°" : " %")); break; }
    case "joystick": {
      const d2 = dev(b.device2);
      if (!d2) return missing(c, "device '" + b.device2 + "'");
      box.append(joystick(c, d, d2));
      break;
    }
    case "gauge": {
      const [lo, hi] = propRange(d, p), v = propVal(d, p);
      const pct = typeof v === "number" ? Math.max(0, Math.min(100, ((v - lo) / (hi - lo)) * 100)) : 0;
      const g = el("div", { className: "gauge" }, el("span", { textContent: fmt(v, p) }));
      g.style.setProperty("--p", pct);
      box.append(g);
      break;
    }
    case "numeric": box.append(el("p", { className: "big", textContent: fmt(propVal(d, p), p) })); break;
    case "progress": {
      const [lo, hi] = propRange(d, p), v = propVal(d, p);
      const bar = el("div", { className: "bar" }, el("i"));
      bar.firstChild.style.width = (typeof v === "number" ? Math.max(0, Math.min(100, ((v - lo) / (hi - lo)) * 100)) : 0) + "%";
      box.append(bar, el("small", { textContent: fmt(v, p) }));
      break;
    }
    case "graph": {
      const [lo, hi] = propRange(d, p), a = sample(c, d, p);
      box.append(graph(a, lo, hi), el("small", { textContent: fmt(propVal(d, p), p) }));
      break;
    }
    case "status": {
      const v = propVal(d, p);
      box.append(el("div", { className: "led" + (v ? " on" : "") }), el("small", { textContent: v ? (p === "ok" ? "reading ok" : "active") : p === "ok" ? "no reading" : "inactive" }));
      break;
    }
    case "text": box.append(el("p", { textContent: propVal(d, p) || "(nothing yet)" })); break;
    case "dropdown": {
      const cur = propVal(d, "level");
      const sel = el("select");
      for (const o of c.options || []) sel.append(el("option", { value: o.value, textContent: o.label, selected: o.value === cur }));
      sel.onchange = () => command(d.id, "set", +sel.value).catch(() => {});
      box.append(sel);
      break;
    }
    case "device_card": box.replaceChildren(devCard(d, true)); break;
  }
  return box;
}

// ---- the page --------------------------------------------------------------------------------------

function currentList() { return dash.mode === "auto" && !dEditing ? autoLayout() : (dEditing ? draft : dash).components; }
function currentTabs() { return (dEditing ? draft : dash).tabs; }

function dashRender() {
  if ($("dash").hidden) return;
  if (sliderInUse() || joyActive) return;
  if (document.activeElement && document.activeElement.closest("#dedit")) { /* typing in the editor form: leave it alone */ }
  const manual = dash.mode === "manual" || dEditing;
  $("dmauto").classList.toggle("on", !manual);
  $("dmmanual").classList.toggle("on", manual);
  $("dedit").hidden = !dEditing;
  $("dtoolbar").hidden = !manual;
  $("dstart").hidden = dEditing || dash.mode !== "manual";
  $("dpreview").hidden = !dEditing;
  $("dpreview").textContent = previewing ? "Back to editing" : "Preview";
  $("dsave").hidden = !dEditing;
  $("dcancel").hidden = !dEditing;
  $("dsize").hidden = !dEditing;
  if (dEditing) $("dsize").textContent = JSON.stringify(draft).length + " / " + MAX_BODY + " bytes";

  const tabs = currentTabs();
  if (curTab >= Math.max(tabs.length, 1)) curTab = 0;
  const bar = $("dtabs");
  bar.replaceChildren();
  if (tabs.length || dEditing) {
    (tabs.length ? tabs : ["Main"]).forEach((n, i) => {
      const t = el("button", { type: "button", className: i === curTab ? "on" : "", textContent: n });
      t.onclick = () => { curTab = i; dashRender(); };
      bar.append(t);
    });
    if (dEditing) bar.append(tabButtons());
  }
  bar.hidden = !bar.childNodes.length;

  const grid = $("dgrid");
  const list = currentList().filter((c) => (c.tab || 0) === curTab);
  const mode = dEditing && !previewing;
  grid.replaceChildren(...list.map((c) => (mode ? wrapEditable(c) : renderComp(c))));
  $("dempty").hidden = list.length > 0;
  $("dempty").textContent = dash.mode === "auto" && !dEditing ? "No devices yet. Add one on the Devices tab." : "Nothing on this page. Use Add component.";
  if (dEditing) fillForm();
}

function tabButtons() {
  const w = el("span", { className: "tabtools" });
  const add = el("button", { type: "button", textContent: "+ tab" });
  add.onclick = () => {
    const n = prompt("Tab name (16 characters at most)");
    if (!n) return;
    if (!draft.tabs.length) draft.tabs.push("Main");
    draft.tabs.push(n.slice(0, 16));
    curTab = draft.tabs.length - 1;
    dashRender();
  };
  const ren = el("button", { type: "button", textContent: "rename" });
  ren.onclick = () => {
    if (!draft.tabs.length) draft.tabs.push("Main");
    const n = prompt("New name", draft.tabs[curTab]);
    if (n) { draft.tabs[curTab] = n.slice(0, 16); dashRender(); }
  };
  const del = el("button", { type: "button", textContent: "delete" });
  del.onclick = () => {
    if (draft.tabs.length < 2) { alert("A dashboard needs at least one page."); return; }
    if (!confirm("Delete the tab '" + draft.tabs[curTab] + "' and everything on it?")) return;
    draft.components = draft.components.filter((c) => (c.tab || 0) !== curTab);
    draft.components.forEach((c) => { if ((c.tab || 0) > curTab) c.tab--; });
    draft.tabs.splice(curTab, 1);
    if (draft.tabs.length === 1) draft.tabs = [];  // one page needs no tab bar
    curTab = 0;
    dashRender();
  };
  w.append(add, ren, del);
  return w;
}

// ---- editing: moving, sizing, adding ----------------------------------------------------------------

function fits(c, others) {
  const L = c.layout, ti = typeInfo(c.type);
  if (L.column < 1 || L.row < 1 || L.width < 1 || L.height < 1 || L.width > 4 || L.height > 4) return false;
  if (L.column + L.width - 1 > 4 || L.row > 64) return false;
  if (ti && (L.width < ti.min_w || L.height < ti.min_h)) return false;
  return !others.some((o) => o !== c && (o.tab || 0) === (c.tab || 0) &&
    !(L.column + L.width <= o.layout.column || o.layout.column + o.layout.width <= L.column || L.row + L.height <= o.layout.row || o.layout.row + o.layout.height <= L.row));
}

function tweak(c, fn) {
  const old = clone(c.layout);
  fn(c.layout);
  if (!fits(c, draft.components)) c.layout = old;
  dashRender();
}

function wrapEditable(c) {
  const box = renderComp(c);
  const tb = el("div", { className: "etb" });
  const b = (txt, title, fn) => { const x = el("button", { type: "button", textContent: txt, title }); x.onclick = (e) => { e.stopPropagation(); fn(); }; tb.append(x); };
  b("◀", "left", () => tweak(c, (l) => l.column--));
  b("▶", "right", () => tweak(c, (l) => l.column++));
  b("▲", "up", () => tweak(c, (l) => l.row--));
  b("▼", "down", () => tweak(c, (l) => l.row++));
  b("w-", "narrower", () => tweak(c, (l) => l.width--));
  b("w+", "wider", () => tweak(c, (l) => l.width++));
  b("h-", "shorter", () => tweak(c, (l) => l.height--));
  b("h+", "taller", () => tweak(c, (l) => l.height++));
  b("edit", "edit", () => loadForm(c));
  b("✕", "delete", () => { draft.components = draft.components.filter((x) => x !== c); dashRender(); });
  box.append(tb);
  box.classList.add("editing");
  return box;
}

function firstFree(c) {
  for (let row = 1; row <= 64; row++)
    for (let col = 1; col <= 4; col++) {
      c.layout.column = col; c.layout.row = row;
      if (fits(c, draft.components)) return true;
    }
  return false;
}

function nextId() {
  let n = 1;
  while (draft.components.some((c) => c.id === "c" + n)) n++;
  return "c" + n;
}

// The add / edit form. Its menus follow the schema the firmware publishes.
function writes(ti) { return ti.need.startsWith("write"); }

function devicesFor(ti) {
  if (ti.need === "timer") return dtimers.map((t) => ({ id: t.id, name: t.name || t.id, props: [], type: "timer" }));
  return devices.filter((d) => {
    if (ti.need === "card" || ti.need === "devices") return true;
    if (ti.need === "action") return d.props.some((p) => p.write) || d.type === "serial";
    return d.props.some((p) => ti.props.includes(p.name) && (!writes(ti) || p.write));
  });
}

function propsFor(ti, d) { return d ? d.props.filter((p) => ti.props.includes(p.name) && (!writes(ti) || p.write)) : []; }

function actionsFor(d) {
  if (!d) return [];
  if (d.type === "relay") return [["toggle", "Toggle"], ["on", "Turn on"], ["off", "Turn off"]];
  if (d.type === "motor") return [["set", "Set speed"], ["stop", "Stop"]];
  if (d.type === "serial") return [["send", "Send text"]];
  return [["set", "Set value"]];
}

function fillForm() {
  const f = $("dform").elements;
  if (!dschema) return;
  if (!f.ctype.options.length) for (const t of dschema.types) f.ctype.append(el("option", { value: t.name, textContent: NAMES[t.name] || t.name }));
  const ti = typeInfo(f.ctype.value), keep = [f.cdev.value, f.cdev2.value, f.cprop.value, f.caction.value];
  const fill = (sel, items, cur) => { sel.replaceChildren(...items.map(([v, t]) => el("option", { value: v, textContent: t, selected: v === cur }))); };
  const ds = devicesFor(ti).map((d) => [d.id, d.name + " (" + d.id + ")"]);
  fill(f.cdev, ds, keep[0]);
  fill(f.cdev2, ds, keep[1]);
  fill(f.cprop, propsFor(ti, dev(f.cdev.value)).map((p) => [p.name, PROPNAMES[p.name] || p.name]), keep[2]);
  fill(f.caction, actionsFor(dev(f.cdev.value)), keep[3]);
  const show = (id, on) => ($(id).hidden = !on);
  show("cf_dev", ti.need !== "devices");
  show("cf_prop", ["write_state", "write_level", "read_number", "read_state", "read_text"].includes(ti.need));
  show("cf_dev2", ti.name === "joystick");
  show("cf_action", ti.need === "action");
  show("cf_opts", ti.name === "dropdown");
  show("cf_group", ti.need === "devices");
  const grp = $("cgroup");
  if (grp.dataset.built !== devices.map((d) => d.id).join()) {
    grp.dataset.built = devices.map((d) => d.id).join();
    grp.replaceChildren(...devices.map((d) => el("label", { className: "row" }, el("input", { type: "checkbox", value: d.id }), d.name)));
  }
  const act = f.caction.value, d = dev(f.cdev.value);
  show("cf_value", ti.need === "action" && act !== "toggle" && act !== "stop");
  f.cvalue.type = d && d.type === "serial" ? "text" : "number";
  f.cvalue.placeholder = act === "send" ? "text to send" : act === "on" || act === "off" ? "" : "number";
  show("cf_value", ti.need === "action" && act !== "toggle" && act !== "stop" && act !== "on" && act !== "off");
  $("dadd").textContent = editId ? "Save component" : "Add component";
}

function loadForm(c) {
  const f = $("dform").elements, b = c.binding || {};
  editId = c.id;
  f.ctype.value = c.type;
  fillForm();
  f.clabel.value = c.label || "";
  if (b.device) f.cdev.value = b.device;
  fillForm();
  if (b.device2) f.cdev2.value = b.device2;
  if (b.property) f.cprop.value = b.property;
  if (b.action) f.caction.value = b.action === "set" && dev(b.device) && dev(b.device).type === "relay" ? (b.value ? "on" : "off") : b.action;
  fillForm();
  if (b.value !== undefined && typeof b.value !== "boolean") f.cvalue.value = b.value;
  f.copts.value = (c.options || []).map((o) => o.label + "=" + o.value).join("\n");
  $("cgroup").querySelectorAll("input").forEach((i) => (i.checked = (b.devices || []).includes(i.value)));
  $("dform").scrollIntoView();
  fillForm();
}

function buildFromForm() {
  const f = $("dform").elements, ti = typeInfo(f.ctype.value), t = ti.name;
  const c = { id: editId || nextId(), type: t, label: f.clabel.value.slice(0, 24), tab: curTab, layout: { column: 1, row: 1, width: 1, height: 1 }, binding: {} };
  const old = editId && draft.components.find((x) => x.id === editId);
  if (old) { c.layout = clone(old.layout); c.tab = old.tab || 0; }
  else { if (t === "slider" || t === "graph" || t === "dropdown") c.layout.width = 2; if (t === "joystick" || t === "group_panel") { c.layout.width = 2; c.layout.height = 2; } if (t === "device_card") { c.layout.width = 2; c.layout.height = 2; } if (t === "timer") c.layout.width = 2; }
  if (ti.need === "devices") {
    c.binding.devices = [...$("cgroup").querySelectorAll("input:checked")].map((i) => i.value);
    if (!c.binding.devices.length) return "Pick at least one device.";
  } else {
    if (!f.cdev.value) return ti.need === "timer" ? "Pick a timer (create one on the Automation tab first)." : "Pick a device.";
    c.binding.device = f.cdev.value;
  }
  if (["write_state", "write_level", "read_number", "read_state", "read_text"].includes(ti.need)) {
    if (!f.cprop.value) return "That device has nothing this component can show.";
    c.binding.property = f.cprop.value;
  }
  if (t === "joystick") { if (!f.cdev2.value) return "Pick the second device."; c.binding.device2 = f.cdev2.value; }
  if (ti.need === "action") {
    const a = f.caction.value, d = dev(f.cdev.value);
    if (a === "on" || a === "off") { c.binding.action = "set"; c.binding.value = a === "on"; }
    else if (a === "toggle" || a === "stop") c.binding.action = a;
    else {
      c.binding.action = a;
      if (a === "send") c.binding.value = f.cvalue.value;
      else if (f.cvalue.value === "") return "Enter a value.";
      else c.binding.value = +f.cvalue.value;
    }
  }
  if (t === "dropdown") {
    const rows = f.copts.value.split("\n").map((s) => s.trim()).filter(Boolean);
    c.options = [];
    for (const r of rows) {
      const i = r.lastIndexOf("=");
      if (i < 1 || r.slice(i + 1).trim() === "" || isNaN(+r.slice(i + 1))) return "Options are lines like  Low=25";
      c.options.push({ label: r.slice(0, i).trim().slice(0, 16), value: +r.slice(i + 1) });
    }
    if (!c.options.length) return "Add at least one option.";
  }
  if (!old && !firstFree(c)) return "No free space on this page.";
  if (old && !fits(c, draft.components.filter((x) => x !== old))) return "It does not fit.";
  if (old) draft.components[draft.components.indexOf(old)] = c; else draft.components.push(c);
  return "";
}

$("dform").addEventListener("submit", (e) => {
  e.preventDefault();
  const err = buildFromForm();
  $("dformmsg").textContent = err;
  $("dformmsg").className = err ? "err" : "";
  if (!err) { editId = null; $("dform").reset(); }
  dashRender();
});
["ctype", "cdev", "caction"].forEach((n) => $("dform").elements[n].addEventListener("change", fillForm));

// ---- mode, save, cancel -----------------------------------------------------------------------------

function describeError(e) {
  const b = e.body || {};
  if (b.reason) return "Rejected" + (b.component >= 0 ? " at component " + (b.component + 1) + (b.id ? " '" + b.id + "'" : "") : "") + ": " + b.reason + (b.field ? " (" + b.field + ")" : "");
  return "Failed: " + e.message;
}

async function saveDash(doc) {
  await api("/dashboard", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(doc) });
  dash = doc;
}

function startEditing() {
  draft = clone(dash);
  draft.mode = "manual";
  fetchTimers();
  dEditing = true; previewing = false; editId = null;
  dashRender();
}

$("dmmanual").onclick = async () => {
  if (dash.mode === "manual") { if (!dEditing) dashRender(); return; }
  if (!dash.components.length) {  // nothing yet: start from the automatic layout
    draft = { schema_version: 1, mode: "manual", tabs: [], components: autoLayout().map((c, i) => Object.assign(c, { id: "c" + (i + 1) })) };
    dEditing = true; previewing = false; editId = null; curTab = 0;
    fetchTimers();
    dashRender();
    return;
  }
  const doc = Object.assign(clone(dash), { mode: "manual" });
  try { await saveDash(doc); } catch (e) { $("dmsg").className = "err"; $("dmsg").textContent = describeError(e) + ". Edit the layout to fix it."; draft = doc; dEditing = true; }
  dashRender();
};

$("dmauto").onclick = async () => {
  if (dEditing && !confirm("Discard your unsaved changes?")) return;
  dEditing = false;
  if (dash.mode === "auto") { dashRender(); return; }
  try { await saveDash(Object.assign(clone(dash), { mode: "auto" })); $("dmsg").textContent = ""; } catch (e) { $("dmsg").className = "err"; $("dmsg").textContent = describeError(e); }
  dashRender();
};

$("dstart").onclick = startEditing;
$("dpreview").onclick = () => { previewing = !previewing; dashRender(); };
$("dcancel").onclick = () => { dEditing = false; previewing = false; editId = null; $("dmsg").textContent = ""; dashRender(); };
$("dsave").onclick = async () => {
  const msg = $("dmsg");
  msg.className = "";
  if (JSON.stringify(draft).length > MAX_BODY) { msg.className = "err"; msg.textContent = "Too big for the board (" + MAX_BODY + " bytes). Remove some components."; return; }
  try {
    await saveDash(clone(draft));
    dEditing = false; previewing = false; editId = null;
    msg.textContent = "Saved.";
  } catch (e) { msg.className = "err"; msg.textContent = describeError(e); }
  dashRender();
};

async function dashShow() {
  try {
    dschema = dschema || (await api("/dashboard/schema"));
    const d = await api("/dashboard");
    d.tabs = d.tabs || []; d.components = d.components || [];
    if (!dEditing) dash = d;
    if ((dEditing ? draft : d).components.some((c) => c.type === "timer") || dEditing) await fetchTimers();
  } catch (e) { /* the offline dot shows it */ }
  if (!wsLive) await fetchDevices();
  dashRender();
}
// Timers are read from the board once a second while a Timer component is on screen.
setInterval(() => {
  if ($("dash").hidden) return;
  const list = dEditing ? draft.components : dash.components;
  if (list.some((c) => c.type === "timer")) fetchTimers();
}, 1000);

// Without a WebSocket the device list is polled (the Devices tab does the same); with one, events keep it current.
setInterval(() => { if (!$("dash").hidden && !wsLive) fetchDevices(); }, 1500);
dashShow();
