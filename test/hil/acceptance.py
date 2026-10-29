"""Milestone-7 acceptance run (SPEC §9): the tests not already proven by the milestone suites, on the bench board
with the PC on its own network (10X). Asks the bench Wi-Fi password once (masked dialog; kept in memory only).

Run from a PowerShell with ESP-IDF active (esptool, flash_all.py):
  pc\\.venv\\Scripts\\python -u test/hil/acceptance.py --mac 7c:df:a1:f2:50:30 --ssid 10X --home 10X [test ...]
Long tests (a02: 50 OTAs, a03: 1 h streaming) are part of the default list; pass names to run a subset.
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import devlink_tools as dl  # noqa: E402
from devlink_pc import configure, discovery, ota, wifi_win  # noqa: E402
from devlink_pc.api import ApiError, DeviceApi  # noqa: E402
from devlink_pc.stream import MonitorSession  # noqa: E402
from hil_m1 import Board, Fail, check  # noqa: E402

BUILD = ROOT / "build"
# Foreign image (test 13): the IDF hello_world example, built with
#   idf.py -C $env:IDF_PATH/examples/get-started/hello_world -B build/hello_world set-target esp32s3 build
FOREIGN = BUILD / "hello_world" / "hello_world.bin"


def ask_password(ssid: str) -> str:
    from PySide6.QtWidgets import QApplication, QInputDialog, QLineEdit  # noqa: PLC0415

    app = QApplication.instance() or QApplication([])
    pw, ok = QInputDialog.getText(None, "devlink acceptance run",
                                  f"Wi-Fi password for '{ssid}' (sent to the board only, kept in memory):",
                                  QLineEdit.Password)
    app.processEvents()
    if not ok:
        raise SystemExit("no password given")
    return pw


class Ctx:
    def __init__(self, a):
        self.mac, self.ssid, self.home, self._pw = discovery.norm_mac(a.mac), a.ssid, a.home, ""
        self.port = dl.find_port(a.port, a.mac)
        self.b = Board(self.port)
        self.mdns = discovery.MdnsBrowser()
        self.ip = ""

    @property
    def pw(self) -> str:
        """Asked on first use only (test 26 needs no Wi-Fi)."""
        if not self._pw:
            self._pw = ask_password(self.ssid)
        return self._pw

    def esptool(self, *args: str) -> None:
        self.b.con.close()
        r = subprocess.run([*dl.esptool_cmd(), "--chip", "esp32s3", "--port", self.port, *args],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail(f"esptool {' '.join(args)}: {r.stdout[-300:]}{r.stderr[-300:]}")

    def lan(self, timeout: float = 60, mode: str | None = None) -> dict:
        """Find the board on the LAN (optionally in a given mode) and return its /info."""
        end = time.time() + timeout
        while time.time() < end:
            s = discovery.find_by_mac(self.mac, timeout=5, mdns=self.mdns, hint_ip=self.ip or None)
            if s is None:
                continue
            try:
                info = DeviceApi(s.ip, self.mac).info()
            except Exception:  # noqa: BLE001
                continue
            if mode is None or info["mode"] == mode:
                self.ip = s.ip
                return info
            time.sleep(2)
        raise Fail(f"board not on the LAN{' in ' + mode if mode else ''} within {timeout:.0f} s")

    def api(self) -> DeviceApi:
        return DeviceApi(self.ip, self.mac)

    def on_10x_via_usb(self) -> None:
        r = configure.configure_via_usb(self.port, self.ssid, self.pw, "bench S3 #1")
        if not r.ok:
            raise Fail(f"could not put the board on {self.ssid}: {r.message}")
        self.ip = r.ip


# ------------------------------------------------------------------------------------------------------------------


def a26_dangerous_writes(c: Ctx) -> None:
    """Test 26: writing the bootloader region aborts; an eFuse burn only changes the virtual copy."""
    b = c.b
    b.con.open()
    b.ensure_app_normal()
    info = b.reboot_with("danger_bootloader", timeout=40)
    check(info["image"] == "bootlab", "board rebooted after the write attempt: the bootloader is intact")
    j = b.con.devlink_json("info")
    check(j["reset_reasons"][0] == 4, f"reset reason panic (abort), ring {j['reset_reasons'][:3]}")
    c.esptool("--after", "hard-reset", "verify-flash", "0x0", str(BUILD / "recovery" / "bootloader" / "bootloader.bin"))
    check(True, "bootloader in flash still matches the build (esptool verify)")
    b.con.open()
    b.wait_boot()
    m = b.con.command("efuse_burn", r"DLT ok efuse_burn [^\n]*")
    check(m is not None and "read_back=0xA5" in m.group(0), f"burn reported done in the virtual copy ({m.group(0) if m else '-'})")
    b.checkpoint()
    m = b.con.command("efuse_read", r"DLT ok efuse_read [^\n]*")
    check(m is not None and "0x00" in m.group(0), f"after a restart the real eFuse is untouched ({m.group(0) if m else '-'})")
    b.con.close()


def a01_fresh_board(c: Ctx) -> None:
    """Test 1: fresh board (flash_all) -> AP only -> configure from the PC app -> on the LAN with its label in <= 30 s."""
    c.b.con.close()
    r = subprocess.run([dl.esptool_cmd()[0], str(ROOT / "tools" / "flash_all.py"), "--port", c.port, "--no-build", "--yes"],
                       capture_output=True, text=True)
    check(r.returncode == 0 and "OK:" in r.stdout, "flash_all.py: erased, written, verified, board reports")
    end = time.time() + 40
    while time.time() < end and not any(dev == c.mac.replace(":", "")[-6:].upper() for _, _, dev in wifi_win.device_aps()):
        time.sleep(2)
    ap = f"ESP32S3-CFG-{c.mac.replace(':', '')[-6:].upper()}"
    check(any(s == ap for s, _, _ in wifi_win.device_aps(fresh=False)), f"no credentials -> config AP {ap} visible")
    marks = {}
    t0 = time.time()
    res = configure.configure_via_ap(ap, c.ssid, c.pw, "acceptance 01", mdns=c.mdns,
                                     say=lambda m: marks.setdefault(m.split()[0], time.time()), lan_wait_s=45)
    check(res.ok, f"configured: {res.message}")
    sent = marks.get("settings", t0)
    took = time.time() - sent
    c.ip = res.ip
    info = c.api().info()
    check(info["label"] == "acceptance 01", "label set")
    check(took <= 30, f"on the LAN {took:.1f} s after the settings were sent (incl. the PC rejoining {c.home})")


def a04_udp_without_mdns(c: Ctx) -> None:
    """Test 4: with mDNS out of the picture the board is still found by UDP broadcast."""
    found = [s for s in discovery.udp_scan(2.0) if s.mac == c.mac]
    check(bool(found), f"UDP broadcast alone finds it at {found[0].ip if found else '-'}")
    s = discovery.find_by_mac(c.mac, timeout=5, mdns=None)
    check(s is not None, "find_by_mac without mDNS works (OTA rediscovery path)")


def a15_nvs_garbage(c: Ctx) -> None:
    """Test 15: garbage in the app's NVS -> app and recovery still get their Wi-Fi config (dlcfg) and come up."""
    garbage = bytes(random.getrandbits(8) for _ in range(0x6000))
    with tempfile.TemporaryDirectory() as tmp:
        g = Path(tmp) / "nvs.bin"
        g.write_bytes(garbage)
        c.esptool("--after", "hard-reset", "write-flash", "0x9000", str(g))
    info = c.lan(60, mode="app")
    check(True, f"app back on the LAN at {c.ip} with NVS full of garbage")
    st = c.api().status()
    check(st["wifi"]["sta_connected"] and st["wifi"]["ssid"] == c.ssid, "app Wi-Fi from dlcfg works")
    c.api().reboot("recovery")
    info = c.lan(60, mode="recovery")
    check(info["mode"] == "recovery", "recovery on the LAN as well (no NVS dependency)")
    c.api().reboot("app")
    c.lan(60, mode="app")


