"""GUI smoke test (offscreen): the window builds, discovery results and config APs show up, highlights work, the
Firmware tab parses a file and lists boards. No network."""
import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtWidgets import QApplication  # noqa: E402

from devlink_pc.discovery import Seen  # noqa: E402
from devlink_pc.gui.main import MainWindow  # noqa: E402
from devlink_pc.settings import Settings  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def app():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def win(app, tmp_path, monkeypatch):
    monkeypatch.setattr(Settings, "save", lambda self, path=None: None)
    w = MainWindow(Settings(), autostart=False)
    yield w
    w.devices.stop()


def seen(mac, ip, boot="normal", label="bench"):
    return Seen(mac, ip, {"id": mac.replace(":", "")[-6:], "label": label, "mode": "app", "fw": "1.0",
                          "rec": "0.2", "boot": boot}, "udp", 0)


def test_devices_table(win):
    d = win.devices
    d.apply_seen([(seen("7C:DF:A1:F2:50:30", "10.0.0.5"), {"wifi": {"sta_connected": True, "rssi": -50}}),
                  (seen("AA:BB:CC:DD:EE:01", "10.0.0.6", boot="rollback", label="psu"), None)])
    d.apply_aps([("ESP32S3-CFG-0A0B0C", "CFG", "0A0B0C"), ("ESP32S3-CFG-F25030", "CFG", "F25030")])
    assert d.table.rowCount() == 3  # the second AP belongs to a board already on the LAN
    texts = {d.table.item(r, 2).text(): r for r in range(d.table.rowCount())}
    assert d.table.item(texts["psu"], 0).background().color().red() == 255  # rollback highlighted
    needs = [d.table.item(r, 5).text() for r in range(3)].count("needs-config")
    assert needs == 1 and len(d.lan_devices()) == 2
    d.apply_aps([])
    assert d.table.rowCount() == 2


def test_firmware_tab(win):
    p = ROOT / "build" / "bootlab" / "bootlab.bin"
    if not p.exists():
        pytest.skip("build/bootlab not built")
    win.devices.apply_seen([(seen("7C:DF:A1:F2:50:30", "10.0.0.5"), None)])
    f = win.firmware
    f.load(str(p))
    assert "bootlab" in f.details.text() and "API 1" in f.details.text() and not f.force.isEnabled()
    assert f.boards.count() == 1
