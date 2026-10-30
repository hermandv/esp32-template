"""Programming blank boards over USB (usbflash, SPEC §3.8): picking builds, checks before erasing, otadata, the
console line reader, cancelling esptool. No board: esptool and the serial port are replaced."""

import shutil
import sys
import threading
import time
from pathlib import Path

import pytest

from devlink_pc import improv, usbflash

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
sys.path.insert(0, str(ROOT / "tools"))


def need(*names):
    for n in names:
        if not (BUILD / n / "flasher_args.json").is_file():
            pytest.skip(f"build/{n} not built")


def copy_build(name: str, dest: Path, flash_size: str | None = None) -> Path:
    """A copy of a real build directory (only what usbflash reads), optionally claiming another flash size."""
    src, d = BUILD / name, dest / name
    for rel in ("flasher_args.json", "bootloader/bootloader.bin", "bootloader/project_description.json",
                "partition_table/partition-table.bin"):
        if (src / rel).is_file():
            (d / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src / rel, d / rel)
    fa = (src / "flasher_args.json").read_text()
    app = usbflash.load_build(src).flasher["app"]["file"]
    shutil.copy(src / app, d / app)
    if flash_size:
        (d / "flasher_args.json").write_text(fa.replace('"8MB"', f'"{flash_size}"'))
    return d


def test_otadata_matches_flash_all():
    import devlink_tools
    assert usbflash.otadata_image(0) == devlink_tools.otadata_image(0)
    assert usbflash.otadata_image(1) == devlink_tools.otadata_image(1)


def test_scan_and_pick_builds(tmp_path):
    need("recovery", "app")
    for n in ("recovery", "app"):
        copy_build(n, tmp_path)
    builds = usbflash.scan_builds(tmp_path)
    assert [b.kind for b in builds] == ["recovery", "app"]
    rec, app = builds
    assert rec.devlink_bootloader is True and rec.flash_size == "8MB"
    fw = usbflash.make_firmware(rec, app)
    assert not fw.errors, fw.errors
    assert fw.partitions["factory"][0] == 0x20000 and len(fw.files) == 4
    assert fw.files[0][1].name == "bootloader.bin" and fw.files[3][1] == app.dir / app.flasher["app"]["file"]
    assert usbflash.scan_builds(tmp_path / "app")[0].kind == "app"   # a build directory itself
    assert usbflash.scan_builds(tmp_path / "nope") == []


def test_wrong_picks_are_refused(tmp_path):
    need("recovery", "app")
    rec = usbflash.load_build(copy_build("recovery", tmp_path))
    app = usbflash.load_build(copy_build("app", tmp_path))
    assert any("is a recovery build" in e for e in usbflash.make_firmware(rec, rec).errors)
    assert any("not a recovery build" in e for e in usbflash.make_firmware(app, app).errors)
    small = usbflash.load_build(copy_build("app", tmp_path / "4mb", flash_size="4MB"))
    assert any("app built for 4MB" in e for e in usbflash.make_firmware(rec, small).errors)
    (rec.dir / "bootloader" / "project_description.json").write_text('{"build_components": ["main"]}')
    rec2 = usbflash.load_build(rec.dir)
    assert any("not the devlink bootloader" in e for e in usbflash.make_firmware(rec2, app).errors)
    with pytest.raises(ValueError):
        usbflash.program("COM0", usbflash.make_firmware(rec2, app), print)


def test_check_board():
    fw = type("F", (), {"flash_size": "8MB"})()
    bc = usbflash.BoardCheck("COM9", chip="ESP32-S3 (QFN56) (revision v0.1)", mac="7C:DF:A1:F2:50:30",
                             flash_size="4MB")
    usbflash.check_board(fw, bc)
    assert any("the board has 4MB" in e for e in bc.errors)
    bc = usbflash.BoardCheck("COM9", chip="ESP32-S3", flash_size="16MB", info={"label": "bench", "fw": {}})
    usbflash.check_board(fw, bc)
    assert not bc.errors and len(bc.warnings) == 2 and "runs devlink" in bc.summary
    bc = usbflash.BoardCheck("COM9", chip="ESP32-C3", flash_size="8MB")
    usbflash.check_board(fw, bc)
    assert any("not an ESP32-S3" in e for e in bc.errors)


