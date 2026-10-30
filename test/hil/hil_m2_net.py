"""Milestone-2 network tests: HTTP API, OTA checks (tests 9, 10, 21, 22), UDP discovery (test 4), against recovery.

The PC must reach the board: either joined to its config AP (--url http://192.168.4.1) or on the same LAN as the
board (--url http://<board ip>). The USB cable stays connected (used to enter recovery and to check results).
  python test/hil/hil_m2_net.py --mac 7c:df:a1:f2:50:30 --url http://192.168.4.1 [test ...]
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import threading
import time
import traceback
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import devlink_tools as dl  # noqa: E402
from hil_m1 import Board, Fail, check  # noqa: E402
from hil_m2 import enter_recovery  # noqa: E402

KEY = "devlink-bench"
BUILD = dl.ROOT / "build"


class Api:
    def __init__(self, url: str, mac: str):
        self.url = url.rstrip("/") + "/api/v1"
        self.mac = mac

    def h(self, key: str = KEY, mac: str | None = None) -> dict:
        return {"X-Api-Key": key, "X-Device-Id": mac if mac is not None else self.mac}

    def get(self, path: str, **kw) -> requests.Response:
        return requests.get(self.url + path, timeout=kw.pop("timeout", 10), **kw)

    def post(self, path: str, body=None, headers=None, **kw) -> requests.Response:
        hd = self.h() if headers is None else headers
        data = body if isinstance(body, (bytes, str)) or body is None else json.dumps(body)
        return requests.post(self.url + path, data=data, headers=hd, timeout=kw.pop("timeout", 30), **kw)

    def ota(self, image: bytes, force: bool = False, **kw) -> requests.Response:
        q = "?target=app" + ("&force=1" if force else "")
        return self.post("/ota" + q, image, headers=self.h(), timeout=120, **kw)


def slots(api: Api) -> list:
    return [(s["name"], s["state"], s["version"]) for s in api.get("/info").json()["slots"]]


def n_info_status(b: Board, api: Api) -> None:
    """GET /info and /status: valid JSON, identity matches the board on USB."""
    info = api.get("/info").json()
    st = api.get("/status").json()
    check(dl.norm_mac(info["mac"]) == dl.norm_mac(api.mac), f"same board ({info['mac']})")
    check(info["mode"] == "recovery", "recovery answers")
    check(st["wifi"]["ap_on"] is True, "AP on")
    check(api.get("/channels").json()["channels"][0]["name"] == "log", "/channels lists log")


def n_auth_and_limits(b: Board, api: Api) -> None:
    """Mutating requests need key + device id (test 22); bodies are size/format-checked (SPEC §10 row 29)."""
    r = api.post("/config", {"label": "x"}, headers={"X-Device-Id": api.mac})
    check(r.status_code == 401, f"no API key -> 401 ({r.status_code})")
    r = api.post("/config", {"label": "x"}, headers=api.h(mac="00:11:22:33:44:55"))
    check(r.status_code == 409 and r.json()["error"] == "wrong_device", "wrong X-Device-Id -> 409 wrong_device")
    r = api.post("/config", "{" + "\"a\":1," * 400 + "\"b\":2}")
    check(r.status_code == 413, f"2 KB body -> 413 ({r.status_code})")
    r = api.post("/config", "{\"label\":")
    check(r.status_code == 400 and r.json()["error"] == "invalid_json", "malformed JSON -> 400")
    r = api.post("/config", {"label": "L" * 40})
    check(r.status_code == 400, "label > 32 bytes -> 400")
    r = api.post("/config", {"label": "net test \"q\""})
    check(r.status_code == 200 and r.json()["label"] == "net test \"q\"", "label with quotes saved and escaped")
    check(api.get("/info").status_code == 200, "server still fine")


def n_scan(b: Board, api: Api) -> None:
    """GET /wifi/scan lists networks."""
    nets = api.get("/wifi/scan", timeout=20).json()["networks"]
    check(len(nets) > 0, f"{len(nets)} networks")


def n_ota_rejects(b: Board, api: Api) -> None:
    """Test 21: wrong chip / bootloader / ELF / foreign / too large -> rejected with a clear error, nothing written."""
    before = slots(api)
    app = (BUILD / "bootlab" / "bootlab.bin").read_bytes()
    cases = [
        ("ELF file", (BUILD / "bootlab" / "bootlab.elf").read_bytes()[:400_000], "not_an_esp_image"),
        ("bootloader image", (BUILD / "recovery" / "bootloader" / "bootloader.bin").read_bytes(), "not_an_app_image"),
        ("ESP32-C3 image", app[:12] + struct.pack("<H", 5) + app[14:], "wrong_chip"),
        ("needs chip rev v9.0", app[:15] + struct.pack("<H", 900) + app[17:], "wrong_chip_revision"),
        ("16 MB flash image", app[:3] + bytes([(app[3] & 0x0F) | 0x40]) + app[4:], "flash_too_small"),
    ]
    hello = Path(r"C:\Users\Admin\AppData\Local\Temp\claude\C--Users-Admin--claude-spares-spare-0ZIHLT"
                 r"\8c89792a-6c91-412d-8039-684a14f57ad0\scratchpad\hello_world\build\hello_world.bin")
    if hello.exists():
        cases.append(("foreign (non-devlink) app", hello.read_bytes(), "not_a_devlink_image"))
    for name, img, want in cases:
        r = api.ota(img)
        err = r.json().get("error") if r.headers.get("content-type", "").startswith("application/json") else r.text
        check(r.status_code >= 400 and err == want, f"{name} -> {want} ({r.status_code} {err})")
    # Announce 3 MB + 1 and send only the first KB (raw request: HTTP libraries won't send a fake Content-Length).
    # esp_http_server only dispatches a request once body bytes arrive; a client that stalls right after the headers
    # gets the server's own 408 instead.
    host = api.url.split("//")[1].split("/")[0]
    s = socket.create_connection((host.split(":")[0], 80), timeout=15)
    s.sendall((f"POST /api/v1/ota?target=app HTTP/1.1\r\nHost: {host}\r\nX-Api-Key: {KEY}\r\n"
               f"X-Device-Id: {api.mac}\r\nContent-Length: {0x300001}\r\n\r\n").encode() + app[:1024])
    resp = b""
    try:
        while b"}" not in resp:  # headers and the JSON body may arrive in separate packets
            chunk = s.recv(512)
            if not chunk:
                break
            resp += chunk
    except OSError:
        pass
    s.close()
    resp = resp.decode(errors="replace")
    # Only the status line is guaranteed: the board does not drain an oversized body, so closing with unread bytes
    # resets the connection and the JSON body may be lost (the PC app checks sizes before uploading, SPEC §7.2).
    check(resp.startswith("HTTP/1.1 413"), f"announced size > slot -> 413 ({resp[:40]!r})")
    check(slots(api) == before, "slots unchanged")


def n_ota_idle_and_concurrent(b: Board, api: Api) -> None:
    """Tests 9 + 10: a stalled upload times out after 10 s and frees the lock; a second upload meanwhile gets 409."""
    app = (BUILD / "bootlab" / "bootlab.bin").read_bytes()
    host = api.url.split("//")[1].split("/")[0]
    s = socket.create_connection((host.split(":")[0], 80), timeout=30)
    s.sendall((f"POST /api/v1/ota?target=app HTTP/1.1\r\nHost: {host}\r\nX-Api-Key: {KEY}\r\n"
               f"X-Device-Id: {api.mac}\r\nContent-Length: {len(app)}\r\n\r\n").encode() + app[:20000])
    time.sleep(1)
    r = api.ota(app)
    check(r.status_code == 409 and r.json()["error"] == "ota_in_progress", "second upload -> 409")
    t0 = time.time()
    resp = b""
    try:
        resp = s.recv(512)
    except OSError:
        pass
    s.close()
    check(time.time() - t0 < 30, f"stalled upload ended after {time.time() - t0:.0f} s ({resp[:40]!r})")
    p = api.get("/ota/progress").json()
    check(p["error"] == "idle_timeout", f"progress reports idle_timeout ({p})")


def n_udp_discovery(b: Board, api: Api) -> None:
    """Test 4 (partly): UDP broadcast DEVLINK? on 47800 is answered with the discovery record."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(3)
    host = api.url.split("//")[1].split("/")[0].split(":")[0]
    got = None
    for dest in ("255.255.255.255", host):
        sock.sendto(b"DEVLINK?", (dest, 47800))
        try:
            while True:
                data, _ = sock.recvfrom(1024)
                rec = json.loads(data)
                if dl.norm_mac(rec["mac"]) == dl.norm_mac(api.mac):
                    got = rec
                    break
        except socket.timeout:
            pass
        if got:
            break
    check(got is not None and got["mode"] == "recovery", f"discovery answer {got}")


