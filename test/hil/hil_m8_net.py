"""Milestone-8 tests: block channels (binary blocks on /ws, SPEC §5.7), against the app template's example.wave
(int16 "sine,count,noise", up to 1 kHz, blocks of 50 ms; "count" goes up by one per sample). The board is on the
LAN (found by MAC); the first test installs build/app over the air if the board runs something else.
  pc\\.venv\\Scripts\\python -u test/hil/hil_m8_net.py --mac 7c:df:a1:f2:50:30 [test ...]
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
import warnings
from pathlib import Path

import numpy as np
from websockets.sync.client import connect as ws_connect

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT / "pc"))
from devlink_pc import blocks, discovery, image, ota  # noqa: E402
from devlink_pc.api import DEFAULT_KEY, DeviceApi  # noqa: E402
from hil_m1 import Fail, check  # noqa: E402

warnings.filterwarnings("ignore", message="connect() must be used as a context manager")
APP = ROOT / "build" / "app" / "devlink_app.bin"
WAVE = "example.wave"


class Ctx:
    def __init__(self, mac: str):
        self.mac = discovery.norm_mac(mac)
        self.ip = ""

    def find(self, timeout: float = 60) -> str:
        end = time.time() + timeout
        while time.time() < end:
            s = discovery.find_by_mac(self.mac, timeout=5, hint_ip=self.ip or None)
            if s is not None:
                self.ip = s.ip
                return s.ip
        raise Fail(f"board {self.mac} not found on the LAN")

    def api(self, timeout: float = 8) -> DeviceApi:
        return DeviceApi(self.ip, self.mac, DEFAULT_KEY, timeout=timeout)


class Ws:
    """WebSocket client that keeps JSON frames and decoded blocks apart."""

    def __init__(self, c: Ctx):
        self.ws = ws_connect(f"ws://{c.ip}/ws", open_timeout=10, max_size=None)
        self.frames: list[dict] = []
        self.blocks: list[tuple[float, blocks.Block]] = []
        self.binary_msgs = 0
        self.hello = json.loads(self.ws.recv(timeout=5).splitlines()[0])
        if self.hello.get("op") != "hello":
            raise Fail(f"no hello ({self.hello})")

    def pump(self, seconds: float, until_op: bool = False) -> dict | None:
        """Read for @p seconds (or until the first op reply when @p until_op)."""
        end = time.time() + seconds
        while time.time() < end:
            try:
                msg = self.ws.recv(timeout=max(0.05, end - time.time()))
            except TimeoutError:
                break
            if isinstance(msg, bytes):
                self.binary_msgs += 1
                b = blocks.decode(msg)
                if b is None:
                    raise Fail(f"undecodable binary message ({len(msg)} bytes)")
                self.blocks.append((time.time(), b))
                continue
            for line in msg.splitlines():
                if line.strip():
                    f = json.loads(line)
                    if ("op" in f or "channels" in f) and until_op:  # the list reply is the bare /channels object
                        return f
                    if "ch" in f:
                        self.frames.append(f)
        return None

    def op(self, obj: dict, timeout: float = 5) -> dict:
        self.ws.send(json.dumps(obj))
        r = self.pump(timeout, until_op=True)
        if r is None:
            raise Fail(f"no reply to {obj}")
        return r

    def close(self) -> None:
        try:
            self.ws.close()
        except Exception:  # noqa: BLE001
            pass


def wave_id(c: Ctx) -> int:
    ch = {x["name"]: x for x in c.api().channels()["channels"]}
    return ch[WAVE]["id"]


def analyse(blks: list[blocks.Block], hz: int) -> dict:
    """Continuity of a run of example.wave blocks: sequence numbers, the per-sample counter, the time stamps."""
    seq_gaps = sum((b.seq - a.seq - 1) & 0xFFFFFFFF for a, b in zip(blks, blks[1:]))
    counts = np.concatenate([b.values[:, 1] for b in blks]).astype(np.int64)
    count_gaps = int(np.sum((np.diff(counts) % 65536) != 1))
    t_err = max((abs((b.t0_us - a.t0_us) - len(a.values) * a.period_ns / 1000) for a, b in zip(blks, blks[1:])),
                default=0.0)
    n = sum(len(b.values) for b in blks)
    span_s = (blks[-1].t0_us - blks[0].t0_us) / 1e6 if len(blks) > 1 else 0.0
    rate = (n - len(blks[-1].values)) / span_s if span_s > 0 else 0.0
    flags = sum(1 for b in blks if b.flags)
    periods = {b.period_ns for b in blks}
    return {"n": n, "blocks": len(blks), "seq_gaps": seq_gaps, "count_gaps": count_gaps, "t_err_us": t_err,
            "rate": rate, "flags": flags, "periods": periods, "per_block": n / max(1, len(blks)),
            "max_bytes": max(28 + 12 + 6 * len(b.values) for b in blks)}


def run_rate(ws: Ws, hz: int, seconds: float) -> dict:
    """Set the rate, skip the first blocks after the change (the producer restarts its clock), then measure."""
    r = ws.op({"op": "rate", "ch": WAVE, "hz": hz})
    check(r.get("hz") == hz, f"rate {hz} accepted ({r})")
    ws.pump(0.5)
    ws.blocks.clear()
    ws.pump(seconds)
    blks = [b for _, b in ws.blocks if b.period_ns == 1_000_000_000 // hz]
    check(len(blks) >= seconds * 15, f"{len(blks)} blocks at {hz} Hz in {seconds:.0f} s")
    return analyse(blks, hz)


def check_clean(a: dict, hz: int, label: str) -> None:
    check(a["seq_gaps"] == 0 and a["count_gaps"] == 0 and a["flags"] == 0,
          f"{label}: no lost blocks or samples (seq gaps {a['seq_gaps']}, count gaps {a['count_gaps']}, "
          f"flagged {a['flags']}; {a['n']} samples in {a['blocks']} blocks, {a['per_block']:.1f} per block)")
    check(abs(a["rate"] - hz) / hz < 0.02, f"{label}: board sample rate {a['rate']:.1f} Hz (asked {hz})")
    check(a["t_err_us"] < 1e6 / hz + 1, f"{label}: block times line up ({a['t_err_us']:.0f} µs worst)")


# ---- tests ----

def b_setup(c: Ctx) -> None:
    """The board runs the milestone-8 app (installed over the air if needed)."""
    c.find()
    want = image.parse_file(APP).version
    info = c.api().info()
    have = info["fw"]["version"] if info.get("mode") == "app" else None
    if have != want or info["fw"]["project"] != "devlink_app":
        print(f"    installing {APP.name} {want} (board runs {info['fw']['project']} {have})", flush=True)
        r = ota.update_device(c.mac, APP.read_bytes(), DEFAULT_KEY, hint_ip=c.ip)
        check(r.state == "updated", f"OTA: {r.state} {r.message}")
        c.find()
    info = c.api().info()
    check(info["fw"]["version"] == want, f"board runs devlink_app {info['fw']['version']}")


def b_channels(c: Ctx) -> None:
    """/channels lists example.wave as a block channel; the hello says the board has block channels."""
    j = c.api().channels()
    ch = {x["name"]: x for x in j["channels"]}
    w = ch.get(WAVE, {})
    check(w.get("type") == "block" and isinstance(w.get("id"), int), f"{WAVE}: type block, id {w.get('id')}")
    check(w.get("fields") == ["sine", "count", "noise"] and w.get("n_fields") == 3 and w.get("sample") == "i16",
          f"fields {w.get('fields')}, sample {w.get('sample')}")
    check(w.get("default_hz") == 200 and w.get("max_hz") == 1000, "rates: default 200, max 1000")
    check(all("id" not in x for x in j["channels"] if x.get("type") != "block"), "JSON channels unchanged (no id)")
    ws = Ws(c)
    try:
        check(ws.hello.get("block") == 1 and ws.hello.get("api") == 1, f"hello {ws.hello}")
    finally:
        ws.close()


def b_rates(c: Ctx) -> None:
    """100, 200, 500 and 1000 Hz, 10 s each: every block and every sample arrives, at the asked rate."""
    ws = Ws(c)
    try:
        r = ws.op({"op": "sub", "ch": [WAVE]})
        check(r["ok"] == [WAVE] and "no_memory" not in r, f"subscribed ({r})")
        for hz in (100, 200, 500, 1000):
            a = run_rate(ws, hz, 10)
            check_clean(a, hz, f"{hz} Hz")
        check(all(b.ch_id == wave_id(c) for _, b in ws.blocks), "blocks carry the channel id from /channels")
        v = np.concatenate([b.values for _, b in ws.blocks])
        check(-1.01 <= v[:, 0].min() < -0.9 and 0.9 < v[:, 0].max() <= 1.01, "sine scaled to +-1 (scale in header)")
    finally:
        ws.close()


def b_long_1khz(c: Ctx, seconds: int = 60) -> None:
    """1 kHz for 60 s together with JSON channels: no lost blocks, JSON rates unchanged; heap stays fine."""
    ws = Ws(c)
    try:
        heap0 = c.api().status()["heap_free"]
        ws.op({"op": "sub", "ch": [WAVE, "example.sine", "sys.heap"]})
        ws.op({"op": "rate", "ch": "example.sine", "hz": 50})
        a = run_rate(ws, 1000, seconds)
        check_clean(a, 1000, f"1 kHz for {seconds} s")
        n_sine = sum(1 for f in ws.frames if f["ch"] == "example.sine")
        check(n_sine >= seconds * 50 * 0.9, f"example.sine at 50 Hz alongside: {n_sine} frames")
        st = c.api().status()
        check(st["heap_free"] > 40000, f"heap {heap0 // 1024} -> {st['heap_free'] // 1024} KiB with a block ring")
        print(f"    Wi-Fi payload ~{a['n'] * 6 * 1.0 / seconds / 1024:.1f} KiB/s of samples, "
              f"{a['max_bytes']} bytes per message max", flush=True)
    finally:
        ws.close()


def b_two_clients(c: Ctx) -> None:
    """Two clients on example.wave at 200 and 1000 Hz: both get every block, at the highest rate asked."""
    a, b = Ws(c), Ws(c)
    try:
        a.op({"op": "sub", "ch": [WAVE]})
        a.op({"op": "rate", "ch": WAVE, "hz": 200})
        b.op({"op": "sub", "ch": [WAVE]})
        b.op({"op": "rate", "ch": WAVE, "hz": 1000})
        th = threading.Thread(target=lambda: a.pump(10.5), daemon=True)
        a.blocks.clear()
        th.start()
        b.pump(10.5)
        th.join(15)
        for name, ws in (("client A (asked 200)", a), ("client B (asked 1000)", b)):
            blks = [x for _, x in ws.blocks if x.period_ns == 1_000_000]
            blks = blks[3:]  # after the rate change
            check(len(blks) > 150, f"{name}: {len(blks)} blocks at 1000 Hz")
            check_clean(analyse(blks, 1000), 1000, name)
    finally:
        a.close()
        b.close()


def b_json_only_client(c: Ctx) -> None:
    """A client that subscribes only to JSON channels never gets a binary message while another gets blocks."""
    a, b = Ws(c), Ws(c)
    try:
        a.op({"op": "sub", "ch": [WAVE]})
        a.op({"op": "rate", "ch": WAVE, "hz": 1000})
        b.op({"op": "sub", "ch": ["example.sine", "sys.temp"]})
        th = threading.Thread(target=lambda: a.pump(6), daemon=True)
        th.start()
        b.pump(6)
        th.join(10)
        check(b.binary_msgs == 0 and len(b.frames) > 30, f"JSON client: {b.binary_msgs} binary, {len(b.frames)} JSON")
        check(len(a.blocks) > 80, f"block client meanwhile: {len(a.blocks)} blocks")
    finally:
        a.close()
        b.close()


def raw_ws_stalled(ip: str, ops: list[dict]) -> socket.socket:
    """A WebSocket client that sends its ops and then never reads (tiny receive buffer)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    s.settimeout(10)
    s.connect((ip, 80))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: {ip}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(1)
    if b" 101 " not in resp.split(b"\r\n")[0]:
        raise Fail(f"handshake failed: {resp[:60]!r}")
    for op in ops:
        data = json.dumps(op).encode()
        mask = os.urandom(4)
        s.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(x ^ mask[i % 4] for i, x in enumerate(data)))
    return s


