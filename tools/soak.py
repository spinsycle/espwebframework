#!/usr/bin/env python3
"""24-hour soak logger: polls /status, /devices and /diagnostics and writes one CSV row per pass.

  python tools/soak.py --ip 192.168.4.23 --password espframework1 --hours 24 --out soak.csv

Pass = no restart (uptime never goes down), reset reason never watchdog/exception, heap low-water not
trending down, every reply complete JSON. Failures are counted, never fatal: a board that stops
answering is part of the result. Re-signs in on 401 (sessions end after 1 h idle or on a restart).
Stdlib only. Keep every browser tab closed or visible-and-open on purpose: a hidden page sends no
heartbeat, so any device with a failsafe will trip.
"""
import argparse, csv, http.client, json, subprocess, sys, time

FIELDS = ["t_s", "uptime_s", "heap_free", "heap_max_block", "heap_min_free", "frag_pct", "loop_max_ms",
          "ws_clients", "devices", "tripped", "events_total", "reset_reason", "wd", "fault", "ping_ok", "errors"]


class Board:
    def __init__(self, ip, password, timeout):
        self.ip, self.password, self.timeout, self.token = ip, password, timeout, None

    def req(self, method, path, body=None):
        c = http.client.HTTPConnection(self.ip, 80, timeout=self.timeout)
        try:
            h = {"Connection": "close"}
            if self.token:
                h["Authorization"] = "Bearer " + self.token
            data = None
            if body is not None:
                data = json.dumps(body); h["Content-Type"] = "application/json"
            c.request(method, "/api/v1" + path, data, h)
            r = c.getresponse()
            raw = r.read()
            return r.status, raw
        finally:
            c.close()

    def login(self):
        self.token = None
        s, raw = self.req("POST", "/session", {"password": self.password})
        if s != 200:
            raise RuntimeError("login HTTP %d" % s)
        self.token = json.loads(raw)["token"]

    def get(self, path):
        """Returns parsed JSON; raises on transport error, bad status or truncated/invalid JSON."""
        if not self.token:
            self.login()
        s, raw = self.req("GET", path)
        if s == 401:
            self.login()
            s, raw = self.req("GET", path)
        if s != 200:
            raise RuntimeError("%s HTTP %d" % (path, s))
        return json.loads(raw)  # a cut-off reply raises here


def ping(ip):
    flag = "-n" if sys.platform == "win32" else "-c"
    r = subprocess.run(["ping", flag, "1", ip], capture_output=True, text=True)
    return int(r.returncode == 0 and "TTL=" in r.stdout.upper() or r.returncode == 0 and sys.platform != "win32")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--hours", type=float, default=24)
    ap.add_argument("--every", type=float, default=20, help="seconds between passes")
    ap.add_argument("--out", default="soak.csv")
    ap.add_argument("--timeout", type=float, default=20)
    a = ap.parse_args()

    b = Board(a.ip, a.password, a.timeout)
    t0 = time.time()
    end = t0 + a.hours * 3600
    last_up, restarts, errors, passes, bad_reason = None, 0, 0, 0, set()
    min_free_seen = None
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, FIELDS); w.writeheader()
        while time.time() < end:
            row = dict.fromkeys(FIELDS, "")
            row["t_s"] = int(time.time() - t0)
            pass_err = 0
            row["ping_ok"] = ping(a.ip)  # separates "the board is down" from "the PC or Wi-Fi is"
            try:
                # one request a pass: /diagnostics has everything (the ESP8266 refuses new connections after a burst)
                dg = b.get("/diagnostics")
                h = dg["heap"]
                row.update(uptime_s=dg["uptime_s"], heap_free=h["free"], heap_max_block=h["max_block"],
                           heap_min_free=h["min_free"], frag_pct=h["fragmentation_pct"],
                           loop_max_ms=dg["loop_max_ms"], ws_clients=dg["ws_clients"],
                           devices=len(dg.get("devices", [])), events_total=dg["events_total"],
                           reset_reason=dg["reset_reason"], wd=int(dg["watchdog_reset"]),
                           fault=int(dg["fault_reset"]))
                row["tripped"] = sum(1 for d in dg.get("devices", []) if d.get("tripped"))
                if last_up is not None and dg["uptime_s"] < last_up:
                    restarts += 1
                    print("RESTART at t=%ds: %s" % (row["t_s"], dg["reset_reason"]), flush=True)
                if dg["watchdog_reset"] or dg["fault_reset"]:
                    bad_reason.add(dg["reset_reason"])
                last_up = dg["uptime_s"]
                min_free_seen = h["min_free"] if min_free_seen is None else min(min_free_seen, h["min_free"])
            except Exception as e:  # transport, status or truncated JSON
                pass_err = 1
                errors += 1
                print("t=%ds error: %s" % (row["t_s"], e), flush=True)
                b.token = None
            row["errors"] = pass_err
            passes += 1
            w.writerow(row); f.flush()
            time.sleep(a.every)
    print("\nsoak done: %d passes, %d errors, %d restarts, lowest heap low-water %s, bad resets %s"
          % (passes, errors, restarts, min_free_seen, sorted(bad_reason) or "none"))
    return 0 if restarts == 0 and errors == 0 and not bad_reason else 1


if __name__ == "__main__":
    sys.exit(main())