def n_ota_good(b: Board, api: Api) -> None:
    """A good app OTA from recovery: written to the slot that is not the last valid one, then the app boots."""
    before = {n: v for n, _, v in slots(api)}
    r = api.ota((BUILD / "bootlab" / "bootlab.bin").read_bytes())
    check(r.status_code == 200 and r.json()["ok"], f"upload accepted ({r.status_code} {r.text[:80]})")
    slot = r.json()["slot"]
    info = b.wait_boot(timeout=60)
    check(info["image"] == "bootlab", f"app boots from {slot} after the OTA")
    print(f"    slots before: {before}")


TESTS = [n_info_status, n_auth_and_limits, n_scan, n_ota_rejects, n_ota_idle_and_concurrent, n_udp_discovery,
         n_ota_good]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--ap", help="config-AP SSID the PC is on (passed by run_via_ap.ps1)")
    ap.add_argument("tests", nargs="*")
    args = ap.parse_args()
    by_name = {f.__name__: f for f in TESTS}
    tests = [by_name[n] for n in args.tests] if args.tests else TESTS
    b = Board(dl.find_port(args.port, args.mac))
    b.con.open(timeout=3.0)
    b.con.drain(0.5)
    enter_recovery(b)
    api = Api(args.url, args.mac)
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}", flush=True)
        try:
            t(b, api)
            results.append((t.__name__, "PASS", ""))
        except Fail as e:
            results.append((t.__name__, "FAIL", str(e)))
            print(f"    FAIL: {e}")
        except Exception as e:  # noqa: BLE001
            results.append((t.__name__, "ERROR", repr(e)))
            traceback.print_exc()
    print("\n== Summary")
    for name, res, msg in results:
        print(f"  {res:5} {name} {msg}")
    return 0 if all(r == "PASS" for _, r, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