def b_slow_client(c: Ctx) -> None:
    """A stalled block client loses whole blocks (counted in drops); a second client still gets every block;
    the stalled client is removed and its ring freed."""
    api = c.api(timeout=15)
    st0 = api.status()
    good = Ws(c)
    s = raw_ws_stalled(c.ip, [{"op": "sub", "ch": [WAVE]}, {"op": "rate", "ch": WAVE, "hz": 1000}])
    try:
        good.op({"op": "sub", "ch": [WAVE]})
        good.op({"op": "rate", "ch": WAVE, "hz": 1000})
        good.pump(0.5)
        good.blocks.clear()
        worst = 0.0
        end = time.time() + 8
        while time.time() < end:
            good.pump(1)
            t0 = time.time()
            st = api.status()
            worst = max(worst, time.time() - t0)
        check(st["drops"] > st0["drops"], f"drops counted ({st0['drops']} -> {st['drops']})")
        check(worst < 3, f"/status answered within {worst:.1f} s every time")
        check_clean(analyse([b for _, b in good.blocks], 1000), 1000, "the client that reads")
    finally:
        s.close()
        good.close()
    time.sleep(12)  # the board gives up on the stalled client
    st = api.status()
    check(st["ws_clients"] == 0, "stalled client removed")
    check(st["heap_free"] >= st0["heap_free"] - 4096,
          f"block rings freed: heap {st0['heap_free'] // 1024} -> {st['heap_free'] // 1024} KiB")


