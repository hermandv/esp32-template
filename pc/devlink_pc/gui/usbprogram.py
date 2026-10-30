"""Devices tab: "Program blank board (USB)…" dialog (SPEC §3.8, §8.1). Uses usbflash (esptool from pip, no ESP-IDF).

Pick the port, a build folder and in it the recovery and app builds; "Erase and program" first checks the board
(chip, MAC, flash size, and what it runs if it answers on the console), asks for confirmation with that, then erases,
writes, verifies, waits for the app and sets the label and Wi-Fi."""

from __future__ import annotations

import threading
from pathlib import Path

from PySide6.QtWidgets import (QComboBox, QDialog, QFileDialog, QFormLayout, QHBoxLayout, QLabel, QLineEdit,
                               QMessageBox, QPlainTextEdit, QPushButton, QVBoxLayout)

from .. import usbflash
from . import workers


def default_build_dir() -> str:
    """The nearest build/ folder above the PC app that holds a recovery build, if there is one."""
    for up in Path(__file__).resolve().parents:
        if any(b.kind == "recovery" for b in usbflash.scan_builds(up / "build")) if (up / "build").is_dir() else False:
            return str(up / "build")
    return ""


def _html(errors: list[str], warnings: list[str]) -> str:
    out = "".join(f"<br><span style='color:#c00'>{e}</span>" for e in errors)
    return out + "".join(f"<br><span style='color:#b60'>{w}</span>" for w in warnings)


