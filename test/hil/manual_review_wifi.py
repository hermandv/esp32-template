"""Bench check of the Wi-Fi fixes from the v0.12 review (SPEC §5.4), board on the LAN running the current app.
ERASES dlcfg (factory reset), then sets Wi-Fi and the label again. The password is asked in a masked dialog.
  1. /factory-reset over the LAN: the reply arrives (the STA drops only 1 s later)
  2. Improv over USB with the right password: connected, URL returned (waits for the connection, up to 40 s)
  3. LAN change to a wrong password: "pending", revert_after_s 45, back on the old network with wifi_change_failed
  4. label set again
  pc\\.venv\\Scripts\\python -u test/hil/manual_review_wifi.py --mac 7c:df:a1:f2:50:30 --ssid 10X --label "test esp"
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from devlink_pc import discovery, improv, usbflash  # noqa: E402
from devlink_pc.api import DeviceApi  # noqa: E402
from hil_m1 import Fail, check  # noqa: E402
from manual_m8_usb import ask_password  # noqa: E402


def find(mac: str, timeout: float) -> discovery.Seen:
    end = time.time() + timeout
    while time.time() < end:
        s = discovery.find_by_mac(mac, timeout=5)
        if s is not None:
            try:
                if DeviceApi(s.ip, mac).status()["wifi"]["sta_connected"]:
                    return s
            except Exception:  # noqa: BLE001
                pass
        time.sleep(2)
    raise Fail(f"{mac} not connected on the LAN within {timeout:.0f} s")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mac", required=True)
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--label", default="")
    args = ap.parse_args()
    mac = discovery.norm_mac(args.mac)
    pw = ask_password(args.ssid)
    port = next(p for p, desc in usbflash.usb_ports() if mac in desc)
    try:
        s = find(mac, 30)
        print(f"\n== 1. factory reset over the LAN ({s.ip})", flush=True)
        r = DeviceApi(s.ip, mac).factory_reset()
        check(r.get("ok") is True, f"reply arrived: {r}")
        time.sleep(5)

        print("\n== 2. Improv over USB", flush=True)
        t0 = time.time()
        with improv.ImprovPort(port) as p:
            url = p.provision(args.ssid, pw)
        check(url.startswith("http://") and "0.0.0.0" not in url, f"connected in {time.time() - t0:.1f} s: {url}")
        s = find(mac, 60)

        print("\n== 3. LAN change to a wrong password", flush=True)
        r = DeviceApi(s.ip, mac).set_wifi(args.ssid, "wrong-password-123")
        check(r.get("mode") == "pending" and r.get("revert_after_s") == 45, f"pending: {r}")
        t0 = time.time()
        time.sleep(50)
        s = find(mac, 40)
        st = DeviceApi(s.ip, mac).status()
        check(st["last_error"] == "wifi_change_failed",
              f"reverted, back on the LAN {time.time() - t0:.0f} s after the change ({st['last_error']})")

        print("\n== 4. label", flush=True)
        if args.label:
            DeviceApi(s.ip, mac).set_config(label=args.label)
        info = DeviceApi(s.ip, mac).info()
        check(info["label"] == args.label, f"label '{info['label']}', {info['fw']['version']}")
        print("\nALL PASSED")
        return 0
    except Fail as e:
        print(f"  FAIL: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
