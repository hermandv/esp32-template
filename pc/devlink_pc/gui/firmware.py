"""Firmware tab (SPEC §8.2): pick a .bin, see what it is, block wrong files, update selected boards one at a time
(§7.2) with per-board progress and result."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QFileDialog, QFormLayout, QGroupBox,
                               QHBoxLayout, QHeaderView, QLabel, QListWidget, QListWidgetItem, QMessageBox,
                               QPushButton, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from .. import image, ota
from ..settings import Settings
from . import workers


class FirmwareTab(QWidget):
    def __init__(self, settings: Settings, devices, parent=None):
        super().__init__(parent)
        self.s, self.devices = settings, devices
        self.data: bytes | None = None
        self.info: image.ImageInfo | None = None
        self.running = False

        self.recent = QComboBox()
        self.recent.setMinimumWidth(420)
        self.recent.activated.connect(lambda i: self.load(self.recent.itemData(i)))
        pick = QPushButton("Open .bin…")
        pick.clicked.connect(self.pick)
        self.details = QLabel("No file selected.")
        self.details.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.details.setWordWrap(True)
        self.force = QCheckBox("Upload anyway (override the warnings above)")
        self.force.setEnabled(False)
        self.target = QComboBox()
        self.target.addItem("app (default)", "app")
        self.target.addItem("recovery (rare, guarded)", "recovery")
        self.target.currentIndexChanged.connect(self._target_changed)

        self.boards = QListWidget()
        self.boards.setSelectionMode(QAbstractItemView.NoSelection)
        self.start_btn = QPushButton("Update selected boards")
        self.start_btn.clicked.connect(self.start)
        self.results = QTableWidget(0, 4)
        self.results.setHorizontalHeaderLabels(["Board", "Phase", "Progress", "Result"])
        self.results.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.results.horizontalHeader().setStretchLastSection(True)
        self.results.setEditTriggers(QAbstractItemView.NoEditTriggers)

        file_box = QGroupBox("Firmware file")
        fl = QFormLayout(file_box)
        row = QHBoxLayout()
        row.addWidget(self.recent, 1)
        row.addWidget(pick)
        fl.addRow("File", row)
        fl.addRow("", self.details)
        fl.addRow("", self.force)
        fl.addRow("Target", self.target)
        dev_box = QGroupBox("Boards (one at a time, stops at the first failure)")
        dl = QVBoxLayout(dev_box)
        dl.addWidget(self.boards)
        dl.addWidget(self.start_btn)
        lay = QVBoxLayout(self)
        lay.addWidget(file_box)
        lay.addWidget(dev_box, 1)
        lay.addWidget(QLabel("Results"))
        lay.addWidget(self.results, 1)

        self._fill_recent()
        devices.devices_changed.connect(self.refresh_boards)
        self.refresh_boards()

    # ---- file ----
    def is_recovery(self) -> bool:
        return self.target.currentData() == "recovery"

    def _target_changed(self) -> None:
        self._fill_recent()
        self.data, self.info = None, None
        self.details.setText("Pick a recovery image (devlink_recovery*.bin)." if self.is_recovery() else
                             "No file selected.")

    def _fill_recent(self) -> None:
        self.recent.clear()
        for p in (self.s.recent_recovery if self.is_recovery() else self.s.recent_app):
            self.recent.addItem(p, p)

    def pick(self) -> None:
        start = str(Path(self.s.recent_app[0]).parent) if self.s.recent_app else ""
        path, _ = QFileDialog.getOpenFileName(self, "Firmware image", start, "App image (*.bin);;All files (*)")
        if path:
            self.load(path)

    def load(self, path: str) -> None:
        try:
            self.data = Path(path).read_bytes()
        except OSError as e:
            QMessageBox.warning(self, "Firmware file", str(e))
            return
        self.info = image.parse(self.data, path, slot_size=ota.FACTORY_SIZE if self.is_recovery() else 0x300000)
        i = self.info
        if self.is_recovery() and i.ok and "recovery" not in i.project:
            i.warnings.append(f"'{i.project}' is not a recovery image")
        lines = [f"<b>{i.project or '?'}</b> {i.version}  &nbsp; built {i.date} {i.time} &nbsp; {i.idf}",
                 f"chip {i.chip}, chip revision {i.min_rev / 100:.2f}..{i.max_rev / 100:.2f}, {i.flash_mb} MB flash, "
                 f"{i.size} bytes, devlink marker {'API ' + str(i.api_version) if i.marker else 'missing'}",
                 f"<small>SHA-256 {i.sha256[:16]}…</small>"]
        lines += [f"<span style='color:#b00'>✖ {e}</span>" for e in i.errors]
        lines += [f"<span style='color:#a60'>⚠ {w}</span>" for w in i.warnings]
        self.details.setText("<br>".join(lines))
        self.force.setChecked(False)
        self.force.setEnabled(bool(i.warnings) and not i.errors)
        if i.ok and not i.errors:
            self.s.remember_file(path, recovery=self.is_recovery())
            self.s.save()
            self._fill_recent()
            self.recent.setCurrentIndex(0)

    # ---- boards ----
    def refresh_boards(self) -> None:
        checked = {self.boards.item(i).data(Qt.UserRole) for i in range(self.boards.count())
                   if self.boards.item(i).checkState() == Qt.Checked}
        self.boards.clear()
        for r in sorted(self.devices.lan_devices(), key=lambda r: r.label or r.mac):
            it = QListWidgetItem(f"{r.label or '(no name)'}  {r.mac}  {r.ip}  {r.mode} {r.app}")
            it.setData(Qt.UserRole, r.mac)
            it.setFlags(it.flags() | Qt.ItemIsUserCheckable)
            it.setCheckState(Qt.Checked if r.mac in checked else Qt.Unchecked)
            self.boards.addItem(it)

    def selected_macs(self) -> list[str]:
        return [self.boards.item(i).data(Qt.UserRole) for i in range(self.boards.count())
                if self.boards.item(i).checkState() == Qt.Checked]

    # ---- update ----
    def start(self) -> None:
        if self.running:
            return
        macs = self.selected_macs()
        if self.data is None or self.info is None or not macs:
            QMessageBox.information(self, "Update", "Pick a firmware file and at least one board.")
            return
        if self.info.errors:
            QMessageBox.warning(self, "Update", "This file cannot be uploaded:\n" + "\n".join(self.info.errors))
            return
        if self.info.warnings and not self.force.isChecked():
            QMessageBox.warning(self, "Update", "\n".join(self.info.warnings) + "\n\nTick 'Upload anyway' to override.")
            return
        if self.info.warnings and QMessageBox.question(
                self, "Upload anyway?", "\n".join(self.info.warnings) + "\n\nUpload this file anyway?") != QMessageBox.Yes:
            return
        rec = self.is_recovery()
        if rec and QMessageBox.warning(
                self, "Recovery update",
                "Replace the RECOVERY image (the safety net) on the selected board(s)?\n\n"
                "Only from a valid, running app. The board then boots the new recovery once as a self-test and "
                "returns to the app. If the self-test fails, the board has no working recovery until you "
                "re-flash a good one.", QMessageBox.Yes | QMessageBox.Cancel) != QMessageBox.Yes:
            return
        self.results.setRowCount(len(macs))
        self.row_of = {}
        for i, m in enumerate(macs):
            self.row_of[m] = i
            for c, v in enumerate([m, "waiting", "", ""]):
                self.results.setItem(i, c, QTableWidgetItem(v))
        hints = {r.mac: r.ip for r in self.devices.lan_devices()}
        self.running = True
        self.start_btn.setEnabled(False)
        data, force, key, mdns = self.data, self.force.isChecked(), self.s.api_key, self.devices.mdns
        step = ota.update_recovery if rec else ota.update_device

        def batch(progress):
            out = []
            for mac in macs:  # one board at a time, stop at the first failure (SPEC §7.2)
                r = step(mac, data, key, force, hints.get(mac), mdns, progress)
                progress(r)
                out.append(r)
                if not r.ok:
                    break
            return out

        workers.run(batch, on_progress=self._progress, progress_kw="progress", on_done=self._done,
                    on_error=self._error)

    def _progress(self, p) -> None:
        if isinstance(p, tuple) and len(p) == 4:
            mac, phase, done, total = p
            row = self.row_of.get(mac)
            if row is None:
                return
            self.results.item(row, 1).setText(phase)
            self.results.item(row, 2).setText(f"{100 * done // total} %" if total else "")
        elif hasattr(p, "mac"):  # an OtaResult
            row = self.row_of.get(p.mac)
            if row is not None:
                self.results.item(row, 1).setText(p.state)
                self.results.item(row, 3).setText(p.message)
                if p.state in ("selftest_failed", "rolled_back"):
                    for c in range(4):
                        self.results.item(row, c).setBackground(QColor(255, 190, 190))

    def _done(self, results) -> None:
        self.running = False
        self.start_btn.setEnabled(True)
        for r in results:
            self._progress(r)
        failed = [r for r in results if r.state == "selftest_failed"]
        if failed and len(self.s.recent_recovery) > 1:
            prev = self.s.recent_recovery[1]
            if QMessageBox.critical(
                    self, "Recovery self-test failed",
                    f"The new recovery failed its self-test on {', '.join(r.mac for r in failed)}.\n\n"
                    f"Re-flash the previous recovery file?\n{prev}",
                    QMessageBox.Yes | QMessageBox.No) == QMessageBox.Yes:
                self.load(prev)
                self.start()
                return
        elif failed:
            QMessageBox.critical(self, "Recovery self-test failed", "The new recovery failed its self-test. Pick a "
                                 "known-good recovery file and update again.")
        for m, row in self.row_of.items():
            if self.results.item(row, 1).text() == "waiting":
                self.results.item(row, 1).setText("skipped")
                self.results.item(row, 3).setText("not started: an earlier board failed")
        self.devices.refresh()

    def _error(self, err: str) -> None:
        self.running = False
        self.start_btn.setEnabled(True)
        QMessageBox.warning(self, "Update", err.splitlines()[0])
