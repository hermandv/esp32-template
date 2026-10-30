"""Milestone-7 acceptance tests that need hands on the board (SPEC §9 tests 8, 12, 14). Each step shows a dialog
with what to do; the script checks the result over USB and the LAN. The board must be on the LAN (10X) with bootlab.

Run from a PowerShell with ESP-IDF active (esptool for clearing bootlab's NVS):
  pc\\.venv\\Scripts\\python -u test/hil/manual_m7.py --mac 7c:df:a1:f2:50:30 [test ...]
"""

from __future__ import annotations

import argparse
import io
import sys
import threading
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import requests  # noqa: E402

import devlink_tools as dl  # noqa: E402
from devlink_pc import discovery  # noqa: E402
from devlink_pc.api import DeviceApi  # noqa: E402
from hil_m1 import Board, Fail, check  # noqa: E402

BUILD = ROOT / "build"


def operator(text: str) -> None:
    """Show the step in a dialog and wait for OK."""
    from PySide6.QtWidgets import QApplication, QMessageBox  # noqa: PLC0415

    print(f"\n    OPERATOR: {text}", flush=True)
    app = QApplication.instance() or QApplication([])
    box = QMessageBox(QMessageBox.Information, "devlink manual test", text, QMessageBox.Ok)
    box.setWindowFlag(box.windowFlags().WindowStaysOnTopHint, True)
    box.exec()
    app.processEvents()


class PortWatch:
    """Records whether the USB port disappeared (power cut / reset) while a step was shown."""

    def __init__(self, port: str):
        self.port, self.gone, self._stop = port, False, threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        while not self._stop.is_set():
            if not any(p.device == self.port for p in dl.list_ports.comports()):
                self.gone = True
            time.sleep(0.05)

    def stop(self) -> bool:
        self._stop.set()
        return self.gone


class TrickleUpload(io.RawIOBase):
    """OTA body that goes at full speed up to `stop_at` (fraction), then 64 bytes every 2 s (below the board's 20 s
    idle timeout), so the board is mid-write for as long as the operator needs to cut the power."""

    def __init__(self, data: bytes, stop_at: float):
        self.data, self.pos, self.limit = data, 0, int(len(data) * stop_at)
        self.reached = threading.Event()

    def __len__(self) -> int:
        return len(self.data)

    def tell(self) -> int:
        return self.pos

    def readable(self) -> bool:
        return True

    def read(self, n: int = -1) -> bytes:
        if self.pos >= self.limit:
            self.reached.set()
            time.sleep(2)
            n = 64
        else:
            n = min(8192, self.limit - self.pos)
        chunk = self.data[self.pos:self.pos + n]
        self.pos += len(chunk)
        return chunk


class Ctx:
    def __init__(self, a):
        self.mac = discovery.norm_mac(a.mac)
        self.port = dl.find_port(a.port, a.mac)
        self.b = Board(self.port)
        self.ip = ""

    def lan(self, timeout: float = 90) -> dict:
        end = time.time() + timeout
        while time.time() < end:
            s = discovery.find_by_mac(self.mac, timeout=5, hint_ip=self.ip or None)
            if s is None:
                continue
            try:
                info = DeviceApi(s.ip, self.mac).info()
            except Exception:  # noqa: BLE001
                continue
            self.ip = s.ip
            return info
        raise Fail(f"board not on the LAN within {timeout:.0f} s")

    def usb_info(self, timeout: float = 60) -> dict:
        """devlink info JSON over USB once the board answers again after a reset."""
        end = time.time() + timeout
        self.b.con.close()
        while time.time() < end:
            try:
                self.b.con.open(timeout=2)
                self.b.con.drain(1.0)
                j = self.b.con.devlink_json("info", timeout=3)
                if j:
                    return j
            except Exception:  # noqa: BLE001
                pass
            self.b.con.close()
            time.sleep(1)
        raise Fail("board not answering over USB")


# ------------------------------------------------------------------------------------------------------------------


def m08_power_cut_during_ota(c: Ctx) -> None:
    """Test 8: power cut at 10 %, 50 %, 99 % of an OTA, and right after the reboot into the new image -> the old app
    boots or the new one verifies; never stuck; the next OTA succeeds."""
    data = (BUILD / "bootlab" / "bootlab.bin").read_bytes()
    for pct in (10, 50, 99):
        before = c.lan()
        body = TrickleUpload(data, pct / 100)
        api = DeviceApi(c.ip, c.mac)
        headers = {**api._headers(), "Content-Type": "application/octet-stream", "Content-Length": str(len(data))}

        def upload(body=body, headers=headers):
            try:
                requests.post(f"{api.base}/ota?target=app", data=body, headers=headers, timeout=(10, 600))
            except requests.RequestException:
                pass  # the board loses power mid-upload

        threading.Thread(target=upload, daemon=True).start()
        if not body.reached.wait(120):
            raise Fail(f"upload did not reach {pct} %")
        prog = api.ota_progress()
        print(f"    upload held at {pct} %: board reports {prog}", flush=True)
        w = PortWatch(c.port)
        operator(f"Power cut at {pct} % of an OTA.\n\nThe board is in the middle of an upload. Unplug the USB cable "
                 f"now, wait 2 s, plug it back in, then click OK.")
        check(w.stop(), f"{pct} %: USB port went away (power was cut)")
        j = c.usb_info()
        check(j["mode"] == "app" and j["running"] == before["running"],
              f"{pct} %: old app boots ({j['running']}, boot reason {j['boot']['reason']})")
        after = c.lan()
        check(after["fw"]["version"] == before["fw"]["version"], f"{pct} %: back on the LAN at {c.ip}")



