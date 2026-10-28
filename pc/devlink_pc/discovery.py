"""Discovery (SPEC §5.5): mDNS _devlink._tcp browse + UDP broadcast "DEVLINK?" on 47800, plus add-by-IP.
Results are merged by MAC (the PC app's primary key)."""

from __future__ import annotations

import json
import socket
import threading
import time
from dataclasses import dataclass, field

import requests

UDP_PORT = 47800
SERVICE = "_devlink._tcp.local."


def norm_mac(mac: str) -> str:
    h = "".join(c for c in mac.lower() if c in "0123456789abcdef")
    return ":".join(h[i:i + 2] for i in range(0, 12, 2)).upper() if len(h) == 12 else mac.upper()


@dataclass
class Seen:
    mac: str
    ip: str
    fields: dict = field(default_factory=dict)   # id, label, board, mode, fw, rec, api, boot
    source: str = ""                             # "udp", "mdns", "ip"
    last_seen: float = 0.0


def _broadcast_addresses() -> list[str]:
    """255.255.255.255 plus each local IPv4 /24 broadcast (Windows sends the limited broadcast on one adapter only)."""
    out = {"255.255.255.255"}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                out.add(".".join(ip.split(".")[:3]) + ".255")
    except OSError:
        pass
    return sorted(out)


def udp_scan(timeout: float = 1.5, extra: list[str] | None = None) -> list[Seen]:
    """Broadcast DEVLINK? and collect answers for `timeout` seconds."""
    found: dict[str, Seen] = {}
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.2)
    try:
        s.bind(("", 0))
        for dest in _broadcast_addresses() + (extra or []):
            try:
                s.sendto(b"DEVLINK?", (dest, UDP_PORT))
            except OSError:
                pass
        end = time.time() + timeout
        while time.time() < end:
            try:
                data, addr = s.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                rec = json.loads(data)
            except ValueError:
                continue
            if "mac" in rec:
                mac = norm_mac(rec["mac"])
                found[mac] = Seen(mac, addr[0], rec, "udp", time.time())
    finally:
        s.close()
    return list(found.values())


def probe_ip(ip: str, timeout: float = 3.0) -> Seen | None:
    """Add by IP: ask /info directly."""
    try:
        i = requests.get(f"http://{ip}/api/v1/info", timeout=timeout).json()
    except (requests.RequestException, ValueError):
        return None
    f = {"id": i.get("id"), "label": i.get("label"), "board": i.get("board"), "mode": i.get("mode"),
         "fw": i.get("fw", {}).get("version"), "rec": (i.get("recovery") or {}).get("version", ""),
         "api": i.get("api"), "boot": i.get("boot", {}).get("reason")}
    return Seen(norm_mac(i["mac"]), ip, f, "ip", time.time())


class MdnsBrowser:
    """Background mDNS browse; .snapshot() returns what was seen. Optional: works without zeroconf installed."""

    def __init__(self):
        self._seen: dict[str, Seen] = {}
        self._lock = threading.Lock()
        self._zc = None
        try:
            from zeroconf import ServiceBrowser, Zeroconf  # noqa: PLC0415

            self._zc = Zeroconf()
            self._browser = ServiceBrowser(self._zc, SERVICE, handlers=[self._on_change])
        except Exception:  # noqa: BLE001  (zeroconf missing or no multicast)
            self._zc = None

    def _on_change(self, zeroconf, service_type, name, state_change):  # noqa: ARG002
        info = zeroconf.get_service_info(service_type, name, timeout=2000)
        if info is None or not info.parsed_addresses():
            return
        txt = {k.decode(): (v.decode() if v else "") for k, v in info.properties.items()}
        if "mac" not in txt:
            return
        mac = norm_mac(txt["mac"])
        with self._lock:
            self._seen[mac] = Seen(mac, info.parsed_addresses()[0], txt, "mdns", time.time())

    def snapshot(self) -> list[Seen]:
        with self._lock:
            return list(self._seen.values())

    def close(self) -> None:
        if self._zc is not None:
            self._zc.close()


def find_by_mac(mac: str, timeout: float = 5.0, mdns: MdnsBrowser | None = None, hint_ip: str | None = None) -> Seen | None:
    """Current IP of a board (SPEC §7.2: rediscover before uploading)."""
    mac = norm_mac(mac)
    end = time.time() + timeout
    while time.time() < end:
        if hint_ip:
            s = probe_ip(hint_ip, timeout=2)
            if s and s.mac == mac:
                return s
        for s in udp_scan(1.0, extra=[hint_ip] if hint_ip else None) + (mdns.snapshot() if mdns else []):
            if s.mac == mac:
                return s
    return None
