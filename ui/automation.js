"use strict";
// Automation tab: the rule list with live status, a rule editor, the clock and interlock settings, and the event log.
// The firmware validates every change; this page only builds the JSON. Uses from app.js: $, el, api, devices, fetchDevices.

let acfg = { enabled: true, utc_offset_min: 0, interlocks: [], rules: [] };
let astat = null, asch = null;
let adraft = null, aisNew = false, atimer = 0;

const WEEK = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const PN = { state: "on/off", level: "level", temp_c: "temperature", humidity: "humidity", pressure_hpa: "pressure", count: "count", ok: "reading ok" };
const TRIG = { change: "A value changes", when: "Conditions become true", interval: "Every so often", schedule: "At a time of day", startup: "When the board starts", wifi: "Wi-Fi connects or drops", manual: "Run by hand only" };
const ACT = { on: "Turn on", off: "Turn off", toggle: "Toggle", set: "Set value", stop: "Stop", send: "Send text", event: "Send an event" };
const jsonBody = (m, b) => ({ method: m, headers: { "Content-Type": "application/json" }, body: JSON.stringify(b) });
const adev = (id) => devices.find((d) => d.id === id);
const dname = (id) => (adev(id) ? adev(id).name : id + " (missing)");
const numProps = (d) => (d ? d.props.map((p) => p.name).filter((n) => n !== "text") : []);

// ---- one-line descriptions ----------------------------------------------------------------------

const days = (a) => (!a || a.length === 7 ? "every day" : a.map((d) => WEEK[d]).join(" "));

function describeTrigger(t) {
  switch (t.type) {
    case "change": return "when " + dname(t.device) + " " + (PN[t.property] || t.property) + " changes";
    case "when": return "when the conditions become true";
    case "interval": return "every " + t.every_s + " s";
    case "schedule": return "at " + t.time + " " + days(t.days);
    case "startup": return "when the board starts";
    case "wifi": return "when Wi-Fi " + t.event;
    default: return "only when run by hand";
  }
}

function describeCond(c) {
  if (c.type === "time") return "between " + c.from + " and " + c.to + " (" + days(c.days) + ")";
  return dname(c.device) + " " + (PN[c.property] || c.property) + " " + c.operator + " " + c.value + (c.for_s ? " for " + c.for_s + " s" : "");
}

function describeAction(a) {
  if (a.type === "event") return "send the event '" + a.text + "'";
  if (a.type === "set") return "set " + dname(a.device) + " to " + a.value;
  if (a.type === "send") return "send '" + a.text + "' to " + dname(a.device);
  return (ACT[a.type] || a.type).toLowerCase() + " " + dname(a.device);
}

function ago(s) { return s < 90 ? s + " s ago" : s < 5400 ? Math.round(s / 60) + " min ago" : Math.round(s / 3600) + " h ago"; }

// ---- loading and the page --------------------------------------------------------------------------

async function autoLoad() {
  try {
    asch = asch || (await api("/automation/schema"));
    acfg = await api("/automation");
    await autoStatus();
  } catch (e) { $("amsg").className = "err"; $("amsg").textContent = "Could not load: " + e.message; }
  autoRender();
}

async function autoStatus() {
  try { astat = await api("/automation/status"); } catch (e) { return; }
  autoRenderStatus();
}

function autoShow() {
  clearTimeout(atimer);
  if ($("automation").hidden) return;
  autoLoad().finally(() => (atimer = setTimeout(autoTick, 2000)));
}
function autoTick() {
  clearTimeout(atimer);
  if ($("automation").hidden) return;
  if (adraft) { atimer = setTimeout(autoTick, 2000); return; }  // do not redraw under an open editor
  autoStatus().finally(() => (atimer = setTimeout(autoTick, 2000)));
}

function statusOf(id) { return astat && astat.status ? astat.status.find((s) => s.id === id) : null; }

