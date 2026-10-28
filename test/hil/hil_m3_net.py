"""Milestone-3 tests: devlink in the app (SPEC §3.5): self-test, OTA cycles, rollback of broken apps (tests 2, 5, 6, 7).

Runs with the PC on the APP's config AP (the bench board has no Wi-Fi credentials, so the app keeps its AP open).
The app restarts after every OTA, which drops the PC from the AP: the test rejoins it each time. Boot results are
read over USB. Build first: build/bootlab and build/bootlab_{panic,hang,noapi} (see CLAUDE.md).
  powershell -File test/hil/run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X -Target app -Script hil_m3_net.py
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
import traceback
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import devlink_tools as dl  # noqa: E402
from hil_m1 import Board, Fail, check  # noqa: E402
from hil_m2_net import Api  # noqa: E402

BUILD = dl.ROOT / "build"


def wifi_ip() -> str:
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-NetIPAddress -InterfaceAlias Wi-Fi -AddressFamily IPv4 -ErrorAction "
                          "SilentlyContinue).IPAddress"], capture_output=True, text=True).stdout
    return out.strip()


def wlan_scan() -> None:
    """Ask Windows for a fresh Wi-Fi scan (WlanScan). Without it a restarted AP can stay missing from the scan cache
    for a minute and 'netsh wlan connect' fails. Read-only: no profile is changed."""
    import ctypes
    import ctypes.wintypes as wt

    class GUID(ctypes.Structure):
        _fields_ = [("d1", wt.DWORD), ("d2", wt.WORD), ("d3", wt.WORD), ("d4", ctypes.c_ubyte * 8)]

    class IfInfo(ctypes.Structure):
        _fields_ = [("guid", GUID), ("desc", ctypes.c_wchar * 256), ("state", ctypes.c_uint)]

    class IfList(ctypes.Structure):
        _fields_ = [("n", wt.DWORD), ("index", wt.DWORD), ("items", IfInfo * 1)]

    wlan = ctypes.windll.wlanapi
    h = wt.HANDLE()
    ver = wt.DWORD()
    if wlan.WlanOpenHandle(2, None, ctypes.byref(ver), ctypes.byref(h)) != 0:
        return
    lst = ctypes.POINTER(IfList)()
    if wlan.WlanEnumInterfaces(h, None, ctypes.byref(lst)) == 0:
        items = (IfInfo * lst.contents.n).from_address(ctypes.addressof(lst.contents.items))
        for it in items:
            wlan.WlanScan(h, ctypes.byref(it.guid), None, None, None)
        wlan.WlanFreeMemory(lst)
    wlan.WlanCloseHandle(h, None)


def connect(ap: str) -> None:
    subprocess.run(["netsh", "wlan", "disconnect"], capture_output=True)
    wlan_scan()
    time.sleep(4)  # scan results arrive within a few seconds
    r = subprocess.run(["netsh", "wlan", "connect", f"name={ap}", f"ssid={ap}"], capture_output=True, text=True)
    print(f"    rejoin: netsh connect -> {r.stdout.strip() or r.stderr.strip()}")
    time.sleep(4)


def rejoin(ap: str, timeout: float = 90) -> None:
    """After the board restarted its AP: connect again and wait for an address and the API. Windows may keep a stale
    192.168.4.x address for a while after the AP restarted, so a silent API also forces a reconnect."""
    end = time.time() + timeout
    silent_since = None
    while time.time() < end:
        ip = wifi_ip()
        if not ip.startswith("192.168.4."):
            print(f"    rejoin: Wi-Fi address '{ip}'")
            connect(ap)
            silent_since = None
            continue
        try:
            if requests.get("http://192.168.4.1/api/v1/info", timeout=3).status_code == 200:
                return
        except requests.RequestException:
            pass
        silent_since = silent_since or time.time()
        if time.time() - silent_since > 10:
            print(f"    rejoin: address {ip} but the API is silent for 10 s")
            connect(ap)
            silent_since = None
            continue
        time.sleep(1)
    raise Fail(f"could not rejoin {ap} within {timeout:.0f} s")


def slot_of(info: dict) -> str:
    return {"0x1a0000": "ota_0", "0x4a0000": "ota_1"}.get(info["running"], info["running"])


def upload(api: Api, image: Path) -> dict:
    r = api.ota(image.read_bytes())
    if r.status_code != 200:
        raise Fail(f"upload of {image.parent.name} refused: {r.status_code} {r.text[:80]}")
    return r.json()


def a_selftest_valid(b: Board, api: Api, ap: str) -> None:
    """The running app passed its self-test: slot valid, AP up, still the same boot after the 90 s verify window."""
    info = api.get("/info").json()
    run = info["running"]
    state = {s["name"]: s["state"] for s in info["slots"]}[run]
    check(info["mode"] == "app" and state in ("valid", "undefined"), f"app in {run}, slot state {state}")
    usb = b.info()
    idx = usb["index"]
    wait = max(0, 95 - int(usb["uptime"]) // 1000)
    print(f"    waiting {wait} s past the 90 s RTC WDT window")
    time.sleep(wait)
    usb2 = b.info()
    check(usb2["index"] == idx, f"no reset after {int(usb2['uptime']) // 1000} s: RTC WDT was switched off")


def a_ota_cycles(b: Board, api: Api, ap: str, count: int = 6) -> None:
    """Test 2 (shortened): repeated OTAs alternate slots; each new app passes its self-test and stays."""
    img = BUILD / "bootlab" / "bootlab.bin"
    prev = slot_of(b.info())
    for i in range(count):
        r = upload(api, img)
        info = b.wait_boot(timeout=60)
        slot = slot_of(info)
        if info["image"] != "bootlab" or slot == prev or slot != r["slot"]:
            raise Fail(f"OTA {i + 1}: expected the app in {r['slot']}, got {info}")
        rejoin(ap)
        st = {s["name"]: s["state"] for s in api.get("/info").json()["slots"]}[slot]
        if st != "valid":
            time.sleep(3)
            st = {s["name"]: s["state"] for s in api.get("/info").json()["slots"]}[slot]
        if st != "valid":
            raise Fail(f"OTA {i + 1}: new app in {slot} not marked valid ({st})")
        print(f"    OTA {i + 1}: {prev} -> {slot}, valid")
        prev = slot
    check(True, f"{count} OTAs, slots alternated, every new app valid")


def rollback_case(b: Board, api: Api, ap: str, variant: str, timeout: float) -> None:
    before = b.info()
    old_slot = slot_of(before)
    r = upload(api, BUILD / f"bootlab_{variant}" / "bootlab.bin")
    print(f"    uploaded bootlab-fault-{variant} to {r['slot']}, waiting for the rollback (up to {timeout:.0f} s)")
    end = time.time() + timeout
    info = None
    while time.time() < end:
        try:
            info = b.wait_boot(timeout=max(1, end - time.time()))
        except Fail:
            break
        if info["reason"] == "rollback":
            break
        print(f"    boot: {info['image']} {slot_of(info)} reason={info['reason']}")
    check(info is not None and info["reason"] == "rollback", "boot reason rollback")
    check(slot_of(info) == old_slot, f"previous app back in {old_slot}")
    rejoin(ap)
    ji = api.get("/info").json()
    check(ji["rejected_version"] == f"bootlab-fault-{variant}", f"rejected version reported ({ji['rejected_version']})")
    states = {s["name"]: s["state"] for s in ji["slots"]}
    check(states[r["slot"]] in ("aborted", "invalid"), f"{r['slot']} marked {states[r['slot']]}")


def a_rollback_panic(b: Board, api: Api, ap: str) -> None:
    """Test 5: a new app that panics at start is rolled back."""
    rollback_case(b, api, ap, "panic", 60)


def a_rollback_noapi(b: Board, api: Api, ap: str) -> None:
    """Test 7: a new app that never starts its API is rolled back after the 60 s self-test."""
    rollback_case(b, api, ap, "noapi", 110)


def a_rollback_hang(b: Board, api: Api, ap: str) -> None:
    """Test 6: a new app that hangs (all of app_main blocked, task WDT quiet) is reset by the RTC WDT and rolled back."""
    rollback_case(b, api, ap, "hang", 140)


TESTS = [a_selftest_valid, a_ota_cycles, a_rollback_panic, a_rollback_noapi, a_rollback_hang]


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
    b.con.open(timeout=3.0)
    b.con.drain(0.5)
    api = Api(args.url, args.mac)
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}", flush=True)
        t0 = time.time()
        try:
            rejoin(args.ap)
            t(b, api, args.ap)
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
