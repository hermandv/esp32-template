"""First flash of a blank (or broken) board over USB from the PC app (SPEC §3.8, §8.1). Qt-free.

The same steps as tools/flash_all.py, without ESP-IDF: esptool (pip) writes the files of existing builds. A build
folder holds one or more IDF build directories, e.g. build/ of this repo; each has flasher_args.json, the image and
project_description.json. The user picks the recovery build and the app build (scan_builds lists them):
  recovery build -> bootloader (the devlink bootloader is built with recovery), partition table, recovery image
  app build      -> the app image
Order: check the board (console + esptool), erase all -> bootloader + partition table + otadata (selects ota_0)
+ recovery -> factory + app -> ota_0, verify, reset, wait for "devlink info" on the console, then the label and
optionally Wi-Fi over Improv.
"""

from __future__ import annotations

import json
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from serial.tools import list_ports

from . import image
from .improv import USB_SERIAL_VIDS, ImprovPort

OTADATA_SIZE = 0x2000
BAUD = "921600"  # native USB ignores it; CP210x / CH34x / FTDI bridges handle it (esptool syncs at 115200 first)
MB = 1024 * 1024


@dataclass
class Build:
    """One IDF build directory."""
    dir: Path
    project: str
    version: str
    kind: str                                   # "recovery" or "app" (from the project name)
    flash_size: str                             # "8MB"
    flasher: dict
    image: image.ImageInfo
    devlink_bootloader: bool | None = None      # None = no bootloader metadata in the folder

    @property
    def label(self) -> str:
        return f"{self.dir.name}: {self.project} {self.version} ({self.flash_size})"


@dataclass
class Firmware:
    recovery: Build
    app: Build
    flash_args: list[str]                       # --flash-mode ... --flash-freq ... --flash-size ...
    flash_size: str
    partitions: dict[str, tuple[int, int]]      # name -> (offset, size)
    files: list[tuple[int, Path]]               # bootloader, partition table, recovery, app (otadata added later)
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def summary(self) -> str:
        return (f"app {self.app.project} {self.app.version}, recovery {self.recovery.project} "
                f"{self.recovery.version}, {self.flash_size} flash")


@dataclass
class BoardCheck:
    port: str
    chip: str = ""
    mac: str = ""
    flash_size: str = ""                        # "8MB", detected by esptool
    info: dict | None = None                    # "devlink info" if the board runs devlink
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def summary(self) -> str:
        s = f"{self.chip or '?'} {self.mac or '?'}, {self.flash_size or '?'} flash"
        if self.info:
            fw = self.info.get("fw", {})
            s += (f"; runs devlink: label '{self.info.get('label') or ''}', {self.info.get('mode', '?')} "
                  f"{fw.get('project', '?')} {fw.get('version', '?')}")
        else:
            s += "; no devlink answer on the console (blank or another firmware)"
        return s


def usb_ports() -> list[tuple[str, str]]:
    """(device, description) of the USB-serial ports a board may be on."""
    out = []
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        if p.vid in USB_SERIAL_VIDS:
            out.append((p.device, f"{p.device}: {p.description or USB_SERIAL_VIDS[p.vid]}"
                                  + (f" ({p.serial_number})" if p.serial_number else "")))
    return out


def parse_partitions(data: bytes) -> dict[str, tuple[int, int]]:
    """ESP partition table binary -> {label: (offset, size)} (32-byte entries, magic 0x50AA)."""
    out = {}
    for i in range(0, len(data) - 31, 32):
        magic, _t, _st, off, size, label, _flags = struct.unpack_from("<HBBII16sI", data, i)
        if magic != 0x50AA:
            break
        out[label.split(b"\0", 1)[0].decode()] = (off, size)
    return out


def otadata_image(slot: int = 0) -> bytes:
    """otadata selecting ota_<slot>, state undefined = valid (same bytes as tools/devlink_tools.otadata_image)."""
    seq = slot + 1
    crc = zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    entry = struct.pack("<I20sII", seq, b"\xff" * 20, 0xFFFFFFFF, crc)
    return entry + b"\xff" * (OTADATA_SIZE - len(entry))


def _size_bytes(s: str) -> int:
    m = re.fullmatch(r"(\d+)MB", s or "")
    return int(m.group(1)) * MB if m else 0


