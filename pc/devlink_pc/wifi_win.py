"""Windows Wi-Fi helper for the configure flow (SPEC §8.1): current network, scan, join a board's config AP with a
temporary non-auto-connect profile, go back. Uses netsh (English output) and WlanScan."""

from __future__ import annotations

import ctypes
import os
import re
import subprocess
import tempfile
import time
from dataclasses import dataclass

DEVICE_AP = re.compile(r"^ESP32S3-(CFG|REC)-([0-9A-F]{6})$")
_NO_WINDOW = 0x08000000  # CREATE_NO_WINDOW


def _run(*args: str) -> str:
    r = subprocess.run(["netsh", "wlan", *args], capture_output=True, text=True, creationflags=_NO_WINDOW)
    return r.stdout + r.stderr


@dataclass
class WifiState:
    connected: bool
    ssid: str
    profile: str


def current() -> WifiState:
    out = _run("show", "interfaces")
    get = lambda key: (re.search(rf"^\s*{key}\s*:\s*(.*)$", out, re.M) or [None, ""])[1].strip()  # noqa: E731
    return WifiState(get("State").lower() == "connected", get("SSID"), get("Profile"))


def ipv4() -> str:
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "(Get-NetIPAddress -InterfaceAlias Wi-Fi -AddressFamily IPv4 -ErrorAction SilentlyContinue)"
                        ".IPAddress"], capture_output=True, text=True, creationflags=_NO_WINDOW)
    return r.stdout.strip().splitlines()[0] if r.stdout.strip() else ""


def request_scan() -> None:
    """Ask Windows for a fresh scan (otherwise a restarted AP can stay missing from the cache for a minute)."""
    import ctypes.wintypes as wt  # noqa: PLC0415

    class GUID(ctypes.Structure):
        _fields_ = [("d1", wt.DWORD), ("d2", wt.WORD), ("d3", wt.WORD), ("d4", ctypes.c_ubyte * 8)]

    class IfInfo(ctypes.Structure):
        _fields_ = [("guid", GUID), ("desc", ctypes.c_wchar * 256), ("state", ctypes.c_uint)]

    class IfList(ctypes.Structure):
        _fields_ = [("n", wt.DWORD), ("index", wt.DWORD), ("items", IfInfo * 1)]

    try:
        wlan = ctypes.windll.wlanapi
    except (AttributeError, OSError):
        return
    h, ver = wt.HANDLE(), wt.DWORD()
    if wlan.WlanOpenHandle(2, None, ctypes.byref(ver), ctypes.byref(h)) != 0:
        return
    lst = ctypes.POINTER(IfList)()
    if wlan.WlanEnumInterfaces(h, None, ctypes.byref(lst)) == 0:
        for it in (IfInfo * lst.contents.n).from_address(ctypes.addressof(lst.contents.items)):
            wlan.WlanScan(h, ctypes.byref(it.guid), None, None, None)
        wlan.WlanFreeMemory(lst)
    wlan.WlanCloseHandle(h, None)


def visible_ssids(fresh: bool = True) -> list[str]:
    if fresh:
        request_scan()
        time.sleep(4)
    return [m.strip() for m in re.findall(r"^SSID \d+\s*:\s*(.*)$", _run("show", "networks"), re.M) if m.strip()]


def device_aps(fresh: bool = True) -> list[tuple[str, str, str]]:
    """Config APs of devlink boards in range: (ssid, 'CFG'|'REC', id)."""
    out = []
    for s in visible_ssids(fresh):
        m = DEVICE_AP.match(s)
        if m:
            out.append((s, m.group(1), m.group(2)))
    return out


def _profile_xml(ssid: str, password: str) -> str:
    esc = lambda s: s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")  # noqa: E731
    return f"""<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>{esc(ssid)}</name>
  <SSIDConfig><SSID><name>{esc(ssid)}</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{esc(password)}</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>"""


def join_temp(ssid: str, password: str, timeout: float = 25.0) -> bool:
    """Join a board's AP with a temporary, manual-connect profile; True once we have a 192.168.4.x address."""
    fd, path = tempfile.mkstemp(suffix=".xml")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(_profile_xml(ssid, password))
        _run("add", "profile", f"filename={path}", "user=current")
    finally:
        os.unlink(path)
    end = time.time() + timeout
    while time.time() < end:
        _run("connect", f"name={ssid}", f"ssid={ssid}")
        for _ in range(10):
            time.sleep(0.5)
            if ipv4().startswith("192.168.4."):
                return True
        request_scan()
        time.sleep(3)
    return False


def remove_temp(ssid: str) -> None:
    _run("delete", "profile", f"name={ssid}")


def connect(profile: str, timeout: float = 30.0) -> bool:
    """Back to a known network; True once there is an address outside the board AP."""
    end = time.time() + timeout
    while time.time() < end:
        _run("connect", f"name={profile}")
        for _ in range(10):
            time.sleep(0.5)
            ip = ipv4()
            if ip and not ip.startswith(("192.168.4.", "169.254.")):
                return True
    return False
