"use strict";
// Automation tab: the rule list with live status, a rule editor, the clock and interlock settings, and the event log.
// The firmware validates every change; this page only builds the JSON. Uses from app.js: $, el, api, devices, fetchDevices.

let acfg = { enabled: true, utc_offset_min: 0, interlocks: [], rules: [] };
let astat = null, asch = null;
let adraft = null, aisNew = false, atimer = 0;
let aseq = null, aseqNew = false, atim = null, atimNew = false;

const WEEK = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const PN = { state: "on/off", level: "level", temp_c: "temperature", humidity: "humidity", pressure_hpa: "pressure", count: "count", ok: "reading ok" };
const TRIG = { change: "A value changes", when: "Conditions become true", interval: "Every so often", schedule: "At a time of day", startup: "When the board starts", wifi: "Wi-Fi connects or drops", manual: "Run by hand only", timer: "A timer finishes", sequence: "A sequence finishes or stops" };
const ACT = { on: "Turn on", off: "Turn off", toggle: "Toggle", set: "Set value", stop: "Stop", send: "Send text", event: "Send an event", seq_start: "Start", seq_stop: "Stop", timer_start: "Start", timer_stop: "Stop" };
const seqs = () => acfg.sequences || [];
const timers = () => acfg.timers || [];
const tname = (id) => { const t = timers().find((x) => x.id === id); return t ? (t.name || t.id) : id + " (missing)"; };
const sname = (id) => { const q = seqs().find((x) => x.id === id); return q ? (q.name || q.id) : id + " (missing)"; };
const aMmss = (s) => Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
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
    case "timer": return "when the timer " + tname(t.device) + " finishes";
    case "sequence": return "when the sequence " + sname(t.device) + (t.event === "stopped" ? " is stopped early" : " finishes");
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
  if (a.type === "seq_start") return "start the sequence " + sname(a.device);
  if (a.type === "seq_stop") return "stop the sequence " + sname(a.device);
  if (a.type === "timer_start") return "start the timer " + tname(a.device);
  if (a.type === "timer_stop") return "stop the timer " + tname(a.device);
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
  const fast = astat && (astat.timers || []).some((t) => t.running);
  autoStatus().finally(() => (atimer = setTimeout(autoTick, fast ? 1000 : 2000)));
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
    line.textContent = "ran " + s.fired + "x" + (s.fired ? " (" + ago(s.fired_ago_s) + ")" : "") + " Â· last: " + s.last.replace("_", " ") +
      (s.failed ? " Â· " + s.failed + " failed" : "") + (s.unconfirmed ? " Â· " + s.unconfirmed + " not confirmed" : "");
    line.className = "rstat" + (s.failed || s.unconfirmed ? " warn" : "");
  });
  $("aseqlist").querySelectorAll("[data-seq]").forEach((card) => {
    const q = (astat.sequences || []).find((x) => x.id === card.dataset.seq);
    const line = card.querySelector(".live");
    if (!q || !line) return;
    line.textContent = q.state === "idle" ? "idle · finished " + q.done + "x, stopped " + q.stopped + "x" :
      q.state + " · step " + q.step + " of " + q.steps + (q.repeat !== 1 ? " · pass " + q.pass : "") + (q.waiting_s !== undefined ? (q.until ? " · waiting for a condition, " : " · ") + q.waiting_s + " s" : "");
    line.className = "live" + (q.state === "running" ? " on" : "");
    const btns = card.querySelectorAll(".rbar button");
    btns.forEach((b) => { if (b.dataset.ctl) b.hidden = !({ idle: ["start"], running: ["pause", "stop"], paused: ["resume", "stop"] }[q.state] || []).includes(b.dataset.ctl); });
  });
  $("atimerlist").querySelectorAll("[data-timer]").forEach((card) => {
    const t = (astat.timers || []).find((x) => x.id === card.dataset.timer);
    const line = card.querySelector(".live");
    if (!t || !line) return;
    line.textContent = (t.running ? "running · " + aMmss(t.remaining_s) + " left" : "stopped") + " · finished " + t.finished + "x";
    line.className = "live" + (t.running ? " on" : "");
    card.querySelectorAll(".rbar button").forEach((b) => { if (b.dataset.ctl === "stop") b.hidden = !t.running; });
  });
  const ev = $("aevents");
  ev.replaceChildren(...(astat.events.length ? astat.events.map((e) => el("li", { textContent: ago(e.ago_s) + " Â· " + e.rule + " Â· " + e.kind + (e.text ? " Â· " + e.text : "") })) : [el("li", { className: "hint", textContent: "Nothing yet." })]));
}

