"""Milestone-6 tests via the app's config AP: Monitor session with auto-reconnect (SPEC §8.3), recovery update +
self-test (SPEC §7.3, test 23 parts). Results of the self-test boot are read over USB (the AP changes while the board
runs recovery). Build first: build/recovery, build/recovery_v2, build/recovery_crash (see CLAUDE.md).
  powershell -File test/hil/run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X -Target app -Script hil_m6_net.py
(run_via_ap.ps1 uses the IDF Python; this script needs pc/.venv for the PC library: it re-launches itself there.)
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
VENV_PY = ROOT / "pc" / ".venv" / "Scripts" / "python.exe"
if Path(sys.executable).resolve() != VENV_PY.resolve() and VENV_PY.exists() and "DEVLINK_NO_REEXEC" not in os.environ:
    os.environ["DEVLINK_NO_REEXEC"] = "1"
    sys.exit(subprocess.call([str(VENV_PY), "-u", __file__, *sys.argv[1:]]))

sys.path.insert(0, str(ROOT / "pc"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import devlink_tools as dl  # noqa: E402
from devlink_pc import image  # noqa: E402
from devlink_pc.api import ApiError, DeviceApi  # noqa: E402
from devlink_pc.discovery import norm_mac  # noqa: E402
from devlink_pc.stream import MonitorSession  # noqa: E402
from hil_m1 import Board, Fail, check  # noqa: E402
from hil_m3_net import rejoin  # noqa: E402

AP_IP = "192.168.4.1"
BUILD = ROOT / "build"


def frames_of(session: MonitorSession, ch: str, seconds: float) -> list[dict]:
    out, end = [], time.time() + seconds
    while time.time() < end:
        out += [f for f in session.drain() if f.get("ch") == ch]
        time.sleep(0.1)
    return out


def m_session_reconnects(b: Board, api: DeviceApi, ap: str) -> None:
    """Monitor session: frames at the requested rate; after a board reboot it reconnects and re-subscribes."""
    s = MonitorSession(api.mac, AP_IP, retry_s=2)
    try:
        end = time.time() + 10
        while s.state != "connected" and time.time() < end:
            time.sleep(0.2)
        check(s.state == "connected", "session connected")
        s.subscribe("sys.temp", 5)
        s.subscribe("log")
        n = len(frames_of(s, "sys.temp", 4))
        check(15 <= n <= 25, f"sys.temp at 5 Hz: {n} frames in 4 s")
        api.reboot("app")
        b.wait_boot(timeout=40)
        rejoin(ap)  # the AP restarted with the board
        end = time.time() + 60
        while time.time() < end and not (s.state == "connected" and frames_of(s, "sys.temp", 1)):
            time.sleep(0.5)
        check(s.reconnects >= 1 and s.state == "connected", f"reconnected after the reboot ({s.reconnects} attempts)")
        n = len(frames_of(s, "sys.temp", 4))
        check(15 <= n <= 25, f"subscription and rate restored: {n} sys.temp frames in 4 s")
    finally:
        s.close()


def recovery_update(b: Board, api: DeviceApi, ap: str, image_path: Path) -> dict:
    """Upload a recovery through the app, follow the self-test boot over USB, return the app's /info afterwards."""
    data = image_path.read_bytes()
    r = api.ota(data, target="recovery", force=True)
    check(r.get("selftest") == "running", f"recovery {r.get('version')} written to factory, self-test boot started")
    end = time.time() + 120
    seen_recovery = False
    while time.time() < end:
        try:
            info = b.wait_boot(timeout=max(1, end - time.time()))
        except Fail:
            break
        if info["image"] == "recovery":
            seen_recovery = True
        elif info["image"] == "bootlab":
            break
    ji = b.con.devlink_json("info")
    rejoin(ap)
    return {"info": ji, "seen_recovery": seen_recovery, "version": image.parse(data).version}


def r_update_ok(b: Board, api: DeviceApi, ap: str) -> None:
    """Recovery update with a good image: written, self-test boot ran in recovery, result ok, app back."""
    res = recovery_update(b, api, ap, BUILD / "recovery_v2" / "devlink_recovery.bin")
    ji = res["info"]
    check(res["seen_recovery"], "the board booted the new recovery once (self-test boot)")
    check(ji["mode"] == "app", "back in the app")
    check(ji["recovery"]["version"] == res["version"], f"recovery is now {ji['recovery']['version']}")
    check(ji["recovery_selftest"] == {"state": "ok", "version": res["version"]}, f"self-test {ji['recovery_selftest']}")


def r_update_selftest_fails(b: Board, api: DeviceApi, ap: str) -> None:
    """Test 23 (part): a recovery that crashes at start fails its self-test; the app keeps running and reports it."""
    res = recovery_update(b, api, ap, BUILD / "recovery_crash" / "devlink_recovery.bin")
    ji = res["info"]
    check(ji["mode"] == "app", "the app runs again (recovery crashed, the one-shot request was used up)")
    check(ji["recovery_selftest"] == {"state": "failed", "version": res["version"]},
          f"self-test reported failed ({ji['recovery_selftest']})")


def r_restore_good(b: Board, api: DeviceApi, ap: str) -> None:
    """Test 23 (part): re-flashing a good recovery after a failed self-test works."""
    res = recovery_update(b, api, ap, BUILD / "recovery" / "devlink_recovery.bin")
    ji = res["info"]
    check(ji["recovery_selftest"] == {"state": "ok", "version": res["version"]}, f"self-test {ji['recovery_selftest']}")
    info = b.reboot_with("recovery")
    check(info["image"] == "recovery", "the restored recovery is reachable")
    b.checkpoint()
    rejoin(ap)


def r_rejects(b: Board, api: DeviceApi, ap: str) -> None:
    """An app image is not accepted as recovery (unless forced); nothing is written."""
    before = b.con.devlink_json("info")["recovery"]["version"]
    try:
        api.ota((BUILD / "bootlab" / "bootlab.bin").read_bytes(), target="recovery")
        raise Fail("app image accepted as recovery")
    except ApiError as e:
        check(e.error == "not_a_recovery_image", f"refused: {e.error}")
    check(b.con.devlink_json("info")["recovery"]["version"] == before, "factory unchanged")


TESTS = [m_session_reconnects, r_rejects, r_update_ok, r_update_selftest_fails, r_restore_good]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac", required=True)
    ap.add_argument("--url", default="http://192.168.4.1")
    ap.add_argument("--ap", required=True)
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    by_name = {f.__name__: f for f in TESTS}
    tests = [by_name[n] for n in a.tests] if a.tests else TESTS
    b = Board(dl.find_port(a.port, a.mac))
    b.con.open(timeout=3.0)
    b.con.drain(0.5)
    api = DeviceApi(AP_IP, norm_mac(a.mac))
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}", flush=True)
        t0 = time.time()
        try:
            rejoin(a.ap)
            t(b, api, a.ap)
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
