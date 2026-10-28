"""Settings (SPEC §8.4), stored as JSON in %APPDATA%\\devlink\\settings.json. The bench Wi-Fi password is never
stored: the configure dialog asks for it."""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path


def data_dir() -> Path:
    base = os.environ.get("APPDATA") or str(Path.home())
    return Path(base) / "devlink"


@dataclass
class Settings:
    api_key: str = "devlink-bench"
    ap_password: str = "configure"
    bench_ssid: str = ""
    discovery_s: int = 5
    ap_scan_s: int = 15
    scan_for_unconfigured: bool = True
    manual_ips: list[str] = field(default_factory=list)
    recent_app: list[str] = field(default_factory=list)       # last few app files
    recent_recovery: list[str] = field(default_factory=list)  # last few recovery files

    @classmethod
    def load(cls, path: Path | None = None) -> Settings:
        p = path or data_dir() / "settings.json"
        try:
            raw = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return cls()
        known = {k: v for k, v in raw.items() if k in cls.__dataclass_fields__}
        return cls(**known)

    def save(self, path: Path | None = None) -> None:
        p = path or data_dir() / "settings.json"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")

    def remember_file(self, path: str, recovery: bool = False, keep: int = 5) -> None:
        lst = self.recent_recovery if recovery else self.recent_app
        if path in lst:
            lst.remove(path)
        lst.insert(0, path)
        del lst[keep:]
