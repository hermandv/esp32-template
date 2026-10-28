"""Firmware file checks (SPEC §7.2, §8.2): the same rules the board applies (§7.1), run before uploading.

Layout: esp_image_header_t (24 B) | first segment header (8 B) | esp_app_desc_t (256 B) | ... ; the devlink marker
is a 16-byte magic somewhere in the image.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

IMG_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
DESC_OFF = 32
HEAD_LEN = DESC_OFF + 256
MARKER_MAGIC = bytes([0xD1]) + b"devlink-marker" + bytes([0x01])

CHIPS = {0: "ESP32", 2: "ESP32-S2", 5: "ESP32-C3", 9: "ESP32-S3", 12: "ESP32-C2", 13: "ESP32-C6", 16: "ESP32-H2",
         18: "ESP32-P4", 23: "ESP32-C5", 20: "ESP32-C61"}
ESP32S3 = 9


@dataclass
class ImageInfo:
    path: str = ""
    size: int = 0
    sha256: str = ""
    ok: bool = False                 # an ESP app image at all
    chip_id: int = -1
    chip: str = "?"
    min_rev: int = 0                 # major*100 + minor
    max_rev: int = 0
    flash_mb: int = 0
    project: str = ""
    version: str = ""
    date: str = ""
    time: str = ""
    idf: str = ""
    marker: bool = False
    api_version: int | None = None
    errors: list[str] = field(default_factory=list)    # block the upload
    warnings: list[str] = field(default_factory=list)  # can be overridden (force=1)

    @property
    def summary(self) -> str:
        return f"{self.project} {self.version} ({self.date} {self.time}, {self.idf}, {self.chip})"


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode(errors="replace")


def parse(data: bytes, path: str = "", chip_rev: int | None = None, flash_size: int | None = None,
          slot_size: int = 0x300000, project_prefix: str = "") -> ImageInfo:
    """Parse and check an app image. chip_rev / flash_size: the target device's, if known."""
    i = ImageInfo(path=path, size=len(data), sha256=hashlib.sha256(data).hexdigest())
    if len(data) < HEAD_LEN:
        i.errors.append("file too small to be an app image")
        return i
    if data[0] != IMG_MAGIC:
        i.errors.append("not an ESP image (ELF or another file?)")
        return i
    i.chip_id = struct.unpack_from("<H", data, 12)[0]
    i.chip = CHIPS.get(i.chip_id, f"chip id {i.chip_id}")
    i.min_rev, i.max_rev = struct.unpack_from("<HH", data, 15)
    i.flash_mb = 1 << (data[3] >> 4)
    if struct.unpack_from("<I", data, DESC_OFF)[0] != APP_DESC_MAGIC:
        i.errors.append("no app descriptor: a bootloader or merged image, not an app")
        return i
    d = data[DESC_OFF:DESC_OFF + 256]
    i.version, i.project = _cstr(d[16:48]), _cstr(d[48:80])
    i.time, i.date, i.idf = _cstr(d[80:96]), _cstr(d[96:112]), _cstr(d[112:144])
    i.ok = True
    m = data.find(MARKER_MAGIC)
    if m >= 0:
        i.marker = True
        if m + 20 <= len(data):
            i.api_version = struct.unpack_from("<I", data, m + 16)[0]

    if i.chip_id != ESP32S3:
        i.errors.append(f"built for {i.chip}, not ESP32-S3")
    if len(data) > slot_size:
        i.errors.append(f"{len(data)} bytes, larger than the app slot ({slot_size})")
    if chip_rev is not None and not (i.min_rev <= chip_rev <= i.max_rev):
        i.errors.append(f"needs chip revision {i.min_rev / 100:.2f}..{i.max_rev / 100:.2f}, board is {chip_rev / 100:.2f}")
    if flash_size is not None and i.flash_mb * 1024 * 1024 > flash_size:
        i.errors.append(f"built for {i.flash_mb} MB flash, board has {flash_size // (1024 * 1024)} MB")
    if not i.marker:
        i.warnings.append("no devlink marker: not a devlink build (the board would be reachable only via the "
                          "quick-reset escape)")
    if project_prefix and not i.project.startswith(project_prefix):
        i.warnings.append(f"project '{i.project}' does not start with '{project_prefix}'")
    return i


def parse_file(path: str | Path, **kw) -> ImageInfo:
    p = Path(path)
    return parse(p.read_bytes(), str(p), **kw)
