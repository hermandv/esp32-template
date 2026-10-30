"""Milestone-5 tests: the PC app's library against the real board (SPEC §8.1, §7.2; tests 1, 4, 16a, 24).

The bench Wi-Fi password is asked in a masked dialog and only passed to the board; never stored or logged.
Run from pc/.venv with ESP-IDF active (for esptool):
  pc\\.venv\\Scripts\\python test/hil/hil_m5_pc.py --mac 7c:df:a1:f2:50:30 --ssid 10X_Guest --home 10X
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc"))
sys.path.insert(0, str(ROOT / "tools"))

import devlink_tools as dl  # noqa: E402
from devlink_pc import configure, discovery, improv, ota, wifi_win  # noqa: E402
from devlink_pc.api import DeviceApi  # noqa: E402


class Fail(Exception):
    pass


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise Fail(msg)
    print(f"    ok: {msg}", flush=True)


def ask_password(ssid: str) -> str:
    from PySide6.QtWidgets import QApplication, QInputDialog, QLineEdit  # noqa: PLC0415

    app = QApplication.instance() or QApplication([])
    pw, ok = QInputDialog.getText(None, "devlink HIL test", f"Wi-Fi password for '{ssid}' (sent to the board only):",
                                  QLineEdit.Password)
    app.processEvents()
    if not ok:
        raise SystemExit("no password given")
    return pw


def esptool(port: str, *args: str) -> None:
    py = Path(os.environ["IDF_PYTHON_ENV_PATH"]) / "Scripts" / "python.exe"
    r = subprocess.run([str(py), "-m", "esptool", "--chip", "esp32s3", "--port", port, *args], capture_output=True,
                       text=True)
    if r.returncode != 0:
        raise Fail(f"esptool {' '.join(args)}: {r.stdout[-300:]}")


def forget_wifi(port: str) -> None:
    """Back to 'no credentials' (AP only): erase dlcfg over USB, reboot."""
    esptool(port, "--after", "hard-reset", "erase-region", "0x12000", "0x2000")
    time.sleep(8)


class Ctx:
    def __init__(self, a, pw):
        self.mac, self.ssid, self.home, self.pw = discovery.norm_mac(a.mac), a.ssid, a.home, pw
        self.port = dl.find_port(a.port, a.mac)
        self.ip = ""
        self.reachable = False
        self.mdns = discovery.MdnsBrowser()


def p_usb_find(c: Ctx) -> None:
    """USB: the board answers Improv 'device info' (SPEC §8.1 USB fallback)."""
    boards = improv.find_boards()
    b = [x for x in boards if x.port == c.port]
    check(len(b) == 1, f"{c.port} answers Improv: {b[0].firmware} {b[0].version} '{b[0].name}'" if b else "found")


def p_usb_configure(c: Ctx) -> None:
    """Test 16a (PC app part): configure Wi-Fi + name over USB with Improv; the board joins the bench network."""
    forget_wifi(c.port)
    r = configure.configure_via_usb(c.port, c.ssid, c.pw, "bench S3 #1", say=lambda m: print(f"    > {m}"))
    print(f"    result: {r.message}")
    check(bool(r.ip), f"board joined '{c.ssid}' with address {r.ip}")
    c.ip, c.reachable = r.ip, r.ok
    print(f"    {'REACHABLE' if r.ok else 'NOT REACHABLE'} from this PC: {r.message}")


def p_ap_configure(c: Ctx) -> None:
    """Configure flow via the config AP (SPEC §8.1): join the AP, send settings, back to the home network."""
    forget_wifi(c.port)
    ap = f"ESP32S3-CFG-{c.mac.replace(':', '')[-6:]}"
    r = configure.configure_via_ap(ap, c.ssid, c.pw, "bench S3 #1", mdns=c.mdns, say=lambda m: print(f"    > {m}"),
                                   lan_wait_s=45)
    st = wifi_win.current()
    check(st.ssid == c.home, f"PC back on its own network '{st.ssid}'")
    check(r.mac == c.mac, f"settings were sent to the right board ({r.mac})")
    # Did the board itself join? Ask over USB (works even if this PC cannot reach the bench network).
    con = dl.Console(c.port, echo=False)
    con.open()
    s = None
    for _ in range(20):
        s = con.devlink_json("status")
        if s and s["wifi"]["sta_connected"]:
            break
        time.sleep(2)
    con.close()
    w = s["wifi"] if s else {}
    check(bool(w.get("sta_connected")) and w.get("ssid") == c.ssid,
          f"board on '{w.get('ssid')}' as {w.get('ip')} (RSSI {w.get('rssi')}), per its USB console")
    # Reachable from this PC = the configure flow found the board on the LAN by discovery.
    c.reachable = r.ok
    if r.ok:
        c.ip = r.ip
    print(f"    {'REACHABLE' if r.ok else 'NOT REACHABLE'} from this PC: {r.message}")


def p_discovery(c: Ctx) -> None:
    """Test 4 (partly): the board is found by UDP broadcast and by mDNS, keyed by MAC."""
    if not c.reachable:
        raise Fail("skipped: the PC cannot reach the board's network")
    udp = [s for s in discovery.udp_scan(2.0) if s.mac == c.mac]
    check(bool(udp), f"UDP broadcast finds it at {udp[0].ip if udp else '-'}")
    end = time.time() + 15
    while time.time() < end and not [s for s in c.mdns.snapshot() if s.mac == c.mac]:
        time.sleep(1)
    m = [s for s in c.mdns.snapshot() if s.mac == c.mac]
    check(bool(m), f"mDNS finds it ({m[0].fields.get('label') if m else '-'})")


def p_lan_ota(c: Ctx) -> None:
    """SPEC §7.2 over the LAN: rediscover by MAC, upload, wait for the board, verify (app valid, reason normal)."""
    if not c.reachable:
        raise Fail("skipped: the PC cannot reach the board's network")
    data = (ROOT / "build" / "bootlab" / "bootlab.bin").read_bytes()
    r = ota.update_device(c.mac, data, "devlink-bench", hint_ip=c.ip, mdns=c.mdns,
                          progress=lambda m, ph, d, t: None)
    check(r.ok, f"OTA: {r.state} - {r.message}")


def p_safe_wifi_change(c: Ctx) -> None:
    """Test 24: change Wi-Fi over the LAN to a wrong password -> the board reverts within 45 s, still on the LAN."""
    if not c.reachable:
        raise Fail("skipped: the PC cannot reach the board's network")
    api = DeviceApi(c.ip, c.mac)
    r = api.set_wifi(c.ssid, "wrong-password-123")
    check(r.get("mode") == "pending", f"accepted as pending ({r})")
    time.sleep(60)  # 1 s + the 45 s window + reconnecting with the old settings
    s = discovery.find_by_mac(c.mac, timeout=30, mdns=c.mdns, hint_ip=c.ip)
    check(s is not None, f"board back on the LAN at {s.ip if s else '-'}")
    st = DeviceApi(s.ip, c.mac).status()
    check(st["wifi"]["sta_connected"] and st["last_error"] == "wifi_change_failed",
          f"reverted, reports {st['last_error']}")


def p_cleanup(c: Ctx) -> None:
    """Back to AP-only (no credentials) so the AP-based suites keep working."""
    forget_wifi(c.port)
    check(True, "dlcfg erased, board in AP mode")


TESTS = [p_usb_find, p_usb_configure, p_ap_configure, p_discovery, p_lan_ota, p_safe_wifi_change, p_cleanup]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac", required=True)
    ap.add_argument("--ssid", required=True, help="bench Wi-Fi the board should join")
    ap.add_argument("--home", required=True, help="this PC's own Wi-Fi profile")
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    by_name = {f.__name__: f for f in TESTS}
    tests = [by_name[n] for n in a.tests] if a.tests else TESTS
    c = Ctx(a, ask_password(a.ssid))
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}", flush=True)
        try:
            t(c)
            results.append((t.__name__, "PASS", ""))
        except Fail as e:
            results.append((t.__name__, "SKIP" if str(e).startswith("skipped") else "FAIL", str(e)))
            print(f"    {results[-1][1]}: {e}")
        except Exception as e:  # noqa: BLE001
            results.append((t.__name__, "ERROR", repr(e)))
            traceback.print_exc()
    c.mdns.close()
    if wifi_win.current().ssid != a.home:
        wifi_win.connect(a.home)
    print("\n== Summary")
    for name, res, msg in results:
        print(f"  {res:5} {name} {msg}")
    return 0 if all(r in ("PASS", "SKIP") for _, r, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
