"""Binary block decoding (block channels, SPEC §5.7): the layout dl_block_header() builds on the board."""

import struct

import numpy as np

from devlink_pc import blocks


def make(ch_id=7, seq=1, flags=0, t0=1_000_000, period=2_000_000, data=((1, -2, 3), (4, 5, -6)),
         scale=(0.5, 1.0, 2.0), stype=0, dtype="<i2"):
    nf = len(scale)
    hdr = struct.pack("<HBBHBBIQIHH", 0xB10C, 1, stype, ch_id, nf, flags, seq, t0, period, len(data), 28 + 4 * nf)
    return hdr + struct.pack(f"<{nf}f", *scale) + np.asarray(data, dtype).tobytes()


def test_decode_scales_and_times():
    b = blocks.decode(make())
    assert b is not None and b.ch_id == 7 and b.seq == 1
    assert np.allclose(b.values, [[0.5, -2, 6], [2, 5, -12]])
    assert np.allclose(b.times_us, [1_000_000, 1_002_000])


def test_decode_other_sample_types():
    assert np.allclose(blocks.decode(make(stype=1, dtype="<i4", data=((70000, 0, -1),))).values, [[35000, 0, -2]])
    assert np.allclose(blocks.decode(make(stype=2, dtype="<f4", data=((0.25, 1, 2),))).values, [[0.125, 1, 4]])


def test_decode_rejects_bad_messages():
    m = make()
    assert blocks.decode(m[:-1]) is None              # the length must match exactly
    assert blocks.decode(b"\x00" + m[1:]) is None     # magic
    assert blocks.decode(m[:2] + b"\x02" + m[3:]) is None  # version
    assert blocks.decode(b"{\"ch\":1}") is None


def test_seq_tracker_counts_lost_blocks():
    t = blocks.SeqTracker()
    assert t.feed(blocks.decode(make(seq=1))) == 0
    assert t.feed(blocks.decode(make(seq=4))) == 2
    assert t.lost[7] == 2
    t.reset()                                          # reconnect: the board starts at 0 again
    assert t.feed(blocks.decode(make(seq=0))) == 0 and t.lost[7] == 2
    assert t.feed(blocks.decode(make(seq=0xFFFFFFFF))) == 0  # going back is not a gap


def test_field_names():
    assert blocks.field_names({"fields": ["a", "b"], "n_fields": 2}) == ["a", "b"]
    assert blocks.field_names({"fields": "x,y,z", "n_fields": 3}) == ["x", "y", "z"]
    assert blocks.field_names({"n_fields": 2}) == ["f0", "f1"]
