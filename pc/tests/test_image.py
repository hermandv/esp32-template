"""Firmware file checks (SPEC §7.2, §8.2), on the real builds and mutated copies."""
import struct
from pathlib import Path

import pytest

from devlink_pc import image

ROOT = Path(__file__).resolve().parents[2]
BOOTLAB = ROOT / "build" / "bootlab" / "bootlab.bin"


@pytest.fixture
def app() -> bytes:
    if not BOOTLAB.exists():
        pytest.skip("build/bootlab not built")
    return BOOTLAB.read_bytes()


def test_real_image(app):
    i = image.parse(app, chip_rev=1, flash_size=8 << 20)
    assert i.ok and i.chip == "ESP32-S3" and i.marker and i.api_version == 1
    assert i.project == "bootlab" and i.version and i.idf.startswith("v6.0")
    assert not i.errors and not i.warnings


def test_rejections(app):
    assert "not an ESP image" in image.parse(b"\x7fELF" + app[4:]).errors[0]
    c3 = app[:12] + struct.pack("<H", 5) + app[14:]
    assert "ESP32-C3" in image.parse(c3).errors[0]
    rev = app[:15] + struct.pack("<H", 300) + app[17:]
    assert any("revision" in e for e in image.parse(rev, chip_rev=1).errors)
    big = app[:3] + bytes([(app[3] & 0x0F) | 0x40]) + app[4:]
    assert any("16 MB" in e for e in image.parse(big, flash_size=8 << 20).errors)
    no_desc = app[:32] + b"\0" * 4 + app[36:]
    assert "no app descriptor" in image.parse(no_desc).errors[0]
    assert image.parse(app, slot_size=1000).errors


def test_bootloader_rejected():
    bl = ROOT / "build" / "recovery" / "bootloader" / "bootloader.bin"
    if not bl.exists():
        pytest.skip("recovery not built")
    assert "no app descriptor" in image.parse(bl.read_bytes()).errors[0]


def test_foreign_image_is_a_warning(app):
    foreign = app.replace(image.MARKER_MAGIC, b"\0" * 16)
    i = image.parse(foreign)
    assert not i.errors and not i.marker and "no devlink marker" in i.warnings[0]


def test_prefix_warning(app):
    assert image.parse(app, project_prefix="devlink").warnings