def a13_foreign_image(c: Ctx) -> None:
    """Test 13: a foreign (non-devlink) image. Over OTA (force=1) it never marks itself valid -> rolled back. Flashed as
    the valid app (esptool) it restarts without ALIVE marks -> quick-reset escape -> recovery; restore from there."""
    if not FOREIGN.exists():
        raise Fail(f"build the IDF hello_world example first ({FOREIGN}, see FOREIGN in this script)")
    data = FOREIGN.read_bytes()
    c.lan(60, mode="app")
    try:
        c.api().ota(data, force=False)
        raise Fail("foreign image accepted without force")
    except ApiError as e:
        check(e.error == "not_a_devlink_image", f"without force: refused ({e.error})")
    before = c.api().info()
    c.api().ota(data, force=True)
    end = time.time() + 120
    info = None
    while time.time() < end:
        try:
            info = c.lan(30, mode="app")
        except Fail:
            continue
        if info["boot"]["reason"] == "rollback":
            break
    check(info is not None and info["boot"]["reason"] == "rollback" and info["running"] == before["running"],
          f"OTA'd foreign image rolled back (rejected '{info and info.get('rejected_version')}')")

    slot_addr = {"ota_0": "0x1a0000", "ota_1": "0x4a0000"}[info["running"]]
    c.esptool("--after", "hard-reset", "write-flash", "--flash-mode", "dio", "--flash-size", "8MB", slot_addr, str(FOREIGN))
    t0 = time.time()
    info = c.lan(150, mode="recovery")
    check(info["boot"]["reason"] == "quick_reset", f"recovery after {time.time() - t0:.0f} s, reason {info['boot']['reason']}")
    r = ota.update_device(c.mac, (BUILD / "bootlab" / "bootlab.bin").read_bytes(), "devlink-bench", hint_ip=c.ip,
                          mdns=c.mdns)
    check(r.ok, f"app restored from recovery over the LAN: {r.message}")


