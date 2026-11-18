"use strict";
// Diagnostics tab: what the board is doing and what happened to it. Everything here is read-only, except the
// watchdog test button, which asks first. Uses $, api, el, fmtUp, fmtBytes from app.js.
(function () {
  let timer = 0, busy = false;

  const when = (e) => (e.time ? new Date(e.time * 1000).toLocaleString() : "up " + fmtUp(e.up_s));
  const BAD = ["failsafe", "boot_fault", "login_failed", "login_locked", "watchdog_test"];

  function fill(tbl, rows) {
    const tb = $(tbl).querySelector("tbody");
    tb.replaceChildren(...rows.map((r) => el("tr", null, ...r.map((c) => el("td", { textContent: c })))));
  }

  function list(id, items, empty) {
    $(id).replaceChildren(...(items.length ? items : [el("li", { className: "hint", textContent: empty })]));
  }

  async function load() {
    if (busy) return;
    busy = true;
    try {
      const d = await api("/diagnostics");
      $("gup").textContent = fmtUp(d.uptime_s);
      $("grst").textContent = d.reset_reason;
      $("grstnote").textContent = d.watchdog_reset ? "the watchdog reset the board: the loop stalled" : d.fault_reset ? "a fault, not a normal restart" : "";
      $("grstnote").className = d.fault_reset ? "warn" : "";
      $("gloop").textContent = d.loop_max_ms + " ms";
      $("gheap").textContent = fmtBytes(d.heap.free) + " free";
      $("gheap2").textContent = "lowest " + fmtBytes(d.heap.min_free) + " · largest block " + fmtBytes(d.heap.max_block) + " · fragmentation " + d.heap.fragmentation_pct + " %";
      $("gfs").textContent = fmtBytes(d.filesystem.used) + " of " + fmtBytes(d.filesystem.total);
      $("gws").textContent = d.ws_clients;
      fill("gdev", d.devices.map((v) => [v.id, v.type, v.enabled ? "enabled" : "disabled",
        v.failsafe_s ? v.failsafe_s + " s" + (v.tripped ? " · TRIPPED" : "") : "off", v.errors === undefined ? "" : v.errors + (v.ok ? "" : " · no reading")]));
      list("gevents", d.events.map((e) => el("li", null, el("span", { className: "k" + (BAD.includes(e.kind) ? " bad" : ""), textContent: e.kind }), when(e) + " · " + e.text)), "Nothing yet.");
    } catch (e) { /* the offline dot shows it */ }
    try {
      const a = await api("/automation/status");
      list("gauto", a.events.slice(0, 8).map((e) => el("li", { textContent: e.ago_s + " s ago · " + e.rule + " · " + e.kind + (e.text ? " · " + e.text : "") })), "Nothing yet.");
    } catch (e) { /* automation not reachable */ }
    try {
      const g = await api("/gpio");
      fill("ggpio", g.pins.map((p) => [p.gpio, p.label, p.note, p.use || "", p.level === undefined ? "-" : p.level ? "HIGH" : "low"]));
    } catch (e) { /* ignore */ }
    busy = false;
  }

  window.diagShow = function () {
    clearTimeout(timer);
    if ($("diag").hidden) return;
    load().finally(() => (timer = setTimeout(window.diagShow, 3000)));
  };

  $("gwdt").onclick = async () => {
    const msg = $("gwdtmsg");
    if (!confirm("Hang the board on purpose? The watchdog restarts it in a few seconds. Outputs go back to their boot state.")) return;
    try {
      await api("/system/test-watchdog", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ confirm: "hang" }) });
      msg.className = "";
      msg.textContent = "Hanging now. Reload this page in about 10 seconds, then check Last reset.";
    } catch (err) {
      msg.className = "err";
      msg.textContent = err.message === "not_supported" ? "This chip does not support the test." : "Failed: " + err.message;
    }
  };
})();