def b_ota_pauses_blocks(c: Ctx) -> None:
    """Blocks pause during an OTA (SPEC §5.7). Runs last: the board reboots into the uploaded app."""
    ws = Ws(c)
    stop = threading.Event()
    try:
        ws.op({"op": "sub", "ch": [WAVE]})
        ws.op({"op": "rate", "ch": WAVE, "hz": 500})
        def reader():
            while not stop.is_set():
                try:
                    ws.pump(0.3)
                except Exception:  # noqa: BLE001  (the board closes the socket when it restarts)
                    return

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        time.sleep(2)
        t_start = time.time()
        api = c.api()
        result: dict = {}
        up = threading.Thread(target=lambda: result.update(r=api.ota(APP.read_bytes())), daemon=True)
        up.start()
        receiving = []
        while up.is_alive():
            try:
                if api.ota_progress()["state"] == "receiving":
                    receiving.append(time.time())
            except Exception:  # noqa: BLE001
                pass
            time.sleep(0.3)
        stop.set()
        check(result.get("r", {}).get("ok", True) is not False and len(receiving) >= 2,
              f"upload accepted, seen in progress ({len(receiving)} polls)")
        during = [t for t, _ in ws.blocks if receiving[1] <= t <= receiving[-1]]
        before = [t for t, _ in ws.blocks if t < t_start]
        check(len(before) > 20, f"blocks before the OTA ({len(before)})")
        check(len(during) == 0, f"no blocks while receiving ({len(during)})")
    finally:
        stop.set()
        ws.close()
    time.sleep(20)
    c.find()