def a25_wrong_password(c: Ctx) -> None:
    """Test 25: saved password wrong -> config AP; a good app OTA through the AP is NOT rolled back; configuring
    through the PC app brings it back on the LAN."""
    with tempfile.TemporaryDirectory() as tmp:
        f = Path(tmp) / "dlcfg.bin"
        f.write_bytes(dl.dlcfg_image(dl.dlcfg_payload(c.ssid, "wrong-password-25", "bench S3 #1"), sequence=1000))
        c.esptool("--after", "hard-reset", "write-flash", "0x12000", str(f))
    check(True, "saved password replaced by a wrong one (dlcfg written)")
    ap = f"ESP32S3-CFG-{c.mac.replace(':', '')[-6:].upper()}"
    time.sleep(40)  # boot + STA failing for 30 s -> the AP opens
    before_profile = wifi_win.current().profile
    try:
        check(wifi_win.join_temp(ap, "configure"), f"PC joined {ap}")
        api = DeviceApi("192.168.4.1", c.mac)
        before = api.info()
        res = api.ota((BUILD / "bootlab" / "bootlab.bin").read_bytes())
        new_slot = res["slot"]
        end = time.time() + 120
        info = None
        while time.time() < end:
            time.sleep(4)
            if not wifi_win.ipv4().startswith("192.168.4."):
                wifi_win.join_temp(ap, "configure", timeout=15)
            try:
                info = api.info()
            except Exception:  # noqa: BLE001
                continue
            states = {s["name"]: s["state"] for s in info["slots"]}
            if info["running"] == new_slot and states.get(new_slot) == "valid":
                break
        check(info is not None and info["running"] == new_slot and info["boot"]["reason"] != "rollback",
              f"new app valid in {new_slot} on the AP path, no rollback (before: {before['running']})")
    finally:
        wifi_win.remove_temp(ap)
        wifi_win.connect(before_profile or c.home)
    res = configure.configure_via_ap(ap, c.ssid, c.pw, "bench S3 #1", mdns=c.mdns, lan_wait_s=45)
    check(res.ok, f"configured from the PC app: {res.message}")
    c.ip = res.ip