function autoRender() {
  $("aoffset").value = acfg.utc_offset_min;
  $("aengine").checked = acfg.enabled;
  renderLocks();
  const list = $("alist");
  list.replaceChildren(...acfg.rules.map(ruleCard));
  $("aempty").hidden = acfg.rules.length > 0;
  $("aadd").disabled = !!adraft || (asch && acfg.rules.length >= asch.limits.rules);
  renderSeqs();
  renderTimers();
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
  if (t.type === "timer") {
    if (!timers().some((x) => x.id === t.device)) t.device = (timers()[0] || {}).id;
    f.append(timers().length ? field("Timer", sel(t, "device", timers().map((x) => [x.id, x.name || x.id]))) : el("p", { className: "warn", textContent: "Create a timer below first." }));
  }
  if (t.type === "sequence") {
    if (!seqs().some((x) => x.id === t.device)) t.device = (seqs()[0] || {}).id;
    t.event = t.event || "done";
    f.append(seqs().length ? field("Sequence", sel(t, "device", seqs().map((x) => [x.id, x.name || x.id]))) : el("p", { className: "warn", textContent: "Create a sequence below first." }), field("When it", sel(t, "event", [["done", "finishes all its passes"], ["stopped", "is stopped early (stop, guard or timeout)"]])));
  }
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

function actKind(a) { return a.type === "event" ? "event" : a.type.indexOf("timer_") === 0 ? "timer" : a.type.indexOf("seq_") === 0 ? "seq" : "device"; }
function replaceObj(o, n) { Object.keys(o).forEach((k) => delete o[k]); Object.assign(o, n); }

// The fields of one action. Edits `a` in place. allowSeq: rules may start and stop sequences, sequence steps may not.
function actFields(box, a, redraw, allowSeq) {
  const cmdDevs = devices.filter((d) => d.props.some((p) => p.write) || d.type === "serial");
  const kinds = [["device", "Command a device"], ["timer", "Start or stop a timer"], ["event", "Send an event"]];
  if (allowSeq) kinds.splice(1, 0, ["seq", "Start or stop a sequence"]);
  const kind = el("select");
  kinds.forEach(([v, t]) => kind.append(el("option", { value: v, textContent: t, selected: actKind(a) === v })));
  kind.onchange = () => {
    const k = kind.value;
    if (k === "event") replaceObj(a, { type: "event", text: "hello" });
    else if (k === "timer") replaceObj(a, { type: "timer_start", device: (timers()[0] || {}).id || "" });
    else if (k === "seq") replaceObj(a, { type: "seq_start", device: (seqs()[0] || {}).id || "" });
    else { const d = cmdDevs[0]; replaceObj(a, d ? { type: actChoices(d)[0], device: d.id, value: 0 } : { type: "event", text: "hello" }); }
    redraw();
  };
  box.append(field("What", kind));
  const k = actKind(a);
  if (k === "event") {
    box.append(field("Event name (15 characters)", inp(a, "text", { maxLength: 15 })));
  } else if (k === "timer" || k === "seq") {
    const list = k === "timer" ? timers() : seqs();
    if (!list.some((x) => x.id === a.device)) a.device = (list[0] || {}).id || "";
    if (!list.length) { box.append(el("p", { className: "warn", textContent: "Create a " + (k === "timer" ? "timer" : "sequence") + " first." })); return; }
    box.append(field(k === "timer" ? "Timer" : "Sequence", sel(a, "device", list.map((x) => [x.id, x.name || x.id]))), field("Do", sel(a, "type", [[k + "_start", "Start"], [k + "_stop", "Stop"]])));
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
}

function cleanAction(a) {
  if (a.type !== "set") delete a.value;
  if (a.type !== "send" && a.type !== "event") delete a.text;
  if (a.type === "event") delete a.device;
}

function actEditor(r, a, i) {
  const box = el("fieldset", { className: "sub" });
  box.append(el("legend", { textContent: "Action " + (i + 1) }));
  actFields(box, a, renderEditor, true);
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
  body.actions.forEach(cleanAction);
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

// ---- sequences -----------------------------------------------------------------------------------------

function defaultCond() {
  const d = devices.find((x) => numProps(x).length);
  return d ? { device: d.id, property: numProps(d)[0], operator: ">", value: 0 } : { type: "time", from: "08:00", to: "20:00", days: [0, 1, 2, 3, 4, 5, 6] };
}

function repeatText(n) { return n === 1 ? "once" : n === 0 ? "until stopped" : n + " times"; }

function describeStep(s) {
  if (s.type === "wait") return "wait " + s.seconds + " s";
  if (s.type === "until") return "wait until " + describeCond(s.condition) + " (at most " + s.timeout_s + " s, then " + (s.on_timeout === "stop" ? "stop" : "carry on") + ")";
  return (s.guard ? "if " + describeCond(s.guard) + " then " : "") + describeAction(s.action) + (s.guard ? (s.else === "stop" ? ", otherwise stop" : ", otherwise skip") : "");
}

function seqCard(q) {
  const card = el("div", { className: "card rule" });
  card.dataset.seq = q.id;
  card.append(el("div", { className: "top" }, el("strong", { textContent: q.name || q.id }), el("small", { textContent: q.steps.length + " steps · " + repeatText(q.repeat) })),
    el("p", { className: "desc", textContent: q.steps.map((s, i) => (i + 1) + ". " + describeStep(s)).join("  ") }),
    el("small", { className: "live", textContent: "" }));
  const bar = el("div", { className: "rbar" });
  const btn = (txt, ctl, fn) => { const b = el("button", { type: "button", textContent: txt }); if (ctl) b.dataset.ctl = ctl; b.onclick = fn; bar.append(b); };
  ["start", "pause", "resume", "stop"].forEach((c) => btn(c[0].toUpperCase() + c.slice(1), c, () => post("/automation/sequences/" + c + "?id=" + q.id, "")));
  btn("Edit", null, () => { aseq = clone(q); aseqNew = false; renderSeqs(); $("aseqeditor").scrollIntoView(); });
  btn("Delete", null, () => { if (confirm("Delete the sequence '" + (q.name || q.id) + "'? Rules that start it will report an error.")) post("/automation/sequences?id=" + q.id, "", "DELETE"); });
  card.append(bar);
  bar.querySelectorAll("[data-ctl]").forEach((b) => (b.hidden = b.dataset.ctl !== "start"));
  return card;
}

const stepsUsed = (except) => seqs().reduce((n, q) => n + (q.id === except ? 0 : q.steps.length), 0);

function renderSeqs() {
  $("aseqlist").replaceChildren(...seqs().map(seqCard));
  $("aseqempty").hidden = seqs().length > 0;
  $("aseqadd").disabled = !!aseq || (asch && seqs().length >= asch.limits.sequences);
  $("aseqeditor").hidden = !aseq;
  if (aseq) renderSeqEditor();
  autoRenderStatus();
}

$("aseqadd").onclick = () => {
  let n = 1;
  while (seqs().some((q) => q.id === "seq" + n)) n++;
  aseq = { id: "seq" + n, name: "", repeat: 1, steps: [{ type: "do", action: { type: "event", text: "start" } }] };
  aseqNew = true;
  renderSeqs();
  $("aseqeditor").scrollIntoView();
};

// A condition held in holder[key]; its kind can switch between a device value and a time of day.
function condFields(box, holder, key, redraw) {
  const c = holder[key];
  const kind = el("select");
  [["compare", "A device value"], ["time", "Time of day"]].forEach(([v, t]) => kind.append(el("option", { value: v, textContent: t, selected: (c.type === "time" ? "time" : "compare") === v })));
  kind.onchange = () => { holder[key] = kind.value === "time" ? { type: "time", from: "08:00", to: "20:00", days: [0, 1, 2, 3, 4, 5, 6] } : defaultCond(); redraw(); };
  box.append(field("Condition", kind));
  if (c.type === "time") {
    box.append(field("From", inp(c, "from", { type: "time" })), field("To", inp(c, "to", { type: "time" })), field("Days", dayBoxes(c)));
    return;
  }
  if (!adev(c.device)) c.device = (devices[0] || {}).id;
  box.append(field("Device", sel(c, "device", devOptions((d) => numProps(d).length), true)));
  const props = numProps(adev(c.device));
  if (!props.includes(c.property)) c.property = props[0];
  box.append(field("Property", sel(c, "property", props.map((p) => [p, PN[p] || p]))), field("Is", sel(c, "operator", asch.operators.map((o) => [o, o]))), field("Value (on = 1, off = 0)", inp(c, "value", { type: "number", step: "any" }, (v) => (v === "" ? 0 : +v))));
}

function stepBox(q, st, i) {
  const box = el("fieldset", { className: "step" });
  box.append(el("legend", { textContent: "Step " + (i + 1) }));
  const kind = el("select");
  [["do", "Do something"], ["wait", "Wait"], ["until", "Wait until a condition holds"]].forEach(([v, t]) => kind.append(el("option", { value: v, textContent: t, selected: st.type === v })));
  kind.onchange = () => {
    replaceObj(st, kind.value === "wait" ? { type: "wait", seconds: 5 } : kind.value === "until" ? { type: "until", condition: defaultCond(), timeout_s: 30, on_timeout: "continue" } : { type: "do", action: { type: "event", text: "hello" } });
    renderSeqEditor();
  };
  box.append(field("Step type", kind));
  if (st.type === "wait") {
    box.append(field("Seconds (1 to 86400)", inp(st, "seconds", { type: "number", min: 1, max: 86400 }, (v) => +v)));
  } else if (st.type === "until") {
    condFields(box, st, "condition", renderSeqEditor);
    box.append(field("Give up after (seconds)", inp(st, "timeout_s", { type: "number", min: 1, max: 86400 }, (v) => +v)), field("If it times out", sel(st, "on_timeout", [["continue", "carry on with the next step"], ["stop", "stop the sequence"]])));
  } else {
    actFields(box, st.action, renderSeqEditor, false);
    const g = el("input", { type: "checkbox", checked: !!st.guard });
    g.onchange = () => { if (g.checked) { st.guard = defaultCond(); st.else = "skip"; } else { delete st.guard; delete st.else; } renderSeqEditor(); };
    box.append(el("label", { className: "row" }, g, "Only if a condition holds"));
    if (st.guard) {
      condFields(box, st, "guard", renderSeqEditor);
      box.append(field("Otherwise", sel(st, "else", [["skip", "skip this step"], ["stop", "stop the sequence"]])));
    }
  }
  const bar = el("div", { className: "rbar" });
  const mv = (txt, d) => { const b = el("button", { type: "button", textContent: txt }); b.disabled = i + d < 0 || i + d >= q.steps.length; b.onclick = () => { const t = q.steps[i]; q.steps[i] = q.steps[i + d]; q.steps[i + d] = t; renderSeqEditor(); }; bar.append(b); };
  mv("Move up", -1);
  mv("Move down", 1);
  if (q.steps.length > 1) { const rm = el("button", { type: "button", textContent: "Remove step" }); rm.onclick = () => { q.steps.splice(i, 1); renderSeqEditor(); }; bar.append(rm); }
  box.append(bar);
  return box;
}

function renderSeqEditor() {
  const f = $("aseqform"), q = aseq;
  f.replaceChildren();
  f.append(el("h3", { textContent: aseqNew ? "New sequence" : "Edit " + q.id }));
  const id = inp(q, "id", { maxLength: 16 });
  id.readOnly = !aseqNew;
  f.append(field("Id (letters, digits, _ and -)", id), field("Name", inp(q, "name", { maxLength: 24 })),
    field("Repeat (1 = once, 0 = until stopped; more than once needs a wait step)", inp(q, "repeat", { type: "number", min: 0, max: 255 }, (v) => +v)));
  const box = el("div", { className: "steps" });
  q.steps.forEach((st, i) => box.append(stepBox(q, st, i)));
  f.append(box);
  const used = stepsUsed(aseqNew ? null : q.id) + q.steps.length;
  const add = el("button", { type: "button", textContent: "+ step" });
  add.disabled = q.steps.length >= asch.limits.steps || used >= asch.limits.steps_total;
  add.onclick = () => { q.steps.push({ type: "wait", seconds: 5 }); renderSeqEditor(); };
  f.append(add, el("small", { className: "hint", textContent: q.steps.length + " of " + asch.limits.steps + " steps here, " + used + " of " + asch.limits.steps_total + " in all sequences. A step that fails is logged and the sequence carries on; add a wait-until step to check a result." }));
  const save = el("button", { type: "submit", textContent: "Save sequence" });
  const cancel = el("button", { type: "button", textContent: "Cancel" });
  cancel.onclick = () => { aseq = null; $("aseqmsg").textContent = ""; renderSeqs(); };
  f.append(el("div", { className: "rbar" }, save, cancel));
}

$("aseqform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = $("aseqmsg");
  msg.className = "";
  const body = clone(aseq);
  body.steps.forEach((s) => {
    if (s.type === "do") { cleanAction(s.action); if (s.guard) delete s.guard.for_s; }
    if (s.type === "until") delete s.condition.for_s;
  });
  try {
    await api("/automation/sequences", jsonBody(aseqNew ? "POST" : "PUT", body));
    aseq = null;
    msg.textContent = "";
    $("amsg").className = "";
    $("amsg").textContent = "Saved.";
    setTimeout(autoLoad, 700);
    renderSeqs();
  } catch (err) { msg.className = "err"; msg.textContent = "Rejected: " + err.message + (err.field ? " (" + err.field + ")" : ""); }
});

// ---- timers --------------------------------------------------------------------------------------------

function timerCard(t) {
  const card = el("div", { className: "card rule" });
  card.dataset.timer = t.id;
  card.append(el("div", { className: "top" }, el("strong", { textContent: t.name || t.id }), el("small", { textContent: aMmss(t.duration_s) + (t.repeat ? " · repeats" : "") })),
    el("small", { className: "live", textContent: "" }));
  const bar = el("div", { className: "rbar" });
  const btn = (txt, ctl, fn) => { const b = el("button", { type: "button", textContent: txt }); if (ctl) b.dataset.ctl = ctl; b.onclick = fn; bar.append(b); };
  btn("Start", "start", () => post("/automation/timers/start?id=" + t.id, ""));
  btn("Stop", "stop", () => post("/automation/timers/stop?id=" + t.id, ""));
  btn("Edit", null, () => { atim = { id: t.id, name: t.name, duration_s: t.duration_s, repeat: t.repeat }; atimNew = false; renderTimers(); $("atimereditor").scrollIntoView(); });
  btn("Delete", null, () => { if (confirm("Delete the timer '" + (t.name || t.id) + "'? Rules that use it will report an error.")) post("/automation/timers?id=" + t.id, "", "DELETE"); });
  card.append(bar);
  return card;
}

function renderTimers() {
  $("atimerlist").replaceChildren(...timers().map(timerCard));
  $("atimerempty").hidden = timers().length > 0;
  $("atimeradd").disabled = !!atim || (asch && timers().length >= asch.limits.timers);
  $("atimereditor").hidden = !atim;
  if (atim) renderTimerEditor();
  autoRenderStatus();
}

$("atimeradd").onclick = () => {
  let n = 1;
  while (timers().some((t) => t.id === "timer" + n)) n++;
  atim = { id: "timer" + n, name: "", duration_s: 60, repeat: false };
  atimNew = true;
  renderTimers();
  $("atimereditor").scrollIntoView();
};

function renderTimerEditor() {
  const f = $("atimerform"), t = atim;
  f.replaceChildren();
  f.append(el("h3", { textContent: atimNew ? "New timer" : "Edit " + t.id }));
  const id = inp(t, "id", { maxLength: 16 });
  id.readOnly = !atimNew;
  const rep = el("input", { type: "checkbox", checked: t.repeat });
  rep.onchange = () => (t.repeat = rep.checked);
  f.append(field("Id (letters, digits, _ and -)", id), field("Name", inp(t, "name", { maxLength: 24 })),
    field("Length (seconds, 1 to 86400)", inp(t, "duration_s", { type: "number", min: 1, max: 86400 }, (v) => +v)),
    el("label", { className: "row" }, rep, "Start again by itself when it finishes"),
    el("small", { className: "hint", textContent: "A timer counts down once it is started (from the Start button, a rule, a sequence or a dashboard Timer). Rules can start when it finishes. Editing a timer stops it." }));
  const save = el("button", { type: "submit", textContent: "Save timer" });
  const cancel = el("button", { type: "button", textContent: "Cancel" });
  cancel.onclick = () => { atim = null; $("atimermsg").textContent = ""; renderTimers(); };
  f.append(el("div", { className: "rbar" }, save, cancel));
}

$("atimerform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = $("atimermsg");
  msg.className = "";
  try {
    await api("/automation/timers", jsonBody(atimNew ? "POST" : "PUT", atim));
    atim = null;
    msg.textContent = "";
    $("amsg").className = "";
    $("amsg").textContent = "Saved.";
    setTimeout(autoLoad, 700);
    renderTimers();
  } catch (err) { msg.className = "err"; msg.textContent = "Rejected: " + err.message + (err.field ? " (" + err.field + ")" : ""); }
});
