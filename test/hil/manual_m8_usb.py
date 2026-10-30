"""Milestone-8 bench test of "Program blank board (USB)" (usbflash, SPEC §3.8): ERASES the board.
  1. cancel in the middle of the write -> the board is left without a working image (no devlink answer)
  2. program recovery + app with a label and Wi-Fi -> the board is back on the LAN under that label
The Wi-Fi password is asked in a masked dialog and never stored.
  pc\\.venv\\Scripts\\python -u test/hil/manual_m8_usb.py --mac 7c:df:a1:f2:50:30 --ssid 10X --label "test esp"
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc"))
from devlink_pc import discovery, usbflash  # noqa: E402
from devlink_pc.api import DeviceApi  # noqa: E402


def ask_password(ssid: str) -> str:
    from PySide6.QtWidgets import QApplication, QInputDialog, QLineEdit  # noqa: PLC0415
    app = QApplication.instance() or QApplication([])
    pw, ok = QInputDialog.getText(None, "Wi-Fi password", f"Password for '{ssid}' (not stored):", QLineEdit.Password)
    del app
    if not ok:
        sys.exit("no password given")
    return pw


def log(line: str) -> None:
    if "Writing at" in line or "Verifying" in line:  # esptool progress: every 10th line is enough
        log.n = getattr(log, "n", 0) + 1
        if log.n % 10:
            return
    print("  |", line, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mac", required=True)
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--label", default="")
    ap.add_argument("--build", default=str(ROOT / "build"))
    args = ap.parse_args()
    mac = discovery.norm_mac(args.mac)
    pw = ask_password(args.ssid)
    port = next(p for p, desc in usbflash.usb_ports() if mac in desc)
    builds = {b.dir.name: b for b in usbflash.scan_builds(args.build)}
    fw = usbflash.make_firmware(builds["recovery"], builds["app"])
    assert not fw.errors, fw.errors
    print(f"port {port}; {fw.summary}", flush=True)
    ok = True

    print("\n== 1. cancel during the write", flush=True)
    cancel = threading.Event()
    result: dict = {}

    def run():
        try:
            usbflash.program(port, fw, watch, cancel=cancel)
            result["r"] = "finished"
        except usbflash.Cancelled as e:
            result["r"] = f"cancelled: {e}"
        except Exception as e:  # noqa: BLE001
            result["r"] = f"error: {e!r}"

    first_write: list[float] = []

    def watch(line: str) -> None:
        log(line)
        if "Writing at" in line and not cancel.is_set():
            first_write.append(time.time())
            if time.time() - first_write[0] > 3.0:  # well into the write
                print(f"  -> cancel: {line}", flush=True)
                cancel.set()

    t0 = time.time()
    th = threading.Thread(target=run)
    th.start()
    th.join(300)
    print(f"  result: {result.get('r')} after {time.time() - t0:.1f} s", flush=True)
    ok &= str(result.get("r", "")).startswith("cancelled")
    time.sleep(2)
    bc = usbflash.probe_board(port, lambda _l: None)
    print(f"  board now: {bc.summary}", flush=True)
    ok &= bc.info is None and bc.mac == mac
    print("  PASS" if ok else "  FAIL", flush=True)

    print("\n== 2. program with label and Wi-Fi", flush=True)
    t0 = time.time()
    info = usbflash.program(port, fw, log, label=args.label)
    url = usbflash.set_wifi(port, args.ssid, pw, log)
    t_prog = time.time() - t0
    print(f"  programmed and on Wi-Fi in {t_prog:.0f} s: {url}", flush=True)
    s = discovery.find_by_mac(mac, timeout=60)
    lan = DeviceApi(s.ip, mac).info() if s else {}
    good = (s is not None and lan.get("label") == args.label and lan.get("mode") == "app"
            and lan.get("fw", {}).get("version") == fw.app.version and info.get("mac") == mac)
    print(f"  on the LAN: {s.ip if s else '-'}, label '{lan.get('label')}', {lan.get('mode')} "
          f"{lan.get('fw', {}).get('project')} {lan.get('fw', {}).get('version')}, recovery "
          f"{lan.get('recovery', {}).get('version') if isinstance(lan.get('recovery'), dict) else lan.get('recovery')}",
          flush=True)
    print("  PASS" if good else "  FAIL", flush=True)
    return 0 if ok and good else 1


if __name__ == "__main__":
    sys.exit(main())
