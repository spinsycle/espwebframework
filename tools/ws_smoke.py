"""Exercises the WebSocket (/ws) on a live board. Standard library only (own tiny WebSocket client).
Usage: python tools/ws_smoke.py <ip> [esp8266|esp32]
Creates smoke_* devices and removes them again; refuses to run if any smoke_* device already exists.
"""
import base64, json, os, socket, struct, sys, time, urllib.request, urllib.error

IP = sys.argv[1]
CHIP = sys.argv[2] if len(sys.argv) > 2 else "esp8266"
RELAY_PIN, PWM_PIN = (2, 14) if CHIP == "esp8266" else (2, 25)
CLOSED = "__closed__"
passed = failed = 0


def check(name, cond, extra=""):
    global passed, failed
    if cond:
        passed += 1
        print("  ok   " + name)
    else:
        failed += 1
        print("  FAIL " + name + (("  [" + str(extra) + "]") if extra else ""))


def rest(method, path, body=None):
    req = urllib.request.Request(f"http://{IP}/api/v1{path}", method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=6) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


class WS:
    def __init__(self):
        self.s = socket.create_connection((IP, 80), timeout=6)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET /ws HTTP/1.1\r\nHost: {IP}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.s.recv(1)
            if not chunk:
                break
            head += chunk
        self.ok = b" 101 " in head.split(b"\r\n")[0]
        self.buf = b""

    def _need(self, n, deadline):
        while len(self.buf) < n:
            left = deadline - time.time()
            if left <= 0:
                return False
            self.s.settimeout(left)
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                return False
            except OSError:
                return False
            if not d:
                return False
            self.buf += d
        return True

    def recv(self, timeout=3.0):
        """Next JSON message, None on timeout, CLOSED if the server closed the connection."""
        deadline = time.time() + timeout
        while True:
            if not self._need(2, deadline):
                return CLOSED if self._eof() else None
            b0, b1 = self.buf[0], self.buf[1]
            ln, off = b1 & 0x7F, 2
            if ln == 126:
                if not self._need(4, deadline): return None
                ln, off = struct.unpack(">H", self.buf[2:4])[0], 4
            elif ln == 127:
                if not self._need(10, deadline): return None
                ln, off = struct.unpack(">Q", self.buf[2:10])[0], 10
            if not self._need(off + ln, deadline): return None
            payload, self.buf = self.buf[off:off + ln], self.buf[off + ln:]
            op = b0 & 0x0F
            if op == 8:
                return CLOSED
            if op == 9:  # ping: answer with pong
                self._frame(0xA, payload)
                continue
            if op == 1:
                return json.loads(payload)

    def _eof(self):
        try:
            self.s.settimeout(0.01)
            return self.s.recv(1) == b""
        except socket.timeout:
            return False
        except OSError:
            return True

    def _frame(self, op, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send(self, obj):
        self._frame(1, (obj if isinstance(obj, str) else json.dumps(obj)).encode())

    def wait_for(self, pred, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(max(0.05, end - time.time()))
            if m == CLOSED:
                return None
            if m and pred(m):
                return m
        return None

    def close(self):
        try:
            self._frame(8, b"")
            self.s.close()
        except OSError:
            pass


def state_of(ws, dev_id, timeout=2.5, **want):
    return ws.wait_for(lambda m: m.get("event") == "device_state" and m.get("id") == dev_id
                       and all(m["state"].get(k) == v for k, v in want.items()), timeout)


code, body = rest("GET", "/devices")
if any(d["id"].startswith("smoke_") for d in body.get("devices", [])):
    print("smoke_* devices already present; remove them first")
    sys.exit(2)

print("== connect")
a = WS()
check("upgrade accepted", a.ok)
hello = a.recv()
check("hello event first", isinstance(hello, dict) and hello.get("event") == "hello" and hello.get("proto") == 1, hello)
st = a.wait_for(lambda m: m.get("event") == "status", 4)
check("status event within 4 s", st is not None and "uptime_s" in st and "heap" in st)
check("status reports an open page", st is not None and st.get("ws_clients", 0) >= 1, st and st.get("ws_clients"))

print("== structure and state events")
rest("POST", "/devices", {"id": "smoke_ws", "type": "relay", "pin": RELAY_PIN, "invert": CHIP == "esp8266"})
check("devices_changed after add", a.wait_for(lambda m: m.get("event") == "devices_changed", 3) is not None)
a.send({"type": "command", "id": "smoke_ws", "action": "toggle"})
m = state_of(a, "smoke_ws", on=True)
check("toggle over WS gives device_state on=true", m is not None)
a.send({"type": "command", "id": "smoke_ws", "action": "set", "value": False})
check("set false gives device_state on=false", state_of(a, "smoke_ws", on=False) is not None)

print("== two clients stay consistent")
b = WS()
b.recv()  # hello
a.send({"type": "command", "id": "smoke_ws", "action": "toggle"})
check("client A sees the change", state_of(a, "smoke_ws", on=True) is not None)
check("client B sees the same change", state_of(b, "smoke_ws", on=True) is not None)
b.send({"type": "command", "id": "smoke_ws", "action": "set", "value": False})
check("client A sees B's command", state_of(a, "smoke_ws", on=False) is not None)
code, body = rest("POST", "/command", {"id": "smoke_ws", "action": "toggle"})
check("REST command shows up on the sockets", state_of(a, "smoke_ws", on=True) is not None and state_of(b, "smoke_ws", on=True) is not None, code)
rest("POST", "/command", {"id": "smoke_ws", "action": "set", "value": False})

print("== client limit")
c = WS(); c.recv()
d = WS()
first = d.recv(3)
gone = first == CLOSED or d.recv(3) == CLOSED
check("fourth client is refused", gone, first)
check("first three still work", c.ok and a.recv(0.3) != CLOSED)
d.close()
c.close()
b.close()

print("== rejected messages")
a.send("{not json")
m = a.wait_for(lambda x: x.get("event") == "error")
check("bad json -> bad_json", m is not None and m["error"] == "bad_json", m)
a.send({"type": "command", "id": "smoke_nope", "action": "toggle"})
m = a.wait_for(lambda x: x.get("event") == "error")
check("unknown device -> not_found", m is not None and m["error"] == "not_found" and m.get("id") == "smoke_nope", m)
a.send({"type": "command", "id": "smoke_ws", "action": "set", "value": 1})
m = a.wait_for(lambda x: x.get("event") == "error")
check("relay with a number -> bad_value", m is not None and m["error"] == "bad_value", m)
a.send({"type": "teleport"})
m = a.wait_for(lambda x: x.get("event") == "error")
check("unknown type -> bad_type", m is not None and m["error"] == "bad_type", m)
a.send({"type": "ping"})
check("ping -> pong", a.wait_for(lambda x: x.get("event") == "pong") is not None)
a.send("x" * 400)
m = a.wait_for(lambda x: x.get("event") == "error")
check("oversize message -> too_large", m is not None and m["error"] == "too_large", m)

print("== pwm: a flood of sets collapses to the latest")
rest("POST", "/devices", {"id": "smoke_pwm", "type": "pwm", "pin": PWM_PIN, "freq_hz": 500})
a.wait_for(lambda m: m.get("event") == "devices_changed", 3)
time.sleep(0.5)
for v in range(1, 31):  # 30 messages, under the 40/s limit
    a.send({"type": "command", "id": "smoke_pwm", "action": "set", "value": v})
final = state_of(a, "smoke_pwm", 4, duty=30)
check("final duty is the last value sent (30)", final is not None)
errs = []
while True:
    m = a.recv(0.4)
    if m is None or m == CLOSED:
        break
    if m.get("event") == "error":
        errs.append(m)
check("no busy errors from the burst", not any(e["error"] == "busy" for e in errs), errs)
check("pwm 101 -> bad_value", (a.send({"type": "command", "id": "smoke_pwm", "action": "set", "value": 101}) or True)
      and (a.wait_for(lambda x: x.get("event") == "error") or {}).get("error") == "bad_value")

print("== rate limit")
time.sleep(1.2)
for i in range(120):
    a.send({"type": "command", "id": "smoke_pwm", "action": "set", "value": i % 100})
limited = a.wait_for(lambda x: x.get("event") == "error" and x.get("error") == "rate_limited", 3)
check("flood of 120 gets rate_limited", limited is not None)
time.sleep(1.3)
a.send({"type": "ping"})
check("socket works again after the window", a.wait_for(lambda x: x.get("event") == "pong", 3) is not None)

print("== cleanup")
for dev in ("smoke_ws", "smoke_pwm"):
    rest("DELETE", f"/devices?id={dev}")
check("devices_changed after removal", a.wait_for(lambda m: m.get("event") == "devices_changed", 3) is not None)
time.sleep(0.4)
code, body = rest("GET", "/devices")
check("nothing left behind", not any(x["id"].startswith("smoke_") for x in body["devices"]))
a.close()
time.sleep(0.5)
code, body = rest("GET", "/status")
print(f"  heap free {body['heap']['free']}, largest block {body['heap']['max_block']}, ws clients now {body['ws_clients']}")
print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
