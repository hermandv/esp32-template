"""Streaming client (SPEC §5.7, §8.3), Qt-free: one WebSocket to a board, subscriptions with rates, and automatic
reconnect after a reboot (the board is found again by MAC; its IP may change). Frames end up in a queue that the GUI
drains on a timer; binary blocks of block channels are decoded into a second queue (drain_blocks)."""

from __future__ import annotations

import json
import queue
import threading
import time
from collections.abc import Callable

from websockets.sync.client import connect as ws_connect

from . import blocks
from .discovery import MdnsBrowser, find_by_mac, norm_mac


class MonitorSession:
    """state: "connecting" / "connected" / "reconnecting" / "closed" (by the user) / "refused" (too many clients)."""

    def __init__(self, mac: str, ip: str, mdns: MdnsBrowser | None = None, retry_s: float = 3.0,
                 on_state: Callable[[str], None] | None = None):
        self.mac, self.ip, self.mdns, self.retry_s = norm_mac(mac), ip, mdns, retry_s
        self.on_state = on_state
        self.frames: queue.Queue = queue.Queue(maxsize=50000)
        self.replies: queue.Queue = queue.Queue(maxsize=100)
        self.blocks: queue.Queue = queue.Queue(maxsize=5000)  # decoded blocks (block channels)
        self.seq = blocks.SeqTracker()           # blocks lost between board and PC, per channel id
        self.hello: dict = {}                    # the board's hello ("block": 1 = it has block channels)
        self.subs: dict[str, int | None] = {}  # channel -> requested rate (None = channel default)
        self.state = "connecting"
        self.reconnects = 0
        self.lost_frames = 0                     # dropped on the PC side (GUI too slow)
        self._ws = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="devlink-monitor", daemon=True)
        self._thread.start()

    # ---- control (any thread) ----
    def subscribe(self, ch: str, hz: int | None = None) -> None:
        with self._lock:
            self.subs[ch] = hz
        self._send({"op": "sub", "ch": [ch]})
        if hz is not None:
            self._send({"op": "rate", "ch": ch, "hz": hz})

    def unsubscribe(self, ch: str) -> None:
        with self._lock:
            self.subs.pop(ch, None)
        self._send({"op": "unsub", "ch": [ch]})

    def set_rate(self, ch: str, hz: int) -> None:
        with self._lock:
            known = ch in self.subs
            if known:
                self.subs[ch] = hz
        if known:
            self._send({"op": "rate", "ch": ch, "hz": hz})

    def close(self) -> None:
        self._stop.set()
        with self._lock:
            ws, self._ws = self._ws, None
        if ws is not None:
            try:
                ws.close()
            except Exception:  # noqa: BLE001
                pass
        self._set("closed")

    def drain(self, max_items: int = 5000) -> list[dict]:
        out = []
        while len(out) < max_items:
            try:
                out.append(self.frames.get_nowait())
            except queue.Empty:
                break
        return out

    def drain_blocks(self, max_items: int = 2000) -> list[blocks.Block]:
        out = []
        while len(out) < max_items:
            try:
                out.append(self.blocks.get_nowait())
            except queue.Empty:
                break
        return out

    # ---- internals ----
    def _set(self, state: str) -> None:
        self.state = state
        if self.on_state:
            self.on_state(state)

    def _send(self, op: dict) -> None:
        with self._lock:
            ws = self._ws
        if ws is not None:
            try:
                ws.send(json.dumps(op))
            except Exception:  # noqa: BLE001  (reconnect handles it)
                pass

    def _connect(self) -> bool:
        try:
            ws = ws_connect(f"ws://{self.ip}/ws", open_timeout=5, max_size=None, ping_interval=10, ping_timeout=10)
        except Exception:  # noqa: BLE001
            return False
        try:
            hello = json.loads(ws.recv(timeout=5).splitlines()[0])
        except Exception:  # noqa: BLE001
            ws.close()
            return False
        if hello.get("error") == "too_many_clients":
            ws.close()
            # First connect: really full. Reconnecting: the board may still hold our dead socket's slot until it
            # notices (send timeout), so keep trying.
            self._set("refused" if self.reconnects == 0 else "reconnecting")
            return False
        self.hello = hello
        self.seq.reset()
        with self._lock:
            self._ws = ws
        with self._lock:
            subs = dict(self.subs)
        for ch, hz in subs.items():  # replay subscriptions after a reconnect
            self._send({"op": "sub", "ch": [ch]})
            if hz is not None:
                self._send({"op": "rate", "ch": ch, "hz": hz})
        self._set("connected")
        return True

    def _run(self) -> None:
        first = True
        while not self._stop.is_set():
            if not first:
                self._set("reconnecting")
                self.reconnects += 1
                s = find_by_mac(self.mac, timeout=self.retry_s, mdns=self.mdns, hint_ip=self.ip)
                if s is not None:
                    self.ip = s.ip
            first = False
            if not self._connect():
                if self.state == "refused":
                    return
                time.sleep(self.retry_s)
                continue
            self._read()
        self._set("closed")

    def _read(self) -> None:
        with self._lock:
            ws = self._ws
        while not self._stop.is_set() and ws is not None:
            try:
                msg = ws.recv(timeout=1)
            except TimeoutError:
                continue
            except Exception:  # noqa: BLE001  (closed: the board rebooted, Wi-Fi dropped, ...)
                break
            if isinstance(msg, bytes):  # binary message = one block of a block channel
                b = blocks.decode(msg)
                if b is not None:
                    self.seq.feed(b)
                    try:
                        self.blocks.put_nowait(b)
                    except queue.Full:
                        self.lost_frames += 1
                continue
            for line in msg.splitlines():
                if not line.strip():
                    continue
                try:
                    f = json.loads(line)
                except ValueError:
                    continue
                target = self.frames if "ch" in f else self.replies
                try:
                    target.put_nowait(f)
                except queue.Full:
                    self.lost_frames += 1
        with self._lock:
            if self._ws is ws:
                self._ws = None