def m08b_power_cut_after_reboot(c: Ctx) -> None:
    """Test 8 (last part): power cut right after the reboot into a new image that has not verified yet -> the old
    app boots; the next OTA succeeds. An image that never verifies (no API) keeps that window open for 60 s."""
    data = (BUILD / "bootlab" / "bootlab.bin").read_bytes()
    before = c.lan()
    noapi = (BUILD / "bootlab_noapi" / "bootlab.bin").read_bytes()
    DeviceApi(c.ip, c.mac).ota(noapi)
    if c.b.con.ser is None:
        c.b.con.open()
    info = c.b.wait_boot(timeout=60)
    check(info["image"] == "bootlab", f"new image booted (running {info.get('running')}), not yet verified")
    w = PortWatch(c.port)
    operator("Power cut right after the reboot into the new image.\n\nUnplug the USB cable NOW (within 40 s), wait 2 s, "
             "plug it back in, then click OK.")
    check(w.stop(), "USB port went away (power was cut)")
    j = c.usb_info(90)
    check(j["mode"] == "app" and j["running"] == before["running"],
          f"unverified new image not kept: old app runs ({j['running']}, boot reason {j['boot']['reason']})")
    c.lan()
    from devlink_pc import ota  # noqa: PLC0415

    r = ota.update_device(c.mac, data, "devlink-bench", hint_ip=c.ip)
    check(r.ok, f"next OTA succeeds: {r.message} ({r.slot})")


def m12_reset_presses(c: Ctx) -> None:
    """Test 12 (reset-button part): app that deep-sleeps forever -> 3 reset-button presses -> recovery."""
    b = c.b
    if b.con.ser is None:
        b.con.open()
    b.ensure_app_normal()
    b.cmd("mode sleep")
    info = b.checkpoint()
    check(info["image"] == "bootlab", "hostile app (deep sleep forever, 1 s after boot) running")
    operator("Press the board's RESET (EN / RST) button 3 times, about 2 s apart, then click OK.\n\n"
             "(The app goes to sleep 1 s after each boot; after the third press the board should start recovery.)")
    time.sleep(3)
    j = c.usb_info(60)
    check(j["mode"] == "recovery" and j["boot"]["reason"] == "quick_reset",
          f"recovery, reason {j['boot']['reason']} (records {j['boot'].get('records')})")
    time.sleep(6)  # past ALIVE_5S in recovery
    b.esptool("erase-region", "0x9000", "0x6000")  # clear bootlab's "mode sleep"
    info = b.wait_boot()
    check(info["image"] == "bootlab", "app back after clearing its NVS")
    b.checkpoint()


def m14_button(c: Ctx) -> None:
    """Test 14 (button part): BOOT held 3 s -> config AP; held 10 s -> recovery; from recovery a restart -> app."""
    b = c.b
    if b.con.ser is None:
        b.con.open()
    b.ensure_app_normal()
    end = time.time() + 60
    while time.time() < end:
        st = b.con.devlink_json("status") or {}
        if st.get("wifi", {}).get("sta_connected") and not st["wifi"].get("ap_on"):
            break
        time.sleep(3)
    check(not st["wifi"]["ap_on"], "app on 10X, config AP off")
    operator("Hold the BOOT button for about 4 s, then release it and click OK.")
    st = b.con.devlink_json("status")
    check(st["wifi"]["ap_on"], "3 s hold: config AP open")
    operator("Now hold the BOOT button for about 12 s (until the board restarts), release it, then click OK.")
    j = c.usb_info(60)
    check(j["mode"] == "recovery" and j["boot"]["reason"] == "requested",
          f"10 s hold: recovery (reason {j['boot']['reason']})")
    time.sleep(6)
    info = b.checkpoint()
    check(info["image"] == "bootlab", "restart from recovery -> app")


TESTS = [m14_button, m12_reset_presses, m08_power_cut_during_ota, m08b_power_cut_after_reboot]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac", required=True)
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
    print("\n== Summary")
    for name, res, msg in results:
        print(f"  {res:5} {name} {msg}")
    print("MANUAL DONE", flush=True)
    return 0 if all(r == "PASS" for _, r, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
