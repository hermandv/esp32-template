"""Milestone-2 hardware-in-the-loop tests over USB: the recovery image (SPEC §6), Improv serial (§5.4), console.

Wi-Fi tests that need the PC on the board's network (HTTP API, OTA, discovery) are in hil_m2_net.py.
Run with ESP-IDF active, after tools/flash_all.py:
  python test/hil/hil_m2.py --mac 7c:df:a1:f2:50:30 [test ...]
"""

from __future__ import annotations

import argparse
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import devlink_tools as dl  # noqa: E402
from hil_m1 import Board, Fail, check  # noqa: E402


def enter_recovery(b: Board) -> dict:
    info = b.info()
    if info["image"] != "recovery":
        info = b.reboot_with("recovery")
    check(info["image"] == "recovery", "in recovery")
    time.sleep(2)  # Wi-Fi + HTTP up
    return info


def t_recovery_services(b: Board) -> None:
    """Recovery starts Wi-Fi with an always-on AP, reports identity/versions, partition table OK."""
    enter_recovery(b)
    info = b.con.devlink_json("info")
    st = b.con.devlink_json("status")
    check(info is not None and st is not None, "devlink info/status answer with JSON")
    check(info["mode"] == "recovery" and info["api"] == 1, "mode recovery, API 1")
    check(info["fw"]["project"] == "devlink_recovery", f"fw {info['fw']['project']} {info['fw']['version']}")
    check(info["bootloader"]["devlink_version"] == 2, "bootloader descriptor says devlink bootloader v2")
    check(info["partitions_ok"] is True, "partition table matches the frozen layout")
    check(info["chip"]["flash_size"] == 8 * 1024 * 1024, "8 MB flash")
    check(info["chip"]["psram_size"] == 0, "recovery runs without PSRAM")
    check(info["hostname"] == "esp-" + info["id"].lower(), f"hostname {info['hostname']}")
    check(st["wifi"]["ap_on"] is True, "config AP is on")
    check(st["wifi"]["tx_power_dbm"] in (8, 11), f"reduced TX power ({st['wifi']['tx_power_dbm']} dBm)")
    check(st["last_error"] is None, f"no error ({st['last_error']})")


def t_improv(b: Board) -> None:
    """Improv serial: state, device info, scan, and error answers."""
    enter_recovery(b)
    info = b.con.devlink_json("info")
    b.con.close()
    b.con.open()

    fr = b.con.improv(dl.IMPROV_CMD_STATE)
    check(any(t == dl.IMPROV_STATE for t, _ in fr), f"state frame ({fr})")
    state = [d[0] for t, d in fr if t == dl.IMPROV_STATE][0]
    check(state in (2, 4), f"state {'ready' if state == 2 else 'provisioned'}")

    fr = b.con.improv(dl.IMPROV_CMD_INFO)
    res = [d for t, d in fr if t == dl.IMPROV_RPC_RESULT]
    check(len(res) == 1, "device info result")
    s = dl.improv_strings(res[0])
    check(s[0] == "devlink_recovery" and s[1] == info["fw"]["version"] and s[2] == "ESP32-S3",
          f"info strings {s}")

    fr = b.con.improv(dl.IMPROV_CMD_SCAN, timeout=15, until_empty=True)
    nets = [dl.improv_strings(d) for t, d in fr if t == dl.IMPROV_RPC_RESULT and d[1] > 0]
    check(len(fr) > 0 and fr[-1][0] == dl.IMPROV_RPC_RESULT and fr[-1][1][1] == 0, "scan list ends with an empty result")
    print(f"    scan: {len(nets)} networks: {', '.join(n[0] + ' ' + n[1] for n in nets[:6])}")
    check(all(len(n) == 3 and n[2] in ("YES", "NO") for n in nets), "each network: ssid, rssi, auth")

    fr = b.con.improv(dl.IMPROV_CMD_WIFI, bytes([5, ord("a")]))  # malformed settings
    check(fr and fr[-1] == (dl.IMPROV_ERROR, bytes([1])), "malformed Wi-Fi settings -> invalid RPC error")
    fr = b.con.improv(0x09)
    check(fr and fr[-1] == (dl.IMPROV_ERROR, bytes([2])), "unknown command -> unknown RPC error")
    # Bad checksum.
    bad = bytearray(dl.improv_frame(dl.IMPROV_RPC, bytes([2, 0])))
    bad[-1] ^= 1
    b.con.ser.reset_input_buffer()
    b.con.ser.write(bytes(bad))
    time.sleep(0.5)
    fr, _ = dl.improv_parse(b.con.ser.read(512))
    check((dl.IMPROV_ERROR, bytes([1])) in fr, "bad checksum -> invalid RPC error")


def t_console_label(b: Board) -> None:
    """'devlink label' saves the friendly name; it shows in info and survives a restart."""
    enter_recovery(b)
    label = f"rec {int(time.time()) % 10000}"
    m = b.con.command(f"devlink label {label}", r"devlink: label (\S+)")
    check(m is not None and m.group(1) == "saved", "label saved")
    check(b.con.devlink_json("info")["label"] == label, "info shows the label")
    info = b.reboot_with("recovery")
    check(b.con.devlink_json("info")["label"] == label, "label kept across a restart")


def t_health_keeps_running(b: Board, seconds: int = 75) -> None:
    """The RTC WDT stays on in recovery (30 s) and the health task keeps it fed: no reset for 75 s."""
    info = enter_recovery(b)
    idx = info["index"]
    time.sleep(seconds)
    info2 = b.info()
    check(info2["index"] == idx and int(info2["uptime"]) > seconds * 1000, f"still the same boot after {seconds} s "
          f"(uptime {int(info2['uptime']) // 1000} s)")
    st = b.con.devlink_json("status")
    check(st["unconfirmed_boots"] <= 2, f"unconfirmed boots before this one: {st['unconfirmed_boots']}")


def t_app_console_and_back(b: Board) -> None:
    """devlink console lines work in the app too; 'devlink restart' from recovery returns to the app."""
    enter_recovery(b)
    b.con.buf = ""
    b.con.send("devlink restart")
    info = b.wait_boot()
    check(info["image"] == "bootlab", "planned restart in recovery -> app")
    ji = b.con.devlink_json("info")
    check(ji is not None and ji["mode"] == "app" and ji["fw"]["project"] == "bootlab", "app answers 'devlink info'")
    check(ji["boot"]["reason"] == "normal", "boot reason normal")


TESTS = [t_recovery_services, t_improv, t_console_label, t_health_keeps_running, t_app_console_and_back]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac")
    ap.add_argument("tests", nargs="*")
    args = ap.parse_args()
    by_name = {f.__name__: f for f in TESTS}
    tests = [by_name[n] for n in args.tests] if args.tests else TESTS
    b = Board(dl.find_port(args.port, args.mac))
    b.con.open(timeout=3.0)
    b.con.drain(0.5)
    results = []
    for t in tests:
        print(f"\n== {t.__name__}: {(t.__doc__ or '').strip().splitlines()[0]}", flush=True)
        t0 = time.time()
        try:
            t(b)
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