ESPTOOL_FLASH_ID = """esptool v5.6.0
Connected to ESP32-S3 on COM22:
Chip type:          ESP32-S3 (QFN56) (revision v0.1)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 2MB (AP_3v3)
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG
MAC:                7c:df:a1:f2:50:30
Detected flash size: 8MB
Hard resetting via RTS pin..."""


def test_probe_board(monkeypatch):
    class FakePort:
        def __init__(self, port):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *a):
            pass

        def line(self, text, expect, timeout=3.0):
            return 'devlink info {"mac":"7C:DF:A1:F2:50:30","mode":"app","label":"bench","fw":{"project":"x"}}'

    monkeypatch.setattr(usbflash, "ImprovPort", FakePort)
    monkeypatch.setattr(usbflash, "_esptool", lambda args, log, cancel=None: ESPTOOL_FLASH_ID)
    bc = usbflash.probe_board("COM22", print)
    assert bc.chip.startswith("ESP32-S3") and bc.mac == "7C:DF:A1:F2:50:30" and bc.flash_size == "8MB"
    assert bc.info["label"] == "bench"


def test_console_line_waits_for_the_whole_line():
    class FakeSerial:
        def __init__(self):
            self.chunks = [b"boot noise\r\ndevlink info {\"a\":", b"1,\"b\":[1,2]", b"}\r\nmore"]

        def reset_input_buffer(self):
            pass

        def write(self, data):
            pass

        def read(self, n):
            return self.chunks.pop(0) if self.chunks else b""

    p = improv.ImprovPort.__new__(improv.ImprovPort)
    p.ser = FakeSerial()
    assert p.line("devlink info", "devlink info {", timeout=1) == 'devlink info {"a":1,"b":[1,2]}'


def test_cancel_kills_esptool(monkeypatch):
    monkeypatch.setattr(usbflash, "esptool_cmd",
                        lambda: [sys.executable, "-c", "import time; print('writing', flush=True); time.sleep(30)"])
    cancel = threading.Event()
    lines = []
    threading.Timer(1.0, cancel.set).start()
    t0 = time.time()
    with pytest.raises(usbflash.Cancelled):
        usbflash._esptool(["write-flash"], lines.append, cancel)
    assert time.time() - t0 < 5 and "writing" in lines


def test_esptool_failure_is_readable(monkeypatch):
    monkeypatch.setattr(usbflash, "esptool_cmd", lambda: [sys.executable, "-c", "import sys; sys.exit(2)"])
    with pytest.raises(RuntimeError, match="esptool failed"):
        usbflash._esptool(["flash-id"], print)


def test_wifi_failure_keeps_a_good_flash(monkeypatch):
    monkeypatch.setattr(usbflash, "program", lambda *a, **k: {"mac": "7C:DF:A1:F2:50:30", "label": "b"})

    def bad_wifi(*a):
        raise RuntimeError("board: unable to connect")

    monkeypatch.setattr(usbflash, "set_wifi", bad_wifi)
    info = usbflash.program_and_join("COM9", None, print, label="b", ssid="10X", password="x")
    assert info["mac"] and info["wifi_error"] == "board: unable to connect" and "url" not in info
    monkeypatch.setattr(usbflash, "set_wifi", lambda *a: "http://10.0.0.5/")
    assert usbflash.program_and_join("COM9", None, print, ssid="10X")["url"] == "http://10.0.0.5/"


def test_find_boards_label_is_not_the_hostname(monkeypatch):
    names = iter(["esp-f25030", "bench 4"])

    class Port:
        def __init__(self, dev, vid):
            self.device, self.vid, self.serial_number = dev, vid, "x"

    class FakeImprov:
        def __init__(self, port):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *a):
            pass

        def info(self):
            return ["devlink_app", "0.8.0", "ESP32-S3", next(names)]

    monkeypatch.setattr(improv.list_ports, "comports",
                        lambda: [Port("COM1", 0x303A), Port("COM2", 0x10C4), Port("COM3", 0x1234)])
    monkeypatch.setattr(improv, "ImprovPort", FakeImprov)
    b = improv.find_boards()
    assert [(x.port, x.name, x.label) for x in b] == [("COM1", "esp-f25030", ""), ("COM2", "bench 4", "bench 4")]