function autoRenderStatus() {
  if (!astat) return;
  const c = astat.clock;
  $("aclock").textContent = c.synced ? "Clock: " + c.local + " (" + WEEK[c.weekday] + ", UTC" + (c.utc_offset_min >= 0 ? "+" : "") + c.utc_offset_min / 60 + ")" : "Clock not set. Schedules and time windows are off until the board joins a Wi-Fi network with internet.";
  $("aclock").className = c.synced ? "hint" : "warn";
  $("aengine").checked = astat.enabled;
  $("alist").querySelectorAll("[data-rule]").forEach((card) => {
    const s = statusOf(card.dataset.rule);
    const line = card.querySelector(".rstat");
    if (!s || !line) return;
    line.textContent = "ran " + s.fired + "x" + (s.fired ? " (" + ago(s.fired_ago_s) + ")" : "") + " · last: " + s.last.replace("_", " ") +
      (s.failed ? " · " + s.failed + " failed" : "") + (s.unconfirmed ? " · " + s.unconfirmed + " not confirmed" : "");
    line.className = "rstat" + (s.failed || s.unconfirmed ? " warn" : "");
  });
  const ev = $("aevents");
  ev.replaceChildren(...(astat.events.length ? astat.events.map((e) => el("li", { textContent: ago(e.ago_s) + " · " + e.rule + " · " + e.kind + (e.text ? " · " + e.text : "") })) : [el("li", { className: "hint", textContent: "Nothing yet." })]));
}

function autoRender() {
  $("aoffset").value = acfg.utc_offset_min;
  $("aengine").checked = acfg.enabled;
  renderLocks();
  const list = $("alist");
  list.replaceChildren(...acfg.rules.map(ruleCard));
  $("aempty").hidden = acfg.rules.length > 0;
  $("aadd").disabled = !!adraft || (asch && acfg.rules.length >= asch.limits.rules);
  $("aeditor").hidden = !adraft;
  if (adraft) renderEditor();
  autoRenderStatus();
}

function ruleCard(r) {
  const card = el("div", { className: "card rule" + (r.enabled ? "" : " off") });
  card.dataset.rule = r.id;
  const conds = r.conditions.length ? " if " + r.conditions.map(describeCond).join(r.match === "any" ? " OR " : " AND ") : "";
  card.append(el("div", { className: "top" }, el("strong", { textContent: r.name || r.id }), el("small", { textContent: r.enabled ? "on" : "off" })),
    el("p", { className: "desc", textContent: describeTrigger(r.trigger) + conds + ", then " + r.actions.map(describeAction).join(", then ") + (r.cooldown_s ? " (at most every " + r.cooldown_s + " s)" : "") + "." }),
    el("small", { className: "rstat", textContent: "" }));
  const bar = el("div", { className: "rbar" });
  const btn = (txt, fn) => { const b = el("button", { type: "button", textContent: txt }); b.onclick = fn; bar.append(b); };
  btn("Run now", () => post("/automation/rules/run?id=" + r.id, "Asked it to run."));
  btn(r.enabled ? "Disable" : "Enable", () => post("/automation/rules/" + (r.enabled ? "disable" : "enable") + "?id=" + r.id, ""));
  btn("Edit", () => { adraft = clone(r); aisNew = false; autoRender(); $("aeditor").scrollIntoView(); });
  btn("Delete", () => { if (confirm("Delete the rule '" + (r.name || r.id) + "'?")) post("/automation/rules?id=" + r.id, "", "DELETE"); });
  card.append(bar);
  return card;
}

async function post(path, okMsg, method) {
  const m = $("amsg");
  m.className = "";
  try {
    await api(path, { method: method || "POST" });
    m.textContent = okMsg;
    setTimeout(autoLoad, 700);  // the change is applied on the board's next pass, then saved
  } catch (e) { m.className = "err"; m.textContent = "Refused: " + e.message + (e.field ? " (" + e.field + ")" : ""); }
}

// ---- settings: clock offset, engine switch, interlocks -----------------------------------------------------