def load_build(d: Path) -> Build | None:
    """A build directory -> Build, or None if it is not a finished app build."""
    try:
        fa = json.loads((d / "flasher_args.json").read_text())
        app_bin = d / fa["app"]["file"]
        info = image.parse_file(app_bin)
    except (OSError, ValueError, KeyError):
        return None
    if not info.ok:
        return None
    boot = None
    try:
        desc = json.loads((d / "bootloader" / "project_description.json").read_text())
        boot = "devlink_boot" in desc.get("build_components", [])
    except (OSError, ValueError):
        pass
    kind = "recovery" if "recovery" in info.project.lower() else "app"
    return Build(d, info.project, info.version, kind, fa["flash_settings"]["flash_size"], fa, info, boot)


def scan_builds(root: str | Path) -> list[Build]:
    """The builds in @p root (a build directory itself, or a folder of build directories), recovery first."""
    root = Path(root)
    dirs = [root] if (root / "flasher_args.json").is_file() else (
        sorted(d for d in root.iterdir() if d.is_dir()) if root.is_dir() else [])
    builds = [b for b in (load_build(d) for d in dirs) if b is not None]
    return sorted(builds, key=lambda b: (b.kind != "recovery", b.dir.name))


def make_firmware(recovery: Build, app: Build) -> Firmware:
    """Combine the two picked builds and check them; errors block programming, warnings are shown."""
    rec = recovery.flasher
    fs = rec["flash_settings"]
    pt = recovery.dir / rec["partition-table"]["file"]
    parts = parse_partitions(pt.read_bytes()) if pt.is_file() else {}
    fw = Firmware(recovery, app, ["--flash-mode", fs["flash_mode"], "--flash-freq", fs["flash_freq"],
                                  "--flash-size", fs["flash_size"]], fs["flash_size"], parts,
                  [(0x0, recovery.dir / rec["bootloader"]["file"]), (0x8000, pt)])
    if recovery.kind != "recovery":
        fw.errors.append(f"'{recovery.project}' is not a recovery build (its project name has no 'recovery')")
    if app.kind == "recovery":
        fw.errors.append(f"'{app.project}' is a recovery build: pick an app for ota_0")
    if recovery.dir == app.dir:
        fw.errors.append("recovery and app are the same build")
    missing = [n for n in ("otadata", "factory", "ota_0") if n not in parts]
    if missing:
        fw.errors.append(f"partition table has no {', '.join(missing)} (not a devlink layout)")
        return fw
    fw.files += [(parts["factory"][0], recovery.dir / rec["app"]["file"]),
                 (parts["ota_0"][0], app.dir / app.flasher["app"]["file"])]
    for _off, f in fw.files:
        if not f.is_file():
            fw.errors.append(f"missing {f}")
    if recovery.devlink_bootloader is False:
        fw.errors.append("the recovery build's bootloader is not the devlink bootloader")
    elif recovery.devlink_bootloader is None:
        fw.warnings.append("cannot check that the bootloader is the devlink one (no bootloader metadata)")
    if app.flash_size != recovery.flash_size:
        fw.errors.append(f"app built for {app.flash_size}, recovery for {recovery.flash_size} flash")
    for b, slot in ((recovery, "factory"), (app, "ota_0")):
        info = image.parse_file(b.dir / b.flasher["app"]["file"], slot_size=parts[slot][1])
        fw.errors += [f"{b.project}: {e}" for e in info.errors]
        fw.warnings += [f"{b.project}: {w}" for w in info.warnings]
    return fw


def check_board(fw: Firmware | None, bc: BoardCheck) -> None:
    """Add errors / warnings for programming @p fw onto the board in @p bc."""
    if bc.chip and "ESP32-S3" not in bc.chip.upper():
        bc.errors.append(f"the board is an {bc.chip}, not an ESP32-S3")
    if fw is not None and bc.flash_size:
        have, need = _size_bytes(bc.flash_size), _size_bytes(fw.flash_size)
        if need > have:
            bc.errors.append(f"the build is for {fw.flash_size} flash, the board has {bc.flash_size}")
        elif need < have:
            bc.warnings.append(f"the build is for {fw.flash_size} flash, the board has {bc.flash_size} "
                               "(works, but the rest of the flash is unused)")
    if bc.info:
        bc.warnings.append(f"the board runs devlink (label '{bc.info.get('label') or ''}'): erasing loses its "
                           "Wi-Fi settings and label")


class Cancelled(Exception):
    pass


def esptool_cmd() -> list[str]:
    """esptool from this Python (pip esptool in the PC app's venv)."""
    return [sys.executable, "-m", "esptool"]


