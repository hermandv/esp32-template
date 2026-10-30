"""Improv Wi-Fi serial client (SPEC §5.4, §8.1 USB fallback) over the board's USB port: native USB-Serial/JTAG or a
USB-UART bridge."""

from __future__ import annotations

import re
import time
from dataclasses import dataclass

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
# USB-serial devices an ESP32 board can sit behind: Espressif native USB, Silicon Labs CP210x, WCH CH34x, FTDI.
USB_SERIAL_VIDS = {ESPRESSIF_VID: "Espressif", 0x10C4: "Silicon Labs", 0x1A86: "WCH", 0x0403: "FTDI"}
STATE, ERROR, RPC, RPC_RESULT = 1, 2, 3, 4
CMD_WIFI, CMD_STATE, CMD_INFO, CMD_SCAN = 1, 2, 3, 4
ERRORS = {1: "invalid RPC", 2: "unknown RPC", 3: "unable to connect", 0xFF: "unknown error"}


def frame(ftype: int, data: bytes) -> bytes:
    f = b"IMPROV" + bytes([1, ftype, len(data)]) + data
    return f + bytes([sum(f) & 0xFF])


def parse(buf: bytes) -> tuple[list[tuple[int, bytes]], bytes]:
    """Complete frames from a stream that also carries log text; returns (frames, unparsed rest)."""
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


def strings(data: bytes) -> list[str]:
    out, p = [], 2
    while p < len(data):
        n = data[p]
        out.append(data[p + 1:p + 1 + n].decode(errors="replace"))
        p += 1 + n
    return out


@dataclass
class UsbBoard:
    port: str
    serial_number: str
    firmware: str = ""
    version: str = ""
    chip: str = ""
    name: str = ""          # the board's label, or its hostname when it has none (Improv device name)
    label: str = ""         # the label only: empty when the name is the hostname (prefill for "friendly name")


class ImprovPort:
    def __init__(self, port: str):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.1
        s.dtr = s.rts = False  # opening must not reset the board
        s.open()
        self.ser = s
        self.log: list[str] = []  # the board's log text seen while waiting for Improv answers (diagnosis)

    def close(self) -> None:
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def rpc(self, cmd: int, payload: bytes = b"", timeout: float = 5.0, until_empty: bool = False,
            until_state: int | None = None) -> list[tuple[int, bytes]]:
        self.ser.reset_input_buffer()
        self.ser.write(frame(RPC, bytes([cmd, len(payload)]) + payload))
        out, raw, end = [], b"", time.time() + timeout
        text = b""
        while time.time() < end:
            chunk = self.ser.read(512)
            raw += chunk
            text += chunk
            fr, raw = parse(raw)
            for t, d in fr:
                out.append((t, d))
                if t == ERROR:
                    return out
                if t == RPC_RESULT and (not until_empty or d[1] == 0) and until_state is None:
                    return out
                if until_state is not None and t == RPC_RESULT and d[0] == cmd:
                    break
            else:
                continue
            break
        self.log += [ln for ln in text.decode(errors="replace").splitlines() if ln.strip() and "IMPROV" not in ln]
        return out

    def info(self) -> list[str] | None:
        res = [d for t, d in self.rpc(CMD_INFO, timeout=2.0) if t == RPC_RESULT]
        return strings(res[0]) if res else None

    def provision(self, ssid: str, password: str, timeout: float = 50.0) -> str:
        """Send Wi-Fi settings; returns the board's URL once connected, raises RuntimeError otherwise."""
        s, p = ssid.encode(), password.encode()
        if not 0 < len(s) <= 32 or len(p) > 64:
            raise ValueError("SSID 1..32 bytes, password up to 64 bytes")
        fr = self.rpc(CMD_WIFI, bytes([len(s)]) + s + bytes([len(p)]) + p, timeout=timeout, until_state=1)
        for t, d in fr:
            if t == ERROR:
                raise RuntimeError(f"board: {ERRORS.get(d[0], d[0])}")
            if t == RPC_RESULT and d[0] == CMD_WIFI:
                urls = strings(d)
                return urls[0] if urls else ""
        raise RuntimeError("no answer from the board")

    def line(self, text: str, expect: str, timeout: float = 3.0) -> str | None:
        """Send a 'devlink ...' console line and wait for a complete reply line containing `expect` (the info line
        is about 1 KB and arrives in pieces)."""
        self.ser.reset_input_buffer()
        self.ser.write((text + "\n").encode())
        buf, end = "", time.time() + timeout
        while time.time() < end:
            buf += self.ser.read(512).decode(errors="replace")
            *done, buf = buf.split("\n")
            for ln in done:
                if expect in ln:
                    return ln.rstrip("\r")
        return None


def find_boards(probe_timeout: float = 2.0) -> list[UsbBoard]:
    """USB ports (native USB or a USB-UART bridge) that answer Improv 'device info'. Runs only when the user asks:
    it writes to every such port, and a bench may have bridges to other instruments."""
    out = []
    for p in list_ports.comports():
        if p.vid not in USB_SERIAL_VIDS:
            continue
        try:
            with ImprovPort(p.device) as ip:
                s = ip.info()
        except (serial.SerialException, OSError):
            continue
        if s and len(s) >= 4:
            label = "" if re.fullmatch(r"esp-[0-9a-f]{6}", s[3]) else s[3]  # devlink hostname = no label set
            out.append(UsbBoard(p.device, p.serial_number or "", s[0], s[1], s[2], s[3], label))
    return out