class ProgramDialog(QDialog):
    def __init__(self, settings, parent=None, on_done=None):
        super().__init__(parent)
        self.setWindowTitle("Program blank board (USB)")
        self.resize(780, 600)
        self.s, self.on_done = settings, on_done
        self.fw: usbflash.Firmware | None = None
        self.builds: list[usbflash.Build] = []
        self.busy = False
        self.cancel = threading.Event()
        self.closing = False  # close once the cancelled worker has stopped

        self.port = QComboBox()
        btn_ports = QPushButton("Refresh")
        btn_ports.clicked.connect(self.refresh_ports)
        pr = QHBoxLayout()
        pr.addWidget(self.port, 1)
        pr.addWidget(btn_ports)
        self.folder = QLineEdit(settings.usb_build_dir or default_build_dir())
        self.folder.editingFinished.connect(self.load_builds)
        btn_folder = QPushButton("Browse…")
        btn_folder.clicked.connect(self.browse)
        fr = QHBoxLayout()
        fr.addWidget(self.folder, 1)
        fr.addWidget(btn_folder)
        self.rec = QComboBox()
        self.app = QComboBox()
        self.rec.currentIndexChanged.connect(self.check_fw)
        self.app.currentIndexChanged.connect(self.check_fw)
        self.fw_info = QLabel("–")
        self.fw_info.setWordWrap(True)
        self.label = QLineEdit()
        self.label.setPlaceholderText("optional, e.g. the board's number")
        self.ssid = QLineEdit(settings.bench_ssid)
        self.ssid.setPlaceholderText("empty = set Wi-Fi later")
        self.pw = QLineEdit()
        self.pw.setEchoMode(QLineEdit.Password)
        self.pw.setPlaceholderText("not stored")
        form = QFormLayout()
        form.addRow("USB port", pr)
        form.addRow("Build folder", fr)
        form.addRow("Recovery build", self.rec)
        form.addRow("App build (ota_0)", self.app)
        form.addRow("", self.fw_info)
        form.addRow("Board label", self.label)
        form.addRow("Wi-Fi SSID", self.ssid)
        form.addRow("Wi-Fi password", self.pw)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        self.btn_go = QPushButton("Erase and program…")
        self.btn_go.clicked.connect(self.go)
        self.btn_close = QPushButton("Close")
        self.btn_close.clicked.connect(self.reject)
        for b in (self.btn_go, self.btn_close, btn_ports, btn_folder):
            b.setAutoDefault(False)  # Enter must never start an erase
        br = QHBoxLayout()
        br.addStretch(1)
        br.addWidget(self.btn_go)
        br.addWidget(self.btn_close)
        v = QVBoxLayout(self)
        v.addLayout(form)
        note = QLabel("The whole flash is erased: use this for new boards, or to re-flash a board completely (its "
                      "Wi-Fi settings and label are lost). For normal updates use the Firmware tab.")
        note.setWordWrap(True)
        v.addWidget(note)
        v.addWidget(self.log, 1)
        v.addLayout(br)
        self.refresh_ports()
        self.load_builds()

    def say(self, text: str) -> None:
        self.log.appendPlainText(text)

    def refresh_ports(self) -> None:
        self.port.clear()
        for dev, desc in usbflash.usb_ports():
            self.port.addItem(desc, dev)
        if self.port.count() == 0:
            self.port.addItem("no USB-serial port found", None)

    def browse(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "Build folder (holds the recovery and app builds)",
                                             self.folder.text())
        if d:
            self.folder.setText(d)
            self.load_builds()

    def load_builds(self) -> None:
        self.builds = usbflash.scan_builds(self.folder.text()) if self.folder.text().strip() else []
        for combo, kind in ((self.rec, "recovery"), (self.app, "app")):
            combo.blockSignals(True)
            combo.clear()
            for b in sorted(self.builds, key=lambda b: (b.kind != kind, b.dir.name)):  # the fitting kind first
                combo.addItem(b.label, b)
            combo.blockSignals(False)
        app_i = next((i for i in range(self.app.count()) if self.app.itemData(i).dir.name == "app"), 0)
        self.app.setCurrentIndex(app_i)
        self.check_fw()

    def check_fw(self) -> None:
        rec, app = self.rec.currentData(), self.app.currentData()
        if rec is None or app is None:
            self.fw = None
            self.fw_info.setText("<span style='color:#c00'>no builds in this folder (each needs flasher_args.json)"
                                 "</span>")
            return
        self.fw = usbflash.make_firmware(rec, app)
        self.fw_info.setText(self.fw.summary + _html(self.fw.errors, self.fw.warnings))

    # ---- programming: check the board, confirm, program ----
    def go(self) -> None:
        port = self.port.currentData()
        if self.busy or port is None:
            return
        if self.fw is None or self.fw.errors:
            QMessageBox.warning(self, "Program board", "Pick a recovery and an app build without errors first.")
            return
        self._busy(True)
        self.say(f"checking the board on {port}…")
        fw = self.fw
        workers.run(lambda emit: self._probe(port, fw, emit), on_done=lambda bc: self._confirm(port, fw, bc),
                    on_error=self._failed, on_progress=self.say)

    @staticmethod
    def _probe(port: str, fw: usbflash.Firmware, emit) -> usbflash.BoardCheck:
        bc = usbflash.probe_board(port, emit)
        usbflash.check_board(fw, bc)
        return bc

    def _confirm(self, port: str, fw: usbflash.Firmware, bc: usbflash.BoardCheck) -> None:
        self._busy(False)
        self.say("board: " + bc.summary)
        for e in bc.errors:
            self.say("ERROR: " + e)
        if bc.errors:
            QMessageBox.warning(self, "Program board", "Not programmed:\n" + "\n".join(bc.errors))
            return
        warn = "".join(f"\n\n{w}" for w in bc.warnings)
        if QMessageBox.question(self, "Erase and program",
                                f"Erase ALL flash of the board on {port}:\n{bc.summary}\n\nand write {fw.summary}?"
                                f"{warn}") != QMessageBox.Yes:
            self.say("not programmed")
            return
        self.s.usb_build_dir = self.folder.text()
        self.s.save()
        label, ssid, pw = self.label.text().strip(), self.ssid.text().strip(), self.pw.text()
        self.cancel.clear()
        self._busy(True)

        def work(emit):
            info = usbflash.program(port, fw, emit, label=label, cancel=self.cancel)
            if ssid:
                info["url"] = usbflash.set_wifi(port, ssid, pw, emit)
            return info

        workers.run(work, on_done=self._finished, on_error=self._failed, on_progress=self.say)

    def _busy(self, on: bool) -> None:
        self.busy = on
        self.btn_go.setEnabled(not on)
        self.btn_close.setText("Cancel" if on else "Close")
        if not on and self.closing:
            super().reject()

    def reject(self) -> None:
        """Close / Cancel / Esc / window X. While programming: ask, stop esptool, close when it has stopped."""
        if not self.busy:
            super().reject()
            return
        if QMessageBox.question(self, "Cancel programming",
                                "Stop programming? A board stopped while it is being written does not start until "
                                "it is programmed again.") == QMessageBox.Yes:
            self.closing = True
            self.cancel.set()
            self.say("cancelling…")

    def closeEvent(self, e) -> None:  # noqa: N802  (the window X goes through reject as well)
        if self.busy:
            e.ignore()
            self.reject()
        else:
            super().closeEvent(e)

    # Not named done(): QDialog.done(int) is what close/reject call.
    def _finished(self, info: dict) -> None:
        self.pw.clear()
        self.say(f"DONE: {info.get('label') or info.get('id')} ({info.get('mac')})"
                 + (f" at {info['url']}" if info.get("url") else "; set Wi-Fi with 'Configure via USB…'"))
        self._busy(False)
        if self.on_done:
            self.on_done()

    def _failed(self, err: str) -> None:
        self.say("FAILED: " + err.splitlines()[0])
        self._busy(False)
