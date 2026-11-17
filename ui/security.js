"use strict";
// Login, password change, logout and firmware update. Loaded before app.js; the gate (login form) covers the page
// until the board says there is a session. The board does all the checking: this page only asks and shows.

let authed = false;
const byId = (id) => document.getElementById(id);

let gateMode = "";  // which form is up; background requests keep calling showGate() and must not steal the cursor

function showGate(mode, forced) {  // mode: "login" | "change"
  authed = false;
  const fresh = byId("gate").hidden || gateMode !== mode;  // only move the focus when the form first appears
  gateMode = mode;
  byId("gate").hidden = false;
  byId("loginform").hidden = mode !== "login";
  byId("pwform").hidden = mode !== "change";
  byId("pwcancel").hidden = !!forced;
  if (!fresh) return;
  const f = byId("pwform").elements;
  (mode === "login" ? byId("loginform").elements.password : f.current.value ? f.password : f.current).focus();
}

// app.js calls this when the board answers 401 (the session ended, or never existed).
function authFail() { if (byId("gate").hidden || authed) showGate("login"); }

function hideGate() { byId("gate").hidden = true; gateMode = ""; }

async function authBoot(start) {
  try {
    const r = await fetch("/api/v1/session");
    const s = await r.json();
    byId("gatename").textContent = s.name ? s.name : "Sign in";
    if (!s.authenticated) { showGate("login"); return; }
    authed = true;
    byId("logout").hidden = false;
    if (s.must_change) { showGate("change", true); byId("pwmsg").textContent = "The factory password must be replaced before you can continue."; return; }
    hideGate();
    start();
  } catch (e) {
    byId("loginmsg").textContent = "Cannot reach the board.";
    showGate("login");
  }
}

byId("loginform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const msg = byId("loginmsg"), pw = byId("loginform").elements.password.value;
  msg.className = "";
  msg.textContent = "Checking...";
  try {
    const r = await fetch("/api/v1/session", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ password: pw }) });
    const j = await r.json().catch(() => ({}));
    if (r.status === 429) { msg.className = "err"; msg.textContent = "Too many wrong passwords. Try again in " + j.retry_s + " s."; return; }
    if (!r.ok) { msg.className = "err"; msg.textContent = "Wrong password."; return; }
    byId("loginform").elements.password.value = "";
    if (j.must_change) {
      byId("pwform").elements.current.value = pw;  // just typed: no need to type it twice
      showGate("change", true);
      byId("pwmsg").textContent = "Choose a new password to continue.";
      return;
    }
    location.reload();
  } catch (err) { msg.className = "err"; msg.textContent = "Cannot reach the board."; }
});

byId("pwform").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = byId("pwform").elements, msg = byId("pwmsg");
  msg.className = "err";
  if (f.password.value !== f.again.value) { msg.textContent = "The two new passwords differ."; return; }
  if (f.password.value.length < 8 || f.password.value.length > 32) { msg.textContent = "Use 8 to 32 characters."; return; }
  try {
    const r = await fetch("/api/v1/password", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ current: f.current.value, password: f.password.value }) });
    const j = await r.json().catch(() => ({}));
    if (r.ok) { msg.className = ""; msg.textContent = "Password changed."; setTimeout(() => location.reload(), 600); return; }
    msg.textContent = j.error === "wrong_password" ? "The current password is wrong." : j.error === "default_password" ? "That is the factory password: pick another." : j.error === "locked" ? "Too many tries. Wait a little." : "Rejected: use 8 to 32 printable characters.";
  } catch (err) { msg.textContent = "Cannot reach the board."; }
});

byId("pwcancel").onclick = () => { hideGate(); authed = true; };
byId("logout").onclick = async () => { try { await fetch("/api/v1/session", { method: "DELETE" }); } catch (e) { /* leaving anyway */ } location.reload(); };
byId("pwopen").onclick = () => {
  const f = byId("pwform");
  f.reset();
  byId("pwmsg").textContent = "";
  showGate("change", false);
};

// ---- firmware update ------------------------------------------------------------------------------------

// MD5 (RFC 1321), because the board's updater checks one. Returns the lower-case hex digest of a Uint8Array.
function md5hex(bytes) {
  const K = new Uint32Array(64);
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) >>> 0;
  const S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
  const len = bytes.length, total = (((len + 8) >>> 6) + 1) * 64;
  const buf = new Uint8Array(total);
  buf.set(bytes);
  buf[len] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(total - 8, (len << 3) >>> 0, true);
  dv.setUint32(total - 4, Math.floor(len / 536870912), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < total; off += 64) {
    for (let i = 0; i < 16; i++) M[i] = dv.getUint32(off + 4 * i, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      F = (F + A + K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      B = (B + ((F << S[i]) | (F >>> (32 - S[i])))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  return [a0, b0, c0, d0].map((x) => [0, 8, 16, 24].map((s) => ((x >>> s) & 255).toString(16).padStart(2, "0")).join("")).join("");
}

byId("fwgo").onclick = async () => {
  const msg = byId("fwmsg"), file = byId("fwfile").files[0], bar = byId("fwbar");
  msg.className = "";
  if (!file) { msg.textContent = "Choose a firmware .bin file first."; return; }
  if (!/\.bin$/i.test(file.name)) { msg.className = "err"; msg.textContent = "That is not a .bin file."; return; }
  const bytes = new Uint8Array(await file.arrayBuffer());
  if (bytes[0] !== 0xE9) { msg.className = "err"; msg.textContent = "That file does not look like a firmware image."; return; }
  if (!confirm("Install " + file.name + " (" + Math.round(bytes.length / 1024) + " KB)? The board restarts, and a wrong image for this chip will not boot. Do not remove power.")) return;
  msg.textContent = "Checking the file...";
  const sum = md5hex(bytes);
  const fd = new FormData();
  fd.append("firmware", file, file.name);
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/v1/system/update?md5=" + sum);
  bar.hidden = false;
  bar.value = 0;
  xhr.upload.onprogress = (ev) => { if (ev.lengthComputable) { bar.value = (ev.loaded / ev.total) * 100; msg.textContent = "Uploading... " + Math.round(bar.value) + "%"; } };
  xhr.onerror = () => { msg.className = "err"; msg.textContent = "The connection dropped during the upload. The board keeps its old firmware."; };
  xhr.onload = () => {
    let j = {};
    try { j = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
    if (xhr.status === 200 && j.ok) {
      msg.textContent = "Installed. The board is restarting; this page reloads when it is back.";
      const wait = setInterval(async () => {
        try { const r = await fetch("/api/v1/session", { cache: "no-store" }); if (r.ok) { clearInterval(wait); location.reload(); } } catch (e) { /* still restarting */ }
      }, 3000);
    } else if (xhr.status === 401) {
      authFail();
    } else {
      msg.className = "err";
      msg.textContent = "Refused: " + (j.detail || j.error || "error " + xhr.status) + ". The board keeps its old firmware.";
    }
  };
  xhr.send(fd);
};
