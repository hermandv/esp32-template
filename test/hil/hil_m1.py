"""Milestone-1 hardware-in-the-loop tests (SPEC §9: 11, 12, 13, 13a, 13b, 13c, 14 (part), 16, 17, 18, 20).

The board must have been flashed with tools/flash_all.py (recovery + bootlab). Run with ESP-IDF active:
  py -3.12 test/hil/hil_m1.py [--mac 7c:df:a1:f2:50:30] [test ...]       (default: all automatic tests)
  py -3.12 test/hil/hil_m1.py manual_quick_power_cycles                    (needs an operator)

Tests marked "manual" print OPERATOR instructions and wait for the board's answer on serial: real power cycles
cannot be automated on this bench (an esptool/RTS reset is a chip reset, not a power cycle).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import devlink_tools as dl  # noqa: E402

OTA0 = 0x1A0000
OTA1 = 0x4A0000
FACTORY = 0x20000
SLOT_SIZE = 0x300000
BUILD = dl.ROOT / "build"


class Fail(Exception):
    pass


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise Fail(msg)
    print(f"    ok: {msg}")


class Board:
    def __init__(self, port: str):
        self.port = port
        self.con = dl.Console(port, echo=False)

    # -- serial --
    def wait_boot(self, timeout: float = 30.0) -> dict:
        """Wait for the next boot banner; returns its info dict."""
        m = self.con.wait_for(r"DLT ready image=\S+[^\n]*\n", timeout)
        if not m:
            raise Fail(f"no boot within {timeout:.0f} s")
        m = self.con.wait_for(r"DLT info ([^\n]*)\n", 5)
        if not m:
            raise Fail("no info line after boot")
        info = dl.parse_kv(m.group(1))
        print(f"    boot: {info.get('image')} reason={info.get('reason')} recent={info.get('recent')}")
        return info

    def cmd(self, line: str, timeout: float = 5.0) -> str:
        m = self.con.command(line, timeout=timeout)
        if not m:
            raise Fail(f"no reply to '{line}'")
        return m.group(0).strip()

    def reboot_with(self, line: str, timeout: float = 30.0) -> dict:
        self.con.buf = ""
        self.con.send(line)
        return self.wait_boot(timeout)

    def info(self) -> dict:
        i = self.con.info()
        if i is None:
            raise Fail("no reply to 'info'")
        return i

    def wait_replug(self, timeout: float) -> dict:
        """Wait for a power cycle (USB port disappears and comes back), then ask the board for its boot info.
        The boot banner itself is lost: the board prints it before Windows has re-opened the port."""
        end = time.time() + timeout
        self.con.close()
        gone = False
        while time.time() < end:
            present = any(p.device == self.port for p in dl.list_ports.comports())
            if not present:
                gone = True
            elif gone:
                time.sleep(0.5)
                info = self.con.info(timeout=2.0)
                if info:
                    print(f"    boot (after power cycle): {info.get('image')} reason={info.get('reason')} "
                          f"rtc={info.get('rtc')} recent={info.get('recent')}")
                    return info
            time.sleep(0.1)
        raise Fail(f"no power cycle seen within {timeout:.0f} s")

    def checkpoint(self) -> dict:
        """Planned restart: the current record gets CLEAN_RESTART, which breaks any unconfirmed chain."""
        return self.reboot_with("restart")

    def ensure_app_normal(self) -> dict:
        info = self.info()
        if info["image"] != "bootlab":
            info = self.checkpoint()  # recovery -> planned restart -> app
        if info["image"] == "bootlab":
            self.cmd("mode normal")
        return self.checkpoint()

    # -- esptool --
    def esptool(self, *args: str, reset: bool = True) -> None:
        self.con.close()
        cmd = [*dl.esptool_cmd(), "--chip", "esp32s3", "--port", self.port,
               "--after", "hard-reset" if reset else "no-reset", *args]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail(f"esptool {' '.join(args)} failed:\n{r.stdout[-800:]}{r.stderr[-800:]}")

    def write(self, addr: int, path: Path) -> None:
        self.esptool("write-flash", "--flash-mode", "dio", "--flash-size", "8MB", hex(addr), str(path))


# ------------------------------------------------------------------------------------------------------------------
# Automatic tests
# ------------------------------------------------------------------------------------------------------------------


def t_boot_info(b: Board) -> None:
    """RTC hand-over works and the app reports a normal boot."""
    info = b.ensure_app_normal()
    check(info["rtc"] == "1", "bootloader handed over a valid RTC block")
    check(info["reason"] == "normal", "planned restart -> reason normal")
    check(info["running"] == info["start"], "running partition = partition the bootloader chose")


def t_dlcfg_persists(b: Board) -> None:
    """dlcfg write survives a restart (test 16, logic part; power-cut part is manual)."""
    b.ensure_app_normal()
    label = f"hil {int(time.time()) % 100000}"
    b.cmd(f"cfg_label {label}")
    b.checkpoint()
    m = b.con.command("cfg_get", r"DLT cfg [^\n]*\n")
    check(m is not None and f'label="{label}"' in m.group(0), f"label '{label}' read back after restart")


def t_request_recovery(b: Board) -> None:
    """Rule 1: one-shot request -> recovery; planned restart -> back to the app."""
    b.ensure_app_normal()
    info = b.reboot_with("recovery")
    check(info["image"] == "recovery" and info["reason"] == "requested", "request -> recovery, reason requested")
    check(info["running"] == hex(FACTORY), "running factory")
    info = b.checkpoint()
    check(info["image"] == "bootlab" and info["reason"] == "normal", "restart in recovery -> app (one-shot)")


def t_quick_reset_by_crashes(b: Board) -> None:
    """Rule 2 (test 12 logic): three boots in a row that end < 5 s -> recovery."""
    b.ensure_app_normal()
    for i in range(3):
        info = b.reboot_with("crash")
        if i < 2:
            check(info["image"] == "bootlab", f"quick crash {i + 1} -> still app")
    check(info["image"] == "recovery" and info["reason"] == "quick_reset", "3rd quick end -> recovery, quick_reset")
    info = b.checkpoint()
    check(info["image"] == "bootlab" and info["reason"] == "normal", "planned restart in recovery -> app")


def t_planned_restarts(b: Board, count: int = 200) -> None:
    """Test 13b: many quick planned restarts never enter recovery."""
    b.ensure_app_normal()
    for i in range(count):
        info = b.checkpoint()
        if info["image"] != "bootlab" or info["reason"] != "normal":
            raise Fail(f"restart {i + 1}: {info}")
    check(True, f"{count} planned restarts, all normal app boots")


def t_crash_loop(b: Board, crash_s: int = 8) -> None:
    """Test 11: a valid app that crash-loops after ALIVE_5S -> recovery after 5 crashes."""
    b.ensure_app_normal()
    b.cmd(f"mode crash:{crash_s}")
    app_boots = 0
    info = b.checkpoint()
    while info["image"] == "bootlab":
        app_boots += 1
        if app_boots > 8:
            raise Fail("no recovery after 8 crashing boots")
        info = b.wait_boot(crash_s + 30)
    check(app_boots == 5, f"5 crashing app boots before recovery (saw {app_boots})")
    check(info["reason"] == "crash_loop", "reason crash_loop")
    # Recovery is fine; leave it with a planned restart, then stop the crashing mode before it fires again.
    info = b.checkpoint()
    check(info["image"] == "bootlab", "restart in recovery -> app")
    b.cmd("mode normal")
    b.checkpoint()


def t_deep_sleep(b: Board, period_s: int = 3, wakes: int = 20) -> None:
    """Test 13a (shortened): repeated deep-sleep wake-ups are not logged and never enter recovery."""
    b.ensure_app_normal()
    b.cmd(f"mode sleepwake:{period_s}:{wakes}")
    b.checkpoint()  # first boot of the series is logged (planned restart before it)
    before = int(b.info()["records"])
    # The USB port drops on every sleep and the boot banner is printed before Windows re-opens the port, so poll
    # the wake count instead; after the last wake the app stays awake.
    end = time.time() + wakes * (period_s + 2) + 60
    n = -1
    while time.time() < end:
        try:
            m = b.con.command("wakes", r"DLT ok wakes=(\d+)", timeout=1.0)
        except (OSError, dl.serial.SerialException):
            b.con.close()
            m = None
        if m:
            n = int(m.group(1))
            if n >= wakes:
                break
        time.sleep(0.5)
    check(n >= wakes, f"{wakes} deep-sleep wake-ups done (count {n})")
    info = b.info()
    check(info["image"] == "bootlab" and info["reason"] == "deep_sleep", "still the app, reason deep_sleep")
    check(int(info["records"]) == before, f"wake-ups added no boot records ({before} -> {info['records']})")
    b.cmd("mode normal")
    b.checkpoint()


def t_bootlog_rollover(b: Board) -> None:
    """Test 13c: sector rollover keeps the history; a crash loop across the rollover is still caught."""
    b.ensure_app_normal()
    records = int(b.info()["records"])
    # Fill with confirmed records, leaving 2 free bytes.
    b.cmd(f"log_inject F0 {4096 - records - 2}")
    info = b.checkpoint()
    for _ in range(4):
        info = b.checkpoint()
    check(info["reason"] == "normal", "planned restarts across the rollover stay normal")
    check(int(info["records"]) <= 16, f"sector rolled over (records now {info['records']})")
    # Crash loop straddling a second rollover.
    records = int(b.info()["records"])
    b.cmd(f"log_inject F0 {4096 - records - 3}")
    t_crash_loop(b)


def t_torn_records(b: Board) -> None:
    """Test 13c: torn records (partial bits) count as unconfirmed; two of them never force recovery."""
    b.ensure_app_normal()
    b.cmd("log_inject FD 1")  # torn: RECOVERY bit only
    b.cmd("log_inject FF 1")  # fully torn: nothing programmed (no record)
    info = b.reboot_with("crash")  # this boot ends quickly too: FD, (this one, quick) -> 2 quick
    check(info["image"] == "bootlab", "torn record + 1 quick boot -> still app")
    b.checkpoint()


def t_slot_corrupt(b: Board) -> None:
    """Test 18: one corrupted byte in the active slot -> the loader falls back (here: to recovery)."""
    b.ensure_app_normal()
    time.sleep(6)  # ALIVE_5S, so the boot interrupted by esptool is not a quick one
    with tempfile.TemporaryDirectory() as tmp:
        sec = Path(tmp) / "sector.bin"
        b.esptool("read-flash", hex(OTA0 + 0x1000), "0x1000", str(sec), reset=False)
        data = bytearray(sec.read_bytes())
        data[0x80] ^= 0x01
        sec.write_bytes(data)
        b.write(OTA0 + 0x1000, sec)
    info = b.wait_boot()
    check(info["image"] == "recovery", "corrupted ota_0 -> recovery runs")
    check(info["reason"] == "no_valid_app", "reason no_valid_app (loader fallback detected)")
    check(info["start"] == hex(OTA0) and info["running"] == hex(FACTORY), "bootloader chose ota_0, factory runs")
    time.sleep(6)
    b.write(OTA0, BUILD / "bootlab" / "bootlab.bin")
    info = b.wait_boot()
    check(info["image"] == "bootlab" and info["reason"] == "normal", "restored ota_0 boots normally")
    b.checkpoint()


def t_slots_erased(b: Board) -> None:
    """Test 17: both app slots erased -> recovery; an app written back boots."""
    b.ensure_app_normal()
    time.sleep(6)
    b.esptool("erase-region", hex(OTA0), hex(SLOT_SIZE), reset=False)
    b.esptool("erase-region", hex(OTA1), hex(SLOT_SIZE))
    info = b.wait_boot()
    check(info["image"] == "recovery" and info["reason"] == "no_valid_app", "no app -> recovery, no_valid_app")
    time.sleep(6)
    b.write(OTA0, BUILD / "bootlab" / "bootlab.bin")
    info = b.wait_boot()
    check(info["image"] == "bootlab", "app written back -> app boots")
    b.checkpoint()


def t_recovery_crashes(b: Board) -> None:
    """Test 20: recovery that crashes at start -> the bootloader falls back to the valid app (recovery_loop)."""
    crash_bin = BUILD / "recovery_crash" / "devlink_recovery.bin"
    good_bin = BUILD / "recovery" / "devlink_recovery.bin"
    if not crash_bin.exists():
        raise Fail(f"build the crash recovery first: {crash_bin}")
    b.ensure_app_normal()
    time.sleep(6)
    b.write(FACTORY, crash_bin)
    b.wait_boot()
    b.checkpoint()
    try:
        # Three quick ends -> recovery (crashes at start, no banner) x3 -> app with reason recovery_loop.
        for _ in range(2):
            b.reboot_with("crash")
        b.con.buf = ""
        b.con.send("crash")
        info = None
        recovery_crashes = 0
        for _ in range(8):
            m = b.con.wait_for(r"DLT (recovery TEST BUILD|info ([^\n]*)\n)", 40)
            if m is None:
                raise Fail("board went silent")
            if m.group(2):
                info = dl.parse_kv(m.group(2))
                break
            recovery_crashes += 1
            print("    recovery (crash build) started and crashed")
        check(recovery_crashes == 3, f"crashing recovery was started 3 times (saw {recovery_crashes})")
        check(info is not None and info["image"] == "bootlab", "valid app runs instead of the crashing recovery")
        check(info["reason"] == "recovery_loop", "reason recovery_loop")
    finally:
        time.sleep(6)
        b.write(FACTORY, good_bin)
        b.wait_boot()
        b.checkpoint()
    info = b.reboot_with("recovery")
    check(info["image"] == "recovery", "good recovery restored and reachable")
    b.checkpoint()


# ------------------------------------------------------------------------------------------------------------------
# Manual tests (operator power-cycles the board)
# ------------------------------------------------------------------------------------------------------------------


def operator(msg: str) -> None:
    print("\n" + "=" * 100 + f"\nOPERATOR: {msg}\n" + "=" * 100, flush=True)


def m_quick_power_cycles(b: Board) -> None:
    """Test 12: app that deep-sleeps forever -> 3 quick power cycles -> recovery; power-on RTC hand-over works."""
    try:
        b.ensure_app_normal()
        b.cmd("mode sleep")
        info = b.checkpoint()
        check(info["image"] == "bootlab", "hostile app (sleeps forever after 1 s) running")
    except Fail:
        print("    board not answering: the sleeping test app is already set (earlier run)")
    operator("Unplug the USB cable, wait 2 s, plug it back in. Do this 3 times, each plug-in within ~5 s of the "
             "previous one. The board should then come up in recovery.")
    end = time.time() + 300
    info = {}
    while time.time() < end and info.get("image") != "recovery":
        # While the hostile app runs, the port is up for ~1 s per boot; info only answers once recovery is up.
        try:
            info = b.wait_replug(timeout=max(1, end - time.time()))
        except Fail:
            break
    check(info.get("image") == "recovery" and info.get("reason") == "quick_reset", "recovery, reason quick_reset")
    check(info["rtc"] == "1", "valid RTC hand-over after a power-on reset")
    # The app would sleep again 1 s after boot: clear its NVS (mode) from here, then back to a normal app.
    time.sleep(6)
    b.esptool("erase-region", "0x9000", "0x6000")
    info = b.wait_boot()
    check(info["image"] == "bootlab", "app back after clearing its NVS")
    b.checkpoint()


def m_power_cycle_in_recovery(b: Board) -> None:
    """Test 14 (part): power cycle while in recovery (one-shot request) -> back to the app."""
    b.ensure_app_normal()
    info = b.reboot_with("recovery")
    check(info["image"] == "recovery", "in recovery (requested)")
    time.sleep(6)
    operator("Unplug the USB cable, wait 2 s, plug it back in (once).")
    info = b.wait_replug(timeout=300)
    check(info["image"] == "bootlab" and info["reason"] == "normal", "power cycle in recovery -> app")


def m_dlcfg_power_cut(b: Board, cuts: int = 5) -> None:
    """Test 16 (manual sample): power cut during continuous dlcfg writes -> config always valid."""
    b.ensure_app_normal()
    for i in range(cuts):
        b.con.buf = ""
        b.con.send("cfg_stress")
        if not b.con.wait_for(r"DLT cfg seq=\d+", 10):
            raise Fail("cfg_stress did not start")
        operator(f"Power cut {i + 1}/{cuts}: the board is writing dlcfg continuously. Unplug the USB cable NOW, "
                 "wait 2 s, plug it back in.")
        info = b.wait_replug(timeout=300)
        m = b.con.command("cfg_get", r"DLT cfg [^\n]*\n")
        check(m is not None and "err=ESP_OK" in m.group(0) and 'label="stress' in m.group(0),
              f"cut {i + 1}: config valid after power cut ({m.group(0).strip() if m else 'no reply'})")
        if info["image"] != "bootlab":
            b.checkpoint()
    b.checkpoint()


AUTO = [t_boot_info, t_dlcfg_persists, t_request_recovery, t_quick_reset_by_crashes, t_torn_records,
        t_crash_loop, t_deep_sleep, t_bootlog_rollover, t_slot_corrupt, t_slots_erased, t_recovery_crashes,
        t_planned_restarts]
MANUAL = [m_quick_power_cycles, m_power_cycle_in_recovery, m_dlcfg_power_cut]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac")
    ap.add_argument("tests", nargs="*", help="test names (default: all automatic)")
    args = ap.parse_args()

    by_name = {f.__name__: f for f in AUTO + MANUAL}
    tests = [by_name[n] for n in args.tests] if args.tests else AUTO
    b = Board(dl.find_port(args.port, args.mac))
    try:
        b.con.open(timeout=3.0)
        b.con.drain(0.5)
    except dl.serial.SerialException:
        print(f"{b.port} not present yet (board asleep or unplugged); continuing")

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
