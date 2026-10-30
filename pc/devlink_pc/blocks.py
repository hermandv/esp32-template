"""Binary block messages of block channels (SPEC §5.7; layout as dl_stream_core.h DL_BLOCK_*).

Little-endian: magic 0xB10C, version 1, sample type, channel id, n_fields, flags, seq, t0_us (u64), period_ns,
n_samples, header length; then n_fields float32 scales; then n_samples x n_fields samples, interleaved.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

MAGIC = 0xB10C
FLAG_GAP = 0x01      # blocks of this channel were dropped on the board before this one (its send ring was full)
FLAG_SRC_GAP = 0x02  # the producer lost samples (e.g. a sensor FIFO overflow)
_HDR = struct.Struct("<HBBHBBIQIHH")  # 28 bytes
_DTYPES = {0: "<i2", 1: "<i4", 2: "<f4"}


@dataclass
class Block:
    ch_id: int
    seq: int
    flags: int
    t0_us: int
    period_ns: int
    values: np.ndarray  # (n_samples, n_fields), scaled

    @property
    def times_us(self) -> np.ndarray:
        """Time of every sample, µs since the board booted (the same clock as the JSON "ts")."""
        return self.t0_us + np.arange(len(self.values)) * (self.period_ns / 1000.0)


def decode(msg: bytes) -> Block | None:
    """One binary WebSocket message -> Block, or None if it is not a valid block."""
    if len(msg) < _HDR.size:
        return None
    magic, ver, stype, ch_id, nf, flags, seq, t0, period, ns, hlen = _HDR.unpack_from(msg, 0)
    dt = _DTYPES.get(stype)
    if magic != MAGIC or ver != 1 or dt is None or nf == 0 or hlen != _HDR.size + 4 * nf:
        return None
    if len(msg) != hlen + ns * nf * np.dtype(dt).itemsize:
        return None
    scale = np.frombuffer(msg, "<f4", nf, _HDR.size).astype(np.float64)
    raw = np.frombuffer(msg, dt, ns * nf, hlen).reshape(ns, nf)
    return Block(ch_id, seq, flags, t0, period, raw * scale)


def field_names(meta: dict) -> list[str]:
    """Field names of a block channel from its /channels entry: a JSON list, or a comma string (older boards)."""
    f = meta.get("fields") or []
    names = [s for s in f.split(",")] if isinstance(f, str) else [str(s) for s in f]
    n = int(meta.get("n_fields") or len(names) or 1)
    return names if len(names) == n else [f"f{i}" for i in range(n)]


class SeqTracker:
    """Counts blocks lost between board and PC from the per-channel sequence numbers."""

    def __init__(self):
        self.last: dict[int, int] = {}
        self.lost: dict[int, int] = {}

    def reset(self) -> None:
        """A new connection: the board starts every channel at sequence 0 again."""
        self.last.clear()

    def feed(self, b: Block) -> int:
        """Returns the number of blocks missing right before @p b."""
        prev = self.last.get(b.ch_id)
        self.last[b.ch_id] = b.seq
        missing = 0 if prev is None else (b.seq - prev - 1) & 0xFFFFFFFF
        if missing > 0x7FFFFFFF:  # older than the last one: not a gap
            missing = 0
        self.lost[b.ch_id] = self.lost.get(b.ch_id, 0) + missing
        return missing
