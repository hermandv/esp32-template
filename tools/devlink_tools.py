"""Shared helpers for the devlink PC tools: ESP-IDF paths, board ports, serial console, image formats.

Boards are found by USB VID 0x303A (Espressif native USB-Serial/JTAG) and their serial number (= MAC),
so changing COM numbers don't matter.
"""

from __future__ import annotations

import json
import os
import re
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parent.parent
ESPRESSIF_VID = 0x303A

# ---------------------------------------------------------------------------------------------------------------
# ESP-IDF environment
# ---------------------------------------------------------------------------------------------------------------


def require_idf() -> None:
    if not os.environ.get("IDF_PATH"):
        sys.exit("ESP-IDF is not active. In PowerShell run:  . C:\\projects\\esp-idf\\export-v6.0.3.ps1")


def esptool_cmd() -> list[str]:
    """esptool from the ESP-IDF Python env when active, else the current interpreter."""
    py = os.environ.get("IDF_PYTHON_ENV_PATH")
    exe = Path(py) / "Scripts" / "python.exe" if py else Path(sys.executable)
    return [str(exe), "-m", "esptool"]


def idf_py(project: Path, build_dir: Path, *args: str) -> None:
    require_idf()
    cmd = [sys.executable, str(Path(os.environ["IDF_PATH"]) / "tools" / "idf.py"),
           "-C", str(project), "-B", str(build_dir), *args]
    print("+", " ".join(cmd[2:]))
    subprocess.run(cmd, check=True)


# ---------------------------------------------------------------------------------------------------------------
# Ports
# ---------------------------------------------------------------------------------------------------------------


def norm_mac(mac: str) -> str:
    return re.sub(r"[^0-9a-f]", "", mac.lower())