function renderLocks() {
  const box = $("alocks");
  box.replaceChildren();
  const relays = devices.filter((d) => d.type === "relay");
  acfg.interlocks.forEach((p, i) => {
    const sel = (k) => { const s = el("select"); relays.forEach((d) => s.append(el("option", { value: d.id, textContent: d.name, selected: d.id === p[k] }))); s.onchange = () => (p[k] = s.value); return s; };
    const rm = el("button", { type: "button", textContent: "remove" });
    rm.onclick = () => { acfg.interlocks.splice(i, 1); renderLocks(); };
    box.append(el("div", { className: "lock" }, sel(0), el("span", { textContent: "never on together with" }), sel(1), rm));
  });
  $("alockadd").hidden = relays.length < 2 || (asch && acfg.interlocks.length >= asch.limits.interlocks);
}

$("alockadd").onclick = () => {
  const r = devices.filter((d) => d.type === "relay");
  acfg.interlocks.push([r[0].id, r[1].id]);
  renderLocks();
};

async function saveSettings(body, msg) {
  const m = $("amsg");
  m.className = "";
  try {
    await api("/automation/settings", jsonBody("PUT", body));
    m.textContent = msg;
    setTimeout(autoLoad, 700);
  } catch (e) { m.className = "err"; m.textContent = "Refused: " + e.message + (e.field ? " (" + e.field + ")" : ""); }
}
$("aengine").onchange = () => saveSettings({ enabled: $("aengine").checked }, $("aengine").checked ? "Engine on." : "Engine off: no rule will run.");
$("aoffsetsave").onclick = () => saveSettings({ utc_offset_min: Math.round(+$("aoffset").value) }, "Offset saved.");
$("alocksave").onclick = () => saveSettings({ interlocks: acfg.interlocks }, "Interlocks saved.");

// ---- the rule editor --------------------------------------------------------------------------------------

function newRule() {
  let n = 1;
  while (acfg.rules.some((r) => r.id === "rule" + n)) n++;
  return { id: "rule" + n, name: "", enabled: true, trigger: { type: "manual" }, match: "all", conditions: [], actions: [{ type: "event", text: "hello" }], cooldown_s: 0 };
}
$("aadd").onclick = () => { adraft = newRule(); aisNew = true; autoRender(); $("aeditor").scrollIntoView(); };

function field(label, input) { return el("label", {}, label, input); }
function inp(obj, key, props, conv) {
  const i = el("input", props || {});
  i.value = obj[key] === undefined ? "" : obj[key];
  i.oninput = () => (obj[key] = conv ? conv(i.value) : i.value);
  return i;
}
function sel(obj, key, items, redraw) {
  const s = el("select");
  items.forEach(([v, t]) => s.append(el("option", { value: v, textContent: t, selected: String(obj[key]) === String(v) })));
  s.onchange = () => { obj[key] = s.value; if (redraw) renderEditor(); };
  return s;
}
function dayBoxes(obj) {
  const wrap = el("div", { className: "days" });
  const cur = obj.days || [0, 1, 2, 3, 4, 5, 6];
  WEEK.forEach((n, d) => {
    const c = el("input", { type: "checkbox", checked: cur.includes(d) });
    c.onchange = () => { const s = new Set(obj.days || [0, 1, 2, 3, 4, 5, 6]); c.checked ? s.add(d) : s.delete(d); obj.days = [...s].sort(); };
    wrap.append(el("label", { className: "row" }, c, n));
  });
  return wrap;
}
const devOptions = (filter) => devices.filter(filter).map((d) => [d.id, d.name + " (" + d.id + ")"]);

