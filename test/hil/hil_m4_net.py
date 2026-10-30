"""Milestone-4 tests: streaming over the WebSocket /ws (SPEC §5.7, §5.8), against the app (bootlab: sysmon +
mod_example). PC on the app's config AP:
  powershell -File test/hil/run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X -Target app -Script hil_m4_net.py
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import sys
import threading
import time
import traceback
from pathlib import Path

import requests
from websockets.sync.client import connect as ws_connect

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import devlink_tools as dl  # noqa: E402
from hil_m1 import Board, Fail, check  # noqa: E402
from hil_m2_net import Api  # noqa: E402
from hil_m3_net import rejoin  # noqa: E402

WS_URL = "ws://192.168.4.1/ws"
BUILD = dl.ROOT / "build"


class Ws:
    def __init__(self):
        self.ws = ws_connect(WS_URL, open_timeout=10, max_size=None)
        self.pending: list[dict] = []
        hello = self.recv(5)
        if hello is None or hello.get("op") != "hello":
            raise Fail(f"no hello ({hello})")

    def send(self, obj: dict) -> None:
        self.ws.send(json.dumps(obj))

    def recv(self, timeout: float) -> dict | None:
        """Next frame (one WS message may carry several, newline-separated)."""
        if self.pending:
            return self.pending.pop(0)
        try:
            msg = self.ws.recv(timeout=timeout)
        except TimeoutError:
            return None
        frames = [json.loads(line) for line in msg.splitlines() if line.strip()]
        self.pending.extend(frames[1:])
        return frames[0] if frames else None

    def op(self, obj: dict, timeout: float = 5) -> dict:
        """Send an op and return its reply (skipping data frames)."""
        self.send(obj)
        end = time.time() + timeout
        while time.time() < end:
            f = self.recv(max(0.1, end - time.time()))
            if f is not None and "op" in f:
                return f
        raise Fail(f"no reply to {obj}")

    def op_reply(self, timeout: float = 5) -> dict:
        """Wait for the next op reply without sending anything."""
        end = time.time() + timeout
        while time.time() < end:
            f = self.recv(max(0.1, end - time.time()))
            if f is not None and "op" in f:
                return f
        raise Fail("no reply")

    def collect(self, seconds: float) -> list[dict]:
        out, end = [], time.time() + seconds
        while time.time() < end:
            f = self.recv(max(0.05, end - time.time()))
            if f is not None and "ch" in f:
                out.append(f)
        return out

    def close(self) -> None:
        try:
            self.ws.close()
        except Exception:  # noqa: BLE001
            pass


def count(frames: list[dict], ch: str) -> int:
    return sum(1 for f in frames if f["ch"] == ch)


def w_channels(b: Board, api: Api) -> None:
    """/channels lists log, sys.* and example.* with metadata."""
    j = api.get("/channels").json()
    names = {c["name"]: c for c in j["channels"]}
    for n in ("log", "sys.temp", "sys.heap", "sys.rssi", "example.sine", "example.adc"):
        check(n in names, f"channel {n} ({names.get(n, {}).get('unit', '-')})")
    check(names["example.sine"]["max_hz"] == 200 and j["max_clients"] == 2, "metadata: max_hz, max_clients")


def w_rates(b: Board, api: Api) -> None:
    """Default rate on subscribe, rate change, clamp to max_hz, unsubscribe."""
    ws = Ws()
    try:
        r = ws.op({"op": "sub", "ch": ["example.sine", "sys.temp", "example.adc"]})
        check(sorted(r["ok"]) == ["example.adc", "example.sine", "sys.temp"] and r["unknown"] == [], f"subscribed {r}")
        fr = ws.collect(5)
        n_sine, n_temp, n_adc = count(fr, "example.sine"), count(fr, "sys.temp"), count(fr, "example.adc")
        check(38 <= n_sine <= 60, f"example.sine at default 10 Hz: {n_sine} in 5 s")
        check(3 <= n_temp <= 7, f"sys.temp at 1 Hz: {n_temp} in 5 s")
        check(18 <= n_adc <= 30 and all(len(f["v"]) == 3 for f in fr if f["ch"] == "example.adc"),
              f"example.adc at 5 Hz, 3 values: {n_adc}")
        t = [f["v"] for f in fr if f["ch"] == "sys.temp"]
        check(all(10 < v < 90 for v in t), f"die temperature plausible ({t[:3]})")
        ts = [f["ts"] for f in fr if f["ch"] == "example.sine"]
        check(ts == sorted(ts), "timestamps increase")

        r = ws.op({"op": "rate", "ch": "example.sine", "hz": 50})
        check(r.get("hz") == 50, f"rate 50 accepted ({r})")
        n = count(ws.collect(4), "example.sine")
        check(160 <= n <= 220, f"example.sine at 50 Hz: {n} in 4 s")
        r = ws.op({"op": "rate", "ch": "example.sine", "hz": 5000})
        check(r.get("hz") == 200, f"rate 5000 clamped to max_hz 200 ({r})")
        r = ws.op({"op": "unsub", "ch": ["example.sine"]})
        ws.collect(0.5)
        fr = ws.collect(2)
        check(count(fr, "example.sine") == 0 and count(fr, "example.adc") > 0, "unsubscribed sine only")
        ws.op({"op": "unsub"})
        ws.collect(0.5)
        check(len(ws.collect(2)) == 0, "unsub without ch -> no frames")
    finally:
        ws.close()


def w_bad_requests(b: Board, api: Api) -> None:
    """Unknown channel, invalid JSON, unknown op -> clear errors; the connection stays usable."""
    ws = Ws()
    try:
        r = ws.op({"op": "sub", "ch": ["nope", "sys.heap"]})
        check(r["ok"] == ["sys.heap"] and r["unknown"] == ["nope"], f"unknown channel reported ({r})")
        ws.ws.send("{not json")
        r = ws.op_reply()
        check(r.get("error") == "invalid_json", f"invalid JSON answered ({r})")
        r = ws.op({"op": "ping?"})
        check(r.get("error") == "unknown_op", f"unknown op answered ({r})")
        r = ws.op({"op": "rate", "ch": "sys.heap", "hz": -1})
        check(r.get("error") == "bad_rate_request", f"negative rate rejected ({r})")
        check(count(ws.collect(3), "sys.heap") >= 2, "still streaming after bad requests")
    finally:
        ws.close()


def w_max_clients(b: Board, api: Api) -> None:
    """Max 2 WebSocket clients: a third is refused; after one leaves, a new one fits."""
    a, c = Ws(), Ws()
    try:
        third = ws_connect(WS_URL, open_timeout=10)
        msg = third.recv(timeout=5)
        check("too_many_clients" in msg, f"third client refused ({msg})")
        try:
            third.recv(timeout=3)
            closed = False
        except Exception:  # noqa: BLE001
            closed = True
        check(closed, "third connection closed by the board")
        check(api.get("/status").json()["ws_clients"] == 2, "/status: 2 clients")
        a.close()
        time.sleep(1)
        d = Ws()
        d.close()
        check(True, "a new client fits after one left")
    finally:
        a.close()
        c.close()


def w_log_channel(b: Board, api: Api) -> None:
    """ESP_LOG* output appears on the log channel with level, tag and message."""
    ws = Ws()
    try:
        ws.op({"op": "sub", "ch": ["log"]})
        ws.collect(0.5)
        r = api.ota(b"\x00" * 1024)  # rejected upload -> ESP_LOGW(TAG "dl_ota", "upload rejected: ...")
        check(r.status_code == 400, f"bad upload rejected ({r.status_code})")
        logs = ws.collect(3)
        hit = [f for f in logs if f["ch"] == "log" and f["tag"] == "dl_ota" and "upload rejected" in f["msg"]]
        check(len(hit) == 1 and hit[0]["lvl"] == "W", f"log frame {hit[:1]}")
    finally:
        ws.close()


def raw_ws_stalled(ops: list[dict]) -> socket.socket:
    """A WebSocket client that sends its ops and then never reads: tiny receive buffer, no background reader
    (the websockets library keeps reading in a thread, so it can't play a stalled client)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    s.settimeout(10)
    s.connect(("192.168.4.1", 80))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: 192.168.4.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(1)
    if b" 101 " not in resp.split(b"\r\n")[0]:
        raise Fail(f"handshake failed: {resp[:60]!r}")
    for op in ops:
        data = json.dumps(op).encode()
        mask = os.urandom(4)
        s.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))
    return s


