"""Board list shared by the Qt and web UIs (SPEC §8.1), Qt-free: boards on the LAN (mDNS + UDP + add-by-IP, keyed
by MAC) and unconfigured boards seen as config APs."""

from __future__ import annotations

import time
from dataclasses import dataclass, field

from . import discovery
from .api import DeviceApi
from .settings import Settings

HIGHLIGHT = {"rollback", "crash_loop", "quick_reset", "recovery_loop", "no_valid_app"}


@dataclass
class Row:
    key: str                     # MAC, or "AP:<ssid>" for an unconfigured board
    mac: str = ""
    id: str = ""
    label: str = ""
    ip: str = ""
    mode: str = ""               # app / recovery / needs-config
    boot: str = ""
    app: str = ""
    rec: str = ""
    rssi: str = ""
    last_seen: float = 0.0
    ap_ssid: str = ""
    extra: dict = field(default_factory=dict)

    @property
    def hostname(self) -> str:
        return f"esp-{self.id.lower()}" if self.id else ""

    @property
    def on_lan(self) -> bool:
        return bool(self.ip) and self.mode != "needs-config"


def discover_all(s: Settings, mdns: discovery.MdnsBrowser | None) -> list[tuple[discovery.Seen, dict | None]]:
    """UDP + mDNS + manual IPs; then a quick /status per board for the RSSI."""
    found: dict[str, discovery.Seen] = {}
    for x in discovery.udp_scan(1.5) + (mdns.snapshot() if mdns else []):
        found.setdefault(x.mac, x)
    for ip in s.manual_ips:
        x = discovery.probe_ip(ip, timeout=2)
        if x:
            found[x.mac] = x
    out = []
    for x in found.values():
        try:
            st = DeviceApi(x.ip, x.mac, s.api_key, timeout=1.5).status()
        except Exception:  # noqa: BLE001
            st = None
        out.append((x, st))
    return out


def merge_seen(rows: dict[str, Row], found: list) -> None:
    """Merge discovery results into rows (keyed by MAC)."""
    for seen, st in found:
        f = seen.fields
        r = rows.get(seen.mac) or Row(seen.mac, seen.mac)
        r.ip, r.id, r.label = seen.ip, f.get("id") or r.id, f.get("label", r.label) or ""
        r.mode, r.boot = f.get("mode") or r.mode, f.get("boot") or r.boot
        r.app, r.rec = f.get("fw") or r.app, f.get("rec") or r.rec
        r.last_seen = seen.last_seen or time.time()
        if st:
            w = st.get("wifi", {})
            r.rssi = str(w.get("rssi")) if w.get("sta_connected") else "AP only"
        rows[seen.mac] = r
        # A configured board is no longer "needs config".
        for k in [k for k, v in rows.items() if v.mode == "needs-config" and v.id and v.id == r.id]:
            del rows[k]


def merge_aps(rows: dict[str, Row], aps: list[tuple[str, str, str]]) -> None:
    """Config/recovery APs in range -> "needs-config" rows, unless that board is already on the LAN."""
    lan_ids = {r.id for r in rows.values() if r.on_lan}
    seen_keys = set()
    for ssid, kind, dev_id in aps:
        if dev_id in lan_ids:
            continue  # already on the LAN: its AP is just the open config window
        key = f"AP:{ssid}"
        seen_keys.add(key)
        r = rows.get(key) or Row(key)
        r.id, r.ap_ssid, r.mode = dev_id, ssid, "needs-config"
        r.boot = "recovery AP" if kind == "REC" else "config AP"
        r.last_seen = time.time()
        rows[key] = r
    for k in [k for k, v in rows.items() if v.mode == "needs-config" and k not in seen_keys]:
        del rows[k]