function renderEditor() {
  const f = $("aeform"), r = adraft, t = r.trigger;
  f.replaceChildren();
  f.append(el("h3", { textContent: aisNew ? "New rule" : "Edit " + r.id }));
  const id = inp(r, "id", { maxLength: 16 });
  id.readOnly = !aisNew;
  f.append(field("Id (letters, digits, _ and -)", id), field("Name", inp(r, "name", { maxLength: 24 })));

  // trigger
  f.append(el("h4", { textContent: "When" }), field("Trigger", sel(t, "type", asch.triggers.map((x) => [x, TRIG[x] || x]), true)));
  if (t.type === "change") {
    if (!adev(t.device)) { t.device = (devices[0] || {}).id; }
    f.append(field("Device", sel(t, "device", devOptions((d) => numProps(d).length), true)));
    const props = numProps(adev(t.device));
    if (!props.includes(t.property)) t.property = props[0];
    f.append(field("Property", sel(t, "property", props.map((p) => [p, PN[p] || p]))));
  }
  if (t.type === "interval") { t.every_s = t.every_s || 60; f.append(field("Every (seconds)", inp(t, "every_s", { type: "number", min: 1, max: 86400 }, (v) => +v))); }
  if (t.type === "schedule") { t.time = t.time || "18:00"; f.append(field("Time", inp(t, "time", { type: "time" })), field("Days", dayBoxes(t))); }
  if (t.type === "wifi") { t.event = t.event || "connected"; f.append(field("Event", sel(t, "event", [["connected", "connects"], ["disconnected", "drops"]]))); }
  if (t.type === "when" && !r.conditions.length) f.append(el("p", { className: "warn", textContent: "Add at least one condition below: this trigger watches them." }));

  // conditions
  f.append(el("h4", { textContent: "Only if" }));
  if (r.conditions.length > 1) f.append(field("Match", sel(r, "match", [["all", "all of these (AND)"], ["any", "any of these (OR)"]])));
  r.conditions.forEach((c, i) => f.append(condEditor(r, c, i)));
  const addc = el("button", { type: "button", textContent: "+ condition" });
  addc.disabled = r.conditions.length >= asch.limits.conditions;
  addc.onclick = () => { const d = devices.find((x) => numProps(x).length); r.conditions.push(d ? { device: d.id, property: numProps(d)[0], operator: ">", value: 0 } : { type: "time", from: "08:00", to: "20:00", days: [0, 1, 2, 3, 4, 5, 6] }); renderEditor(); };
  f.append(addc);

  // actions
  f.append(el("h4", { textContent: "Then" }));
  r.actions.forEach((a, i) => f.append(actEditor(r, a, i)));
  const adda = el("button", { type: "button", textContent: "+ action" });
  adda.disabled = r.actions.length >= asch.limits.actions;
  adda.onclick = () => { r.actions.push({ type: "event", text: "hello" }); renderEditor(); };
  f.append(adda);

  f.append(el("h4", { textContent: "Safeguards" }), field("Cooldown: at most once every (seconds, 0 = no limit)", inp(r, "cooldown_s", { type: "number", min: 0, max: 86400 }, (v) => +v)));
  const save = el("button", { type: "submit", textContent: "Save rule" });
  const cancel = el("button", { type: "button", textContent: "Cancel" });
  cancel.onclick = () => { adraft = null; $("aformmsg").textContent = ""; autoRender(); };
  f.append(el("div", { className: "rbar" }, save, cancel));
}

function condEditor(r, c, i) {
  const box = el("fieldset", { className: "sub" });
  const isTime = c.type === "time";
  const kind = el("select");
  [["compare", "A device value"], ["time", "Time of day"]].forEach(([v, t]) => kind.append(el("option", { value: v, textContent: t, selected: (isTime ? "time" : "compare") === v })));
  kind.onchange = () => {
    if (kind.value === "time") r.conditions[i] = { type: "time", from: "08:00", to: "20:00", days: [0, 1, 2, 3, 4, 5, 6] };
    else { const d = devices.find((x) => numProps(x).length); r.conditions[i] = { device: d ? d.id : "", property: d ? numProps(d)[0] : "", operator: ">", value: 0 }; }
    renderEditor();
  };
  box.append(field("Condition " + (i + 1), kind));
  if (isTime) {
    box.append(field("From", inp(c, "from", { type: "time" })), field("To (earlier than From = past midnight)", inp(c, "to", { type: "time" })), field("Days", dayBoxes(c)));
  } else {
    if (!adev(c.device)) c.device = (devices[0] || {}).id;
    box.append(field("Device", sel(c, "device", devOptions((d) => numProps(d).length), true)));
    const props = numProps(adev(c.device));
    if (!props.includes(c.property)) c.property = props[0];
    const opSel = sel(c, "operator", asch.operators.map((o) => [o, o]));
    box.append(field("Property", sel(c, "property", props.map((p) => [p, PN[p] || p]))), field("Is", opSel), field("Value (on = 1, off = 0)", inp(c, "value", { type: "number", step: "any" }, (v) => (v === "" ? 0 : +v))));
    if (r.trigger.type === "when") box.append(field("For at least (seconds, 0 = at once)", inp(c, "for_s", { type: "number", min: 0, max: 86400 }, (v) => +v || 0)));
  }
  const rm = el("button", { type: "button", textContent: "remove condition" });
  rm.onclick = () => { r.conditions.splice(i, 1); renderEditor(); };
  box.append(rm);
  return box;
}

