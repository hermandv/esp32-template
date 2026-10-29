"""Main window: Devices and Firmware tabs (SPEC §8.1, §8.2); Monitor follows in milestone 6. Settings (§8.4)."""

from __future__ import annotations

import sys

from PySide6.QtWidgets import (QApplication, QCheckBox, QDialog, QDialogButtonBox, QFormLayout, QLabel, QLineEdit,
                               QMainWindow, QSpinBox, QTabWidget)

from .. import __version__
from ..settings import Settings, data_dir
from .devices import DevicesTab
from .firmware import FirmwareTab
from .monitor import MonitorTab


class SettingsDialog(QDialog):
    def __init__(self, s: Settings, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Settings")
        self.s = s
        self.key = QLineEdit(s.api_key)
        self.ap_pw = QLineEdit(s.ap_password)
        self.ssid = QLineEdit(s.bench_ssid)
        self.disc = QSpinBox(minimum=2, maximum=120, value=s.discovery_s)
        self.aps = QSpinBox(minimum=5, maximum=300, value=s.ap_scan_s)
        self.unconf = QCheckBox()
        self.unconf.setChecked(s.scan_for_unconfigured)
        f = QFormLayout(self)
        f.addRow("API key (fixed at build time)", self.key)
        f.addRow("Config-AP password", self.ap_pw)
        f.addRow("Bench Wi-Fi SSID", self.ssid)
        f.addRow(QLabel("The bench Wi-Fi password is never stored; the configure dialog asks for it."))
        f.addRow("Discovery every (s)", self.disc)
        f.addRow("Look for config APs every (s)", self.aps)
        f.addRow("Look for unconfigured boards", self.unconf)
        f.addRow("Data folder", QLabel(str(data_dir())))
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.accepted.connect(self.accept)
        bb.rejected.connect(self.reject)
        f.addRow(bb)

    def apply(self) -> None:
        s = self.s
        s.api_key, s.ap_password, s.bench_ssid = self.key.text(), self.ap_pw.text(), self.ssid.text()
        s.discovery_s, s.ap_scan_s, s.scan_for_unconfigured = self.disc.value(), self.aps.value(), self.unconf.isChecked()
        s.save()


class MainWindow(QMainWindow):
    def __init__(self, settings: Settings | None = None, autostart: bool = True):
        super().__init__()
        self.s = settings or Settings.load()
        self.setWindowTitle(f"devlink {__version__}")
        self.resize(1200, 720)
        self.devices = DevicesTab(self.s)
        self.firmware = FirmwareTab(self.s, self.devices)
        tabs = QTabWidget()
        tabs.addTab(self.devices, "Devices")
        tabs.addTab(self.firmware, "Firmware")
        self.monitor = MonitorTab(self.s, self.devices)
        tabs.addTab(self.monitor, "Monitor")
        self.setCentralWidget(tabs)
        m = self.menuBar().addMenu("&File")
        m.addAction("Settings…", self.open_settings)
        m.addSeparator()
        m.addAction("Quit", self.close)
        self.statusBar().showMessage("Looking for boards…")
        self.devices.devices_changed.connect(
            lambda: self.statusBar().showMessage(f"{len(self.devices.lan_devices())} board(s) on the network, "
                                                 f"{len(self.devices.rows) - len(self.devices.lan_devices())} to configure"))
        if autostart:
            self.devices.start()

    def open_settings(self) -> None:
        d = SettingsDialog(self.s, self)
        if d.exec() == QDialog.Accepted:
            d.apply()

    def closeEvent(self, e) -> None:  # noqa: N802
        self.monitor.stop()
        self.devices.stop()
        super().closeEvent(e)


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("devlink")
    # Fusion: the same look everywhere (the Windows 11 style draws an accent bar in every selected table cell).
    app.setStyle("Fusion")
    w = MainWindow()
    w.show()
    return app.exec()
