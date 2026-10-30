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


def test_monitor_tab(win):
    m = win.monitor
    m.channels = {"log": {"name": "log"}, "sys.temp": {"name": "sys.temp", "unit": "degC", "default_hz": 1, "max_hz": 10},
                  "example.adc": {"name": "example.adc", "unit": "counts", "default_hz": 5, "max_hz": 50}}
    m.fill_channels()
    assert m.table.rowCount() == 2
    m.handle_frames([{"ch": "sys.temp", "ts": 1, "v": 41.5}, {"ch": "example.adc", "ts": 2, "v": [1, 2, 3]},
                     {"ch": "log", "ts": 3000000, "lvl": "W", "tag": "wifi", "msg": "STA down"},
                     {"ch": "log", "ts": 3100000, "lvl": "D", "tag": "x", "msg": "debug noise"}])
    assert m.table.item(m._row("sys.temp"), 4).text() == "41.5"
    assert set(m.series) == {"sys.temp", "example.adc[0]", "example.adc[1]", "example.adc[2]"}
    x, y = m.series["example.adc[1]"].getData()
    assert list(y) == [2.0] and len(x) == 1  # pyqtgraph curve holds the point
    m.handle_frames([{"ch": "sys.temp", "ts": 2, "v": 42.0}])
    assert list(m.series["sys.temp"].getData()[1]) == [41.5, 42.0]
    m.follow.setChecked(False)  # zoomed/panned: the view no longer follows
    m.handle_frames([{"ch": "sys.temp", "ts": 3, "v": 43.0}])
    m.follow.setChecked(True)
    m.all_btn.click()  # show all: x spans every point and keeps growing, y fits
    assert m.show_all and not m.follow.isChecked()
    m.handle_frames([{"ch": "sys.temp", "ts": 4, "v": 50.0}])
    m.plot.getViewBox().updateAutoRange()  # y auto-range is applied lazily (on the next paint)
    (x0, x1), (y0, y1) = m.plot.getViewBox().viewRange()
    xs = m.series["sys.temp"].getData()[0]
    assert x0 <= xs.min() and x1 >= xs.max() and y1 >= 50.0
    m.on_manual_range()  # zoom/pan with the mouse leaves both modes
    assert not m.show_all and not m.follow.isChecked()
    m.follow.setChecked(True)
    text = m.log.toPlainText()
    assert "STA down" in text and "debug noise" not in text  # default filter: info and above
    m.filter.setText("nomatch")
    assert m.log.toPlainText() == ""
    m.filter.setText("")
    m.level.setCurrentIndex(1)  # debug and above
    assert "debug noise" in m.log.toPlainText()


def test_monitor_unsubscribe_removes_curves(win):
    m = win.monitor
    m.session = type("S", (), {"unsubscribe": lambda self, ch: None})()
    m.handle_frames([{"ch": "example.adc", "ts": 1, "v": [1, 2]}, {"ch": "sys.temp", "ts": 1, "v": 40.0}])
    assert len(m.plot.getPlotItem().listDataItems()) == 3
    m.set_sub("example.adc", False)
    assert set(m.series) == {"sys.temp"} and len(m.plot.getPlotItem().listDataItems()) == 1
    m.session = None


def test_monitor_block_channel(win):
    import numpy as np

    from devlink_pc import blocks
    m = win.monitor
    m.channels = {"example.wave": {"name": "example.wave", "type": "block", "id": 4, "n_fields": 2,
                                   "fields": ["sine", "count"], "default_hz": 200, "max_hz": 1000}}
    m.fill_channels()
    spin = m.table.cellWidget(0, 3)
    assert spin.minimum() == 1 and spin.maximum() == 1000 and spin.value() == 200
    vals = np.array([[0.5, 0], [0.25, 1], [0.0, 2]])
    m.handle_blocks([blocks.Block(4, 0, 0, 10_000_000, 5_000_000, vals),
                     blocks.Block(4, 1, 0, 10_015_000, 5_000_000, vals + [0, 3]),
                     blocks.Block(9, 0, 0, 0, 1000, vals)])          # unknown channel id: ignored
    assert set(m.series) == {"example.wave[sine]", "example.wave[count]"}
    t, v = m.series_data("example.wave[count]")
    assert list(v) == [0, 1, 2, 3, 4, 5] and np.allclose(np.diff(t), 0.005)
    m.redraw(t[-1])
    m.session = type("S", (), {"unsubscribe": lambda self, ch: None})()
    m.set_sub("example.wave", False)
    assert not m.series and not m.bpoints
    m.session = None


def test_program_dialog(win, monkeypatch, tmp_path):
    from PySide6.QtWidgets import QMessageBox

    from devlink_pc import usbflash
    from devlink_pc.gui import usbprogram
    if not all((ROOT / "build" / n / "flasher_args.json").is_file() for n in ("recovery", "app")):
        pytest.skip("build/recovery and build/app not built")
    monkeypatch.setattr(usbflash, "usb_ports", lambda: [("COM77", "COM77: USB JTAG/serial debug unit")])
    win.devices.s.usb_build_dir = str(ROOT / "build")
    d = usbprogram.ProgramDialog(win.devices.s)
    assert d.rec.currentData().kind == "recovery" and d.app.currentData().dir.name == "app"
    assert d.fw is not None and not d.fw.errors and "8MB" in d.fw_info.text()
    d.app.setCurrentIndex(next(i for i in range(d.app.count()) if d.app.itemData(i).kind == "recovery"))
    assert d.fw.errors and "recovery build" in d.fw_info.text()
    d.load_builds()

    # board check -> confirmation (answered No): nothing is written
    asked, programmed = [], []
    monkeypatch.setattr(usbflash, "probe_board",
                        lambda port, log: usbflash.BoardCheck(port, "ESP32-S3", "7C:DF:A1:F2:50:30", "8MB",
                                                              {"label": "bench", "fw": {}}))
    monkeypatch.setattr(usbflash, "program", lambda *a, **k: programmed.append(a))
    monkeypatch.setattr(QMessageBox, "question", lambda *a, **k: asked.append(a[2]) or QMessageBox.No)
    bc = d._probe("COM77", d.fw, print)
    d._confirm("COM77", d.fw, bc)
    assert asked and "7C:DF:A1:F2:50:30" in asked[0] and "runs devlink" in asked[0] and not programmed
    assert "not programmed" in d.log.toPlainText() and not d.busy
    d.close()