def find_port(port: str | None = None, mac: str | None = None) -> str:
    if port:
        return port
    boards = [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if mac:
        boards = [p for p in boards if norm_mac(p.serial_number or "") == norm_mac(mac)]
    if len(boards) == 1:
        return boards[0].device
    found = ", ".join(f"{p.device} ({p.serial_number})" for p in boards) or "none"
    sys.exit(f"Need exactly one ESP32-S3 board{' with MAC ' + mac if mac else ''}; found: {found}. "
             "Use --port or --mac.")


# ---------------------------------------------------------------------------------------------------------------
# Serial console (no reset on open: DTR/RTS are released before the port opens)
# ---------------------------------------------------------------------------------------------------------------


class Console:
    def __init__(self, port: str, echo: bool = True):
        self.port = port
        self.echo = echo
        self.ser: serial.Serial | None = None
        self.buf = ""

    def open(self, timeout: float = 15.0) -> None:
        """Open, waiting for the port to come back (native USB re-enumerates after a reset)."""
        end = time.time() + timeout
        while True:
            try:
                s = serial.Serial()
                s.port = self.port
                s.baudrate = 115200
                s.timeout = 0.1
                s.dtr = False
                s.rts = False
                s.open()
                self.ser = s
                return
            except serial.SerialException:
                if time.time() > end:
                    raise
                time.sleep(0.2)

    def close(self) -> None:
        if self.ser:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
        self.ser = None

    def _read(self) -> str:
        if self.ser is None:
            # Port may be gone for a while (unplugged, deep sleep): keep trying, the caller owns the timeout.
            try:
                self.open(timeout=0.5)
            except serial.SerialException:
                time.sleep(0.3)
                return ""
        try:
            data = self.ser.read(4096)
        except serial.SerialException:
            # Board reset or unplugged: reopen when it comes back.
            self.close()
            time.sleep(0.3)
            try:
                self.open(timeout=1.0)
            except serial.SerialException:
                pass
            return ""
        text = data.decode(errors="replace")
        if text and self.echo:
            sys.stdout.write(text)
            sys.stdout.flush()
        return text

    def wait_for(self, pattern: str, timeout: float) -> re.Match | None:
        """Wait for a regex in the console output; returns the match (consumes output up to it)."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            if time.time() > end:
                return None
            self.buf += self._read()
            if len(self.buf) > 200_000:
                self.buf = self.buf[-100_000:]

    def drain(self, seconds: float = 0.3) -> None:
        end = time.time() + seconds
        while time.time() < end:
            self.buf += self._read()
        self.buf = ""

    def send(self, line: str) -> None:
        if self.ser is None:
            self.open(timeout=3.0)
        self.ser.write((line + "\n").encode())
        self.ser.flush()

    def command(self, line: str, reply: str = r"DLT (ok|err)[^\n]*\n", timeout: float = 5.0) -> re.Match | None:
        self.buf = ""
        try:
            self.send(line)
        except serial.SerialException:
            self.close()
            return None
        return self.wait_for(reply, timeout)

    def info(self, timeout: float = 5.0) -> dict | None:
        m = self.command("info", r"DLT info ([^\n]*)\n", timeout)
        return parse_kv(m.group(1)) if m else None


    def devlink_json(self, what: str = "info", timeout: float = 5.0) -> dict | None:
        """'devlink info' / 'devlink status' console line -> parsed JSON."""
        m = self.command(f"devlink {what}", rf"devlink {what} (\{{[^\n]*\}})\r?\n", timeout)
        return json.loads(m.group(1)) if m else None

    # -- Improv serial (https://www.improv-wifi.com/serial/) --
    def improv(self, cmd: int, payload: bytes = b"", timeout: float = 5.0, until_empty: bool = False) -> list:
        """Send an Improv RPC; return the frames received as (type, data) until a result/error arrives
        (or, with until_empty, until an empty RPC result ends a list such as the scan)."""
        if self.ser is None:
            self.open()
        self.ser.reset_input_buffer()
        self.ser.write(improv_frame(IMPROV_RPC, bytes([cmd, len(payload)]) + payload))
        self.ser.flush()
        frames, raw = [], b""
        end = time.time() + timeout
        while time.time() < end:
            raw += self.ser.read(512)
            found, raw = improv_parse(raw)
            for t, d in found:
                frames.append((t, d))
                if t == IMPROV_ERROR:
                    return frames
                if t == IMPROV_RPC_RESULT and (not until_empty or d[1] == 0):
                    return frames
        return frames


def parse_kv(text: str) -> dict:
    return dict(kv.split("=", 1) for kv in text.split() if "=" in kv)


IMPROV_STATE, IMPROV_ERROR, IMPROV_RPC, IMPROV_RPC_RESULT = 1, 2, 3, 4
IMPROV_CMD_WIFI, IMPROV_CMD_STATE, IMPROV_CMD_INFO, IMPROV_CMD_SCAN = 1, 2, 3, 4


def improv_frame(ftype: int, data: bytes) -> bytes:
    f = b"IMPROV" + bytes([1, ftype, len(data)]) + data
    return f + bytes([sum(f) & 0xFF])


def improv_parse(buf: bytes) -> tuple[list, bytes]:
    """Extract complete Improv frames from a byte stream that also carries text; returns (frames, rest)."""
    frames = []
    while True:
        i = buf.find(b"IMPROV")
        if i < 0:
            return frames, buf[-5:]
        if len(buf) < i + 9:
            return frames, buf[i:]
        n = buf[i + 8]
        if len(buf) < i + 10 + n:
            return frames, buf[i:]
        f = buf[i:i + 10 + n]
        if f[6] == 1 and (sum(f[:-1]) & 0xFF) == f[-1]:
            frames.append((f[7], f[9:9 + n]))
        buf = buf[i + 10 + n:]


def improv_strings(data: bytes) -> list[str]:
    """Strings of an RPC result: command, total length, then length-prefixed strings."""
    out, p = [], 2
    while p < len(data):
        n = data[p]
        out.append(data[p + 1:p + 1 + n].decode(errors="replace"))
        p += 1 + n
    return out


# ---------------------------------------------------------------------------------------------------------------
# Image formats
# ---------------------------------------------------------------------------------------------------------------

OTADATA_SIZE = 0x2000


def otadata_image(slot: int) -> bytes:
    """otadata selecting ota_<slot> (same as otatool switch_ota_partition, state left undefined = valid)."""
    seq = slot + 1
    crc = zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    entry = struct.pack("<I20sII", seq, b"\xff" * 20, 0xFFFFFFFF, crc)
    return entry + b"\xff" * (OTADATA_SIZE - len(entry))


DLCFG_MAGIC = 0x47464344
DLCFG_FORMAT = 1
DLCFG_SECTOR = 4096


def dlcfg_payload(ssid: str = "", password: str = "", label: str = "", log_level: int = 3,
                  ap_window_s: int = 20, button_window_s: int = 120) -> bytes:
    """dlcfg_t format 1 (components/devlink/include/dlcfg_core.h), 272 bytes."""
    def s(v: str, n: int) -> bytes:
        b = v.encode()
        if len(b) > n - 1:
            raise ValueError(f"'{v}' longer than {n - 1} bytes")
        return b.ljust(n, b"\0")

    # C layout: strings @0..228, log_level @229, ap_window_s @230, button_window_s @232, reset_ring_pos @234,
    # reserved @235..237, padding @238..239, reset_ring[8] @240..271.
    p = s(ssid, 33) + s(password, 65) + s("", 33) + s("", 65) + s(label, 33)
    p += struct.pack("<BHHB3s2x8I", log_level, ap_window_s, button_window_s, 0, b"\0" * 3, *([0] * 8))
    assert len(p) == 272, len(p)
    return p


def dlcfg_image(payload: bytes, sequence: int = 1) -> bytes:
    """Whole dlcfg partition (2 sectors): record in sector 0, sector 1 erased."""
    hdr = struct.pack("<IHHI", DLCFG_MAGIC, DLCFG_FORMAT, len(payload), sequence)
    crc = zlib.crc32(payload, zlib.crc32(hdr)) & 0xFFFFFFFF
    rec = hdr + struct.pack("<I", crc) + payload
    return rec + b"\xff" * (2 * DLCFG_SECTOR - len(rec))