def w_slow_client(b: Board, api: Api) -> None:
    """A client that stops reading loses frames (counted in /status); the board stays responsive."""
    before = api.get("/status").json()["drops"]
    s = raw_ws_stalled([{"op": "sub", "ch": ["example.sine", "example.adc", "log"]},
                        {"op": "rate", "ch": "example.sine", "hz": 200},
                        {"op": "rate", "ch": "example.adc", "hz": 50}])
    try:
        worst = 0.0
        for _ in range(8):  # 8 s of not reading, poll the board meanwhile
            t0 = time.time()
            st = api.get("/status", timeout=15).json()
            worst = max(worst, time.time() - t0)
            time.sleep(1)
        check(st["drops"] > before, f"drops counted ({before} -> {st['drops']})")
        check(worst < 3, f"/status answered within {worst:.1f} s every time while the client stalled")
        check(st["heap_free"] > 40000, f"heap fine ({st['heap_free']})")
    finally:
        s.close()
    time.sleep(12)  # the board gives up on the stalled client (send timeout)
    check(api.get("/status").json()["ws_clients"] == 0, "stalled client removed")


def w_stream_long(b: Board, api: Api, seconds: int = 120) -> None:
    """Test 3 (shortened): sys.temp at 10 Hz + log for 2 minutes without a disconnect."""
    ws = Ws()
    try:
        ws.op({"op": "sub", "ch": ["sys.temp", "log"]})
        ws.op({"op": "rate", "ch": "sys.temp", "hz": 10})
        fr = ws.collect(seconds)
        n = count(fr, "sys.temp")
        check(n >= seconds * 10 * 0.95, f"{n} sys.temp frames in {seconds} s (expected ~{seconds * 10})")
        ts = [f["ts"] for f in fr if f["ch"] == "sys.temp"]
        gap = max(b2 - a2 for a2, b2 in zip(ts, ts[1:])) / 1e6
        check(gap < 1.0, f"longest gap {gap:.2f} s")
    finally:
        ws.close()