function actChoices(d) {
  if (!d) return ["event"];
  if (d.type === "relay") return ["on", "off", "toggle"];
  if (d.type === "motor") return ["set", "stop"];
  if (d.type === "serial") return ["send"];
  return ["set"];
}

function actEditor(r, a, i) {
  const box = el("fieldset", { className: "sub" });
  const cmdDevs = devices.filter((d) => d.props.some((p) => p.write) || d.type === "serial");
  const kind = el("select");
  [["device", "Command a device"], ["event", "Send an event"]].forEach(([v, t]) => kind.append(el("option", { value: v, textContent: t, selected: (a.type === "event" ? "event" : "device") === v })));
  kind.onchange = () => {
    r.actions[i] = kind.value === "event" ? { type: "event", text: "hello" } : (() => { const d = cmdDevs[0]; return d ? { type: actChoices(d)[0], device: d.id, value: 0 } : { type: "event", text: "hello" }; })();
    renderEditor();
  };
  box.append(field("Action " + (i + 1), kind));
  if (a.type === "event") {
    box.append(field("Event name (15 characters)", inp(a, "text", { maxLength: 15 })));
  } else {
    if (!adev(a.device)) { const d = cmdDevs[0]; a.device = d ? d.id : ""; }
    const d = adev(a.device);
    const choices = actChoices(d);
    if (!choices.includes(a.type)) a.type = choices[0];
    box.append(field("Device", sel(a, "device", cmdDevs.map((x) => [x.id, x.name + " (" + x.id + ")"]), true)), field("Do", sel(a, "type", choices.map((c) => [c, ACT[c]]), true)));
    if (a.type === "set") {
      const lim = d.type === "pwm" ? [0, 100] : d.type === "motor" ? [-100, 100] : [0, 180];
      a.value = a.value === undefined ? lim[0] : a.value;
      box.append(field("Value (" + lim[0] + " to " + lim[1] + ")", inp(a, "value", { type: "number", min: lim[0], max: lim[1], step: "any" }, (v) => +v)));
    }
    if (a.type === "send") box.append(field("Text (15 characters)", inp(a, "text", { maxLength: 15 })));
  }
  const rm = el("button", { type: "button", textContent: "remove action" });
  rm.onclick = () => { r.actions.splice(i, 1); renderEditor(); };
  if (r.actions.length > 1) box.append(rm);
  return box;
}

$("aeform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = $("aformmsg");
  msg.className = "";
  const body = clone(adraft);
  body.actions.forEach((a) => { if (a.type !== "set") delete a.value; if (a.type !== "send" && a.type !== "event") delete a.text; if (a.type === "event") delete a.device; });
  if (body.trigger.type !== "when" && body.trigger.type !== "change") body.conditions.forEach((c) => delete c.for_s);
  try {
    await api("/automation/rules", jsonBody(aisNew ? "POST" : "PUT", body));
    adraft = null;
    msg.textContent = "";
    $("amsg").className = "";
    $("amsg").textContent = "Saved.";
    setTimeout(autoLoad, 700);
    autoRender();
  } catch (err) { msg.className = "err"; msg.textContent = "Rejected: " + err.message + (err.field ? " (" + err.field + ")" : ""); }
});

setInterval(() => { if (!$("automation").hidden && !wsLive) fetchDevices(); }, 3000);