def a02_many_otas(c: Ctx, count: int = 50) -> None:
    """Test 2: 50 OTAs in a row from the PC app (LAN), alternating slots, no failures."""
    data = (BUILD / "bootlab" / "bootlab.bin").read_bytes()
    c.lan(60, mode="app")
    t0 = time.time()
    prev = c.api().info()["running"]
    for i in range(count):
        r = ota.update_device(c.mac, data, "devlink-bench", hint_ip=c.ip, mdns=c.mdns)
        if not r.ok or r.slot == prev:
            raise Fail(f"OTA {i + 1}: {r.state} {r.message} (slot {r.slot}, before {prev})")
        prev = r.slot
        print(f"    OTA {i + 1}/{count}: {r.slot} valid ({time.time() - t0:.0f} s)", flush=True)
    check(True, f"{count} OTAs, slots alternated, all valid, {(time.time() - t0) / count:.0f} s each")


def a03_stream_one_hour(c: Ctx, seconds: int = 3600) -> None:
    """Test 3: sys.temp at 10 Hz + log for 1 h without a disconnect (one session, no reconnects)."""
    c.lan(30, mode="app")
    s = MonitorSession(c.mac, c.ip, c.mdns)
    try:
        end = time.time() + 15
        while s.state != "connected" and time.time() < end:
            time.sleep(0.2)
        check(s.state == "connected", "connected")
        s.subscribe("sys.temp", 10)
        s.subscribe("log")
        n, last_ts, worst_gap, t0 = 0, None, 0.0, time.time()
        while time.time() - t0 < seconds:
            for f in s.drain():
                if f.get("ch") == "sys.temp" and "ts" in f:
                    n += 1
                    if last_ts is not None:
                        worst_gap = max(worst_gap, (f["ts"] - last_ts) / 1e6)
                    last_ts = f["ts"]
            time.sleep(0.2)
            if int(time.time() - t0) % 600 == 0:
                print(f"    {int(time.time() - t0)} s: {n} frames, worst gap {worst_gap:.2f} s, "
                      f"reconnects {s.reconnects}", flush=True)
                time.sleep(1)
        st = c.api().status()
        check(s.reconnects == 0, "no disconnect in the whole run")
        check(n >= seconds * 10 * 0.97, f"{n} sys.temp frames in {seconds} s (expected ~{seconds * 10})")
        check(worst_gap < 2.0, f"longest gap {worst_gap:.2f} s; board drops {st['drops']}, PC drops {s.lost_frames}")
    finally:
        s.close()


def a99_cleanup(c: Ctx) -> None:
    """Back to AP-only (no credentials), app in place."""
    c.esptool("--after", "hard-reset", "erase-region", "0x12000", "0x2000")
    check(True, "dlcfg erased, board in AP mode")


TESTS = [a26_dangerous_writes, a01_fresh_board, a04_udp_without_mdns, a15_nvs_garbage, a13_foreign_image,
         a25_wrong_password, a02_many_otas, a03_stream_one_hour, a99_cleanup]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac", required=True)
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--home", required=True)
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    by_name = {f.__name__: f for f in TESTS}
    tests = [by_name[n] for n in a.tests] if a.tests else TESTS
    c = Ctx(a)
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}  [{time.strftime('%H:%M:%S')}]", flush=True)
        t0 = time.time()
        try:
            t(c)
            results.append((t.__name__, "PASS", ""))
        except Fail as e:
            results.append((t.__name__, "FAIL", str(e)))
            print(f"    FAIL: {e}")
        except Exception as e:  # noqa: BLE001
            results.append((t.__name__, "ERROR", repr(e)))
            traceback.print_exc()
        print(f"   ({time.time() - t0:.0f} s)", flush=True)
        if wifi_win.current().ssid != a.home:
            wifi_win.connect(a.home)
    c.mdns.close()
    print("\n== Summary")
    for name, res, msg in results:
        print(f"  {res:5} {name} {msg}")
    print("ACCEPTANCE DONE", flush=True)
    return 0 if all(r == "PASS" for _, r, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