def w_ota_pauses_streaming(b: Board, api: Api) -> None:
    """Streaming pauses during an OTA (SPEC §5.7). Runs last: the board reboots into the new app."""
    ws = Ws()
    got: list[tuple[float, dict]] = []
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try:
                f = ws.recv(0.2)
            except Exception:  # noqa: BLE001
                return
            if f is not None and "ch" in f:
                got.append((time.time(), f))

    try:
        ws.op({"op": "sub", "ch": ["example.sine"]})
        ws.op({"op": "rate", "ch": "example.sine", "hz": 50})
        th = threading.Thread(target=reader, daemon=True)
        th.start()
        time.sleep(2)
        t_start = time.time()
        image = (BUILD / "bootlab" / "bootlab.bin").read_bytes()
        result = {}
        up = threading.Thread(target=lambda: result.update(r=api.ota(image)), daemon=True)
        up.start()
        receiving = []
        while up.is_alive():
            try:
                p = api.get("/ota/progress", timeout=3).json()
                if p["state"] == "receiving":
                    receiving.append(time.time())
            except requests.RequestException:
                pass
            time.sleep(0.3)
        stop.set()
        r = result.get("r")
        check(r is not None and r.status_code == 200, f"upload accepted ({r.status_code if r is not None else 'no reply'} "
              f"{r.text[:80] if r is not None else ''})")
        check(len(receiving) >= 2, f"saw the upload in progress ({len(receiving)} polls)")
        during = [t for t, _ in got if receiving[1] <= t <= receiving[-1]]
        before = [t for t, _ in got if t < t_start]
        check(len(before) > 50, f"streaming before the OTA ({len(before)} frames)")
        check(len(during) == 0, f"no frames while receiving ({len(during)} frames)")
    finally:
        stop.set()
        ws.close()
    time.sleep(20)  # the board restarts into the new app (boot seen in the USB log)


TESTS = [w_channels, w_rates, w_bad_requests, w_max_clients, w_log_channel, w_slow_client, w_stream_long,
         w_ota_pauses_streaming]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--ap", required=True)
    ap.add_argument("tests", nargs="*")
    args = ap.parse_args()
    by_name = {f.__name__: f for f in TESTS}
    tests = [by_name[n] for n in args.tests] if args.tests else TESTS
    b = Board(dl.find_port(args.port, args.mac))
    b.con.echo = False
    b.con.open(timeout=3.0)
    b.con.drain(0.5)

    def usb_log():  # the board's USB log goes into the test output (diagnosis); sole reader of the port
        while True:
            text = b.con._read()
            for line in text.splitlines():
                if line.strip():
                    print(f"    | {line.rstrip()}", flush=True)

    threading.Thread(target=usb_log, daemon=True).start()
    api = Api(args.url, args.mac)
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}", flush=True)
        t0 = time.time()
        try:
            rejoin(args.ap)
            t(b, api)
            results.append((t.__name__, "PASS", ""))
        except Fail as e:
            results.append((t.__name__, "FAIL", str(e)))
            print(f"    FAIL: {e}")
        except Exception as e:  # noqa: BLE001
            results.append((t.__name__, "ERROR", repr(e)))
            traceback.print_exc()
        print(f"   ({time.time() - t0:.0f} s)", flush=True)
    print("\n== Summary")
    for name, res, msg in results:
        print(f"  {res:5} {name} {msg}")
    return 0 if all(r == "PASS" for _, r, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
