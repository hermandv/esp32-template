"""HTTP client for one devlink board (SPEC §5.6). Mutating calls send X-Api-Key and X-Device-Id."""

from __future__ import annotations

import io
from collections.abc import Callable

import requests

DEFAULT_KEY = "devlink-bench"


class ApiError(Exception):
    def __init__(self, status: int, error: str):
        super().__init__(f"HTTP {status}: {error}")
        self.status = status
        self.error = error


class _Upload(io.RawIOBase):
    """File-like body with a known length (so requests sends Content-Length, not chunked) and progress."""

    def __init__(self, data: bytes, progress: Callable[[int, int], None] | None):
        self.data, self.pos, self.progress = data, 0, progress

    def __len__(self) -> int:
        return len(self.data)

    def tell(self) -> int:
        # requests computes the body length as len - tell(); without tell() it gets 0 and falls back to chunked
        # transfer encoding, which the board does not accept.
        return self.pos

    def readable(self) -> bool:
        return True

    def read(self, n: int = -1) -> bytes:
        n = len(self.data) - self.pos if n is None or n < 0 else min(n, 8192)
        chunk = self.data[self.pos:self.pos + n]
        self.pos += len(chunk)
        if self.progress:
            self.progress(self.pos, len(self.data))
        return chunk


class DeviceApi:
    def __init__(self, ip: str, mac: str, key: str = DEFAULT_KEY, timeout: float = 8.0):
        self.ip, self.mac, self.key, self.timeout = ip, mac, key, timeout

    @property
    def base(self) -> str:
        return f"http://{self.ip}/api/v1"

    def _headers(self) -> dict:
        return {"X-Api-Key": self.key, "X-Device-Id": self.mac}

    def _check(self, r: requests.Response) -> requests.Response:
        if r.status_code >= 400:
            try:
                err = r.json().get("error", r.text)
            except ValueError:
                err = r.text[:80]
            raise ApiError(r.status_code, err)
        return r

    def get(self, path: str, timeout: float | None = None) -> dict:
        return self._check(requests.get(self.base + path, timeout=timeout or self.timeout)).json()

    def post(self, path: str, body: dict | None = None, timeout: float | None = None) -> dict:
        r = requests.post(self.base + path, json=body or {}, headers=self._headers(), timeout=timeout or self.timeout)
        return self._check(r).json()

    # -- endpoints --
    def info(self) -> dict:
        return self.get("/info")

    def status(self) -> dict:
        return self.get("/status")

    def channels(self) -> dict:
        return self.get("/channels")

    def config(self) -> dict:
        return self.get("/config")

    def set_config(self, **fields) -> dict:
        return self.post("/config", fields)

    def set_wifi(self, ssid: str, password: str) -> dict:
        """Over the LAN: pending + revert after 45 s if it does not connect (SPEC §5.4)."""
        return self.post("/wifi", {"ssid": ssid, "password": password})

    def scan(self) -> list[dict]:
        return self.get("/wifi/scan", timeout=20)["networks"]

    def reboot(self, target: str = "app") -> dict:
        return self.post("/reboot", {"target": target})

    def factory_reset(self, nvs: bool = False) -> dict:
        return self.post("/factory-reset", {"nvs": nvs})

    def coredump(self) -> bytes | None:
        r = requests.get(self.base + "/coredump", timeout=30)
        if r.status_code == 404:
            return None
        return self._check(r).content

    def delete_coredump(self) -> None:
        self._check(requests.delete(self.base + "/coredump", headers=self._headers(), timeout=self.timeout))

    def ota_progress(self) -> dict:
        return self.get("/ota/progress", timeout=4)

    def ota(self, image: bytes, target: str = "app", force: bool = False,
            progress: Callable[[int, int], None] | None = None) -> dict:
        url = f"{self.base}/ota?target={target}" + ("&force=1" if force else "")
        headers = {**self._headers(), "Content-Type": "application/octet-stream", "Content-Length": str(len(image))}
        r = requests.post(url, data=_Upload(image, progress), headers=headers, timeout=(20, 120))  # 20 s: the board's idle timeout
        return self._check(r).json()