def _esptool(args: list[str], log: Callable[[str], None], cancel: threading.Event | None = None) -> str:
    """Run esptool in a subprocess, stream its output to @p log; returns the whole output."""
    cmd = [*esptool_cmd(), "--chip", "esp32s3", *args]
    log("> esptool " + " ".join(a for a in args if not a.endswith(".bin"))[:160])
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if cancel is not None:  # kill esptool when the user cancels (its output pipe then closes and the loop ends)
        def watch():
            while p.poll() is None:
                if cancel.wait(0.2):
                    p.kill()
                    return
        threading.Thread(target=watch, daemon=True).start()
    buf, out = b"", []
    while True:
        c = p.stdout.read(1)
        if not c:
            break
        if c in (b"\r", b"\n"):  # esptool redraws its progress line with \r
            if buf.strip():
                line = buf.decode(errors="replace").rstrip()
                out.append(line)
                log(line)
            buf = b""
        else:
            buf += c
    if p.wait() != 0:
        if cancel is not None and cancel.is_set():
            raise Cancelled("cancelled: the board's flash is incomplete, program it again before use")
        raise RuntimeError(f"esptool failed (exit {p.returncode}); is the board on this port and powered?")
    return "\n".join(out)


def console_json(ip: ImprovPort, what: str, timeout: float) -> dict | None:
    """'devlink <what>' on the console -> its JSON reply (None if the board does not answer)."""
    line = ip.line(f"devlink {what}", f"devlink {what} {{", timeout=timeout)
    if line is None:
        return None
    try:
        return json.loads(line[line.index("{"):])
    except ValueError:
        return None


def probe_board(port: str, log: Callable[[str], None]) -> BoardCheck:
    """What is on @p port: devlink on the console (if it runs), then chip, MAC and flash size from esptool."""
    bc = BoardCheck(port)
    try:
        with ImprovPort(port) as ip:
            bc.info = console_json(ip, "info", timeout=2.0)
    except OSError as e:
        bc.errors.append(f"cannot open {port}: {e}")
        return bc
    out = _esptool(["--port", port, "flash-id"], log)
    if m := re.search(r"Chip type:\s*(.+)", out) or re.search(r"Chip is (.+)", out):
        bc.chip = m.group(1).strip()
    if m := re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out):
        bc.mac = m.group(1).upper()
    if m := re.search(r"Detected flash size:\s*(\d+MB)", out):
        bc.flash_size = m.group(1)
    return bc


def program(port: str, fw: Firmware, log: Callable[[str], None], label: str = "", wait_s: float = 40.0,
            cancel: threading.Event | None = None) -> dict:
    """Erase and write everything, verify, reset, wait for the app. Returns the board's 'devlink info'."""
    if fw.errors:
        raise ValueError("; ".join(fw.errors))
    with tempfile.TemporaryDirectory() as tmp:
        ota = Path(tmp) / "otadata.bin"
        ota.write_bytes(otadata_image(0))
        regions = fw.files[:2] + [(fw.partitions["otadata"][0], ota)] + fw.files[2:]
        files = [x for off, path in regions for x in (hex(off), str(path))]
        log("writing: " + ", ".join(f"{hex(o)} {p.name}" for o, p in regions))
        # Erase + write in one session, then verify; only the final hard reset runs the new bootloader.
        _esptool(["--port", port, "--baud", BAUD, "--after", "no-reset", "write-flash", "--erase-all",
                  *fw.flash_args, *files], log, cancel)
        _esptool(["--port", port, "--baud", BAUD, "--after", "hard-reset", "verify-flash", *fw.flash_args, *files],
                 log, cancel)
    log("waiting for the app on the console…")
    end = time.time() + wait_s
    info = None
    time.sleep(1.0)  # the USB port re-enumerates after the reset
    while time.time() < end and info is None:
        if cancel is not None and cancel.is_set():
            raise Cancelled("cancelled while waiting for the app (the flash itself is complete)")
        try:
            with ImprovPort(port) as ip:
                info = console_json(ip, "info", timeout=3.0)
                if info is not None and label:
                    if ip.line(f"devlink label {label}", "devlink: label", timeout=3.0) is None:
                        log("the board did not confirm the label")
                    info["label"] = label
        except OSError:
            time.sleep(1.0)  # port not back yet
    if info is None:
        raise RuntimeError(f"flashed and verified, but the board did not answer on {port} within {wait_s:.0f} s")
    log(f"running: {info.get('fw', {}).get('project')} {info.get('fw', {}).get('version')}, MAC {info.get('mac')}")
    return info


def set_wifi(port: str, ssid: str, password: str, log: Callable[[str], None]) -> str:
    """Wi-Fi over Improv on the same port; returns the board's URL."""
    log(f"joining '{ssid}' over Improv…")
    with ImprovPort(port) as ip:
        url = ip.provision(ssid, password)
    log(f"on the network: {url}")
    return url