def b_ops_edge_cases(c: Ctx) -> None:
    """Review fixes: list answers in full; "ch":[] unsubscribes nothing; a repeated sub keeps the rate."""
    ws = Ws(c)
    try:
        r = ws.op({"op": "list"})
        names = [x["name"] for x in r.get("channels", [])]
        check(WAVE in names and "error" not in r, f"list answers with {len(names)} channels")
        ws.op({"op": "sub", "ch": [WAVE, "sys.heap"]})
        ws.op({"op": "rate", "ch": WAVE, "hz": 500})
        r = ws.op({"op": "unsub", "ch": []})
        check(r.get("ok") == [], f"unsub of an empty list ({r})")
        r = ws.op({"op": "sub", "ch": [WAVE]})  # again: keeps 500 Hz
        ws.pump(1.0)
        ws.blocks.clear()
        ws.pump(3)
        periods = {b.period_ns for _, b in ws.blocks}
        check(periods == {2_000_000}, f"still subscribed at 500 Hz after both ({sorted(periods)})")
    finally:
        ws.close()


REC = ROOT / "build" / "recovery" / "devlink_recovery.bin"


def b_recovery_cut_off(c: Ctx) -> None:
    """Review fix: a recovery upload cut off after writing started is recorded as a failed self-test; a good
    recovery update afterwards passes its self-test."""
    data = REC.read_bytes()
    want = image.parse_file(REC).version
    s = socket.create_connection((c.ip, 80), timeout=30)
    s.sendall((f"POST /api/v1/ota?target=recovery HTTP/1.1\r\nHost: {c.ip}\r\nX-Api-Key: {DEFAULT_KEY}\r\n"
               f"X-Device-Id: {c.mac}\r\nContent-Length: {len(data)}\r\n\r\n").encode() + data[:300_000])
    time.sleep(3)
    s.close()  # connection lost after about 300 KB of factory were written
    end = time.time() + 30
    st = {}
    while time.time() < end:
        try:
            st = c.api().info().get("recovery_selftest", {})
            if st.get("state") == "failed":
                break
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1)
    check(st.get("state") == "failed" and "incomplete" in st.get("version", ""), f"recorded: {st}")
    r = ota.update_recovery(c.mac, data, DEFAULT_KEY, hint_ip=c.ip)
    check(r.state == "updated", f"good recovery update: {r.state} {r.message}")
    c.find()
    info = c.api().info()
    check(info["recovery_selftest"]["state"] == "ok" and info["recovery"]["version"] == want,
          f"recovery {info['recovery']['version']}, self-test {info['recovery_selftest']}")


TESTS = [b_setup, b_channels, b_rates, b_long_1khz, b_two_clients, b_json_only_client, b_slow_client,
         b_ops_edge_cases, b_recovery_cut_off, b_ota_pauses_blocks]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mac", required=True)
    ap.add_argument("tests", nargs="*")
    args = ap.parse_args()
    by_name = {f.__name__: f for f in TESTS}
    tests = [by_name[n] for n in args.tests] if args.tests else TESTS
    c = Ctx(args.mac)
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}", flush=True)
        t0 = time.time()
        try:
            if not c.ip:
                c.find()
            t(c)
            results.append((t.__name__, "PASS", ""))
        except Fail as e:
            results.append((t.__name__, "FAIL", str(e)))
            print(f"  FAIL: {e}", flush=True)
        except Exception as e:  # noqa: BLE001
            results.append((t.__name__, "ERROR", repr(e)))
            traceback.print_exc()
        print(f"   ({time.time() - t0:.0f} s)", flush=True)
    print("\n== summary")
    for name, res, msg in results:
        print(f"  {res:5} {name} {msg}")
    return 0 if all(r == "PASS" for _, r, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
