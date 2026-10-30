"""Devices tab (SPEC §8.1): boards on the LAN (mDNS + UDP + add-by-IP, keyed by MAC), unconfigured boards seen as
config APs, the configure flow (AP or USB), and per-board actions."""

from __future__ import annotations

import time
from dataclasses import dataclass, field

from PySide6.QtCore import Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QAction, QColor, QDesktopServices
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QDialog, QDialogButtonBox, QFileDialog, QFormLayout,
                               QHBoxLayout, QHeaderView, QInputDialog, QLabel, QLineEdit, QListWidget, QMenu,
                               QMessageBox, QPlainTextEdit, QPushButton, QTableWidget, QTableWidgetItem, QToolButton,
                               QVBoxLayout, QWidget)

from .. import configure, discovery, improv, wifi_win
from ..api import ApiError, DeviceApi
from ..settings import Settings, data_dir
from . import workers
from .usbprogram import ProgramDialog

HIGHLIGHT = {"rollback", "crash_loop", "quick_reset", "recovery_loop", "no_valid_app"}
COLUMNS = ["MAC", "ID", "Label", "IP", "Hostname", "Mode", "Boot reason", "App", "Recovery", "RSSI", "Last seen"]


@dataclass
class Row:
    key: str                     # MAC, or "AP:<ssid>" for an unconfigured board
    mac: str = ""
    id: str = ""
    label: str = ""
    ip: str = ""
    mode: str = ""               # app / recovery / needs-config
    boot: str = ""
    app: str = ""
    rec: str = ""
    rssi: str = ""
    last_seen: float = 0.0
    ap_ssid: str = ""
    extra: dict = field(default_factory=dict)

    @property
    def hostname(self) -> str:
        return f"esp-{self.id.lower()}" if self.id else ""

    @property
    def on_lan(self) -> bool:
        return bool(self.ip) and self.mode != "needs-config"


def discover_all(s: Settings, mdns: discovery.MdnsBrowser | None) -> list[tuple[discovery.Seen, dict | None]]:
    """UDP + mDNS + manual IPs; then a quick /status per board for the RSSI."""
    found: dict[str, discovery.Seen] = {}
    for x in discovery.udp_scan(1.5) + (mdns.snapshot() if mdns else []):
        found.setdefault(x.mac, x)
    for ip in s.manual_ips:
        x = discovery.probe_ip(ip, timeout=2)
        if x:
            found[x.mac] = x
    out = []
    for x in found.values():
        try:
            st = DeviceApi(x.ip, x.mac, s.api_key, timeout=1.5).status()
        except Exception:  # noqa: BLE001
            st = None
        out.append((x, st))
    return out


class ConfigureDialog(QDialog):
    """SSID / password / name, then runs the flow and shows its log."""

    def __init__(self, parent, title: str, ssid: str, label: str):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.ssid = QLineEdit(ssid)
        self.password = QLineEdit()
        self.password.setEchoMode(QLineEdit.Password)
        self.label = QLineEdit(label)
        self.label.setMaxLength(32)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMinimumHeight(140)
        form = QFormLayout()
        form.addRow("Wi-Fi SSID", self.ssid)
        form.addRow("Password", self.password)
        form.addRow("Friendly name", self.label)
        self.buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Close)
        self.buttons.button(QDialogButtonBox.Ok).setText("Configure")
        self.buttons.rejected.connect(self.reject)
        lay = QVBoxLayout(self)
        lay.addLayout(form)
        lay.addWidget(self.log)
        lay.addWidget(self.buttons)

    def say(self, text: str) -> None:
        self.log.appendPlainText(text)


class DevicesTab(QWidget):
    devices_changed = Signal()

    def __init__(self, settings: Settings, parent=None):
        super().__init__(parent)
        self.s = settings
        self.rows: dict[str, Row] = {}
        self.mdns = discovery.MdnsBrowser()
        self._busy_lan = self._busy_ap = False

        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.itemSelectionChanged.connect(self._update_buttons)
        self.table.doubleClicked.connect(lambda *_: self.open_web())

        self.btn_refresh = QPushButton("Refresh")
        self.btn_refresh.clicked.connect(self.refresh)
        self.btn_add = QPushButton("Add by IP…")
        self.btn_add.clicked.connect(self.add_by_ip)
        self.chk_ap = QCheckBox("Look for unconfigured boards")
        self.chk_ap.setChecked(self.s.scan_for_unconfigured)
        self.chk_ap.toggled.connect(self._toggle_ap_scan)
        self.btn_configure = QPushButton("Configure…")
        self.btn_configure.clicked.connect(self.configure_ap)
        self.btn_usb = QPushButton("Configure via USB…")
        self.btn_usb.clicked.connect(self.configure_usb)
        self.btn_flash = QPushButton("Program blank board (USB)…")
        self.btn_flash.setToolTip("First flash over USB: erases everything, writes bootloader, recovery and app")
        self.btn_flash.clicked.connect(self.program_usb)
        self.btn_web = QPushButton("Open web page")
        self.btn_web.clicked.connect(self.open_web)
        self.btn_name = QPushButton("Set name…")
        self.btn_name.clicked.connect(self.set_name)
        self.btn_wifi = QPushButton("Change Wi-Fi…")
        self.btn_wifi.clicked.connect(self.change_wifi)
        self.btn_reboot = QToolButton()
        self.btn_reboot.setText("Reboot")
        self.btn_reboot.setPopupMode(QToolButton.InstantPopup)
        m = QMenu(self)
        for text, target in (("Reboot (app)", "app"), ("Reboot to recovery", "recovery"), ("Boot other slot", "other")):
            a = QAction(text, self)
            a.triggered.connect(lambda _=False, t=target: self.reboot(t))
            m.addAction(a)
        self.btn_reboot.setMenu(m)
        self.btn_reset = QPushButton("Factory reset…")
        self.btn_reset.clicked.connect(self.factory_reset)
        self.btn_dump = QPushButton("Core dump…")
        self.btn_dump.clicked.connect(self.core_dump)

        top = QHBoxLayout()
        for w in (self.btn_refresh, self.btn_add, self.chk_ap):
            top.addWidget(w)
        top.addStretch(1)
        acts = QHBoxLayout()
        for w in (self.btn_configure, self.btn_usb, self.btn_flash, self.btn_web, self.btn_name, self.btn_wifi, self.btn_reboot,
                  self.btn_reset, self.btn_dump):
            acts.addWidget(w)
        acts.addStretch(1)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(500)
        self.log.setFixedHeight(110)
        lay = QVBoxLayout(self)
        lay.addLayout(top)
        lay.addWidget(self.table, 1)
        lay.addLayout(acts)
        lay.addWidget(self.log)

        self.t_lan = QTimer(self, interval=max(2, self.s.discovery_s) * 1000, timeout=self.refresh)
        self.t_ap = QTimer(self, interval=max(5, self.s.ap_scan_s) * 1000, timeout=self.scan_aps)
        self._update_buttons()

    # ---- lifecycle ----
    def start(self) -> None:
        self.refresh()
        self.t_lan.start()
        if self.chk_ap.isChecked():
            self.scan_aps()
            self.t_ap.start()

    def stop(self) -> None:
        self.t_lan.stop()
        self.t_ap.stop()
        self.mdns.close()

    def note(self, text: str) -> None:
        self.log.appendPlainText(time.strftime("%H:%M:%S ") + text)

    # ---- discovery ----
    def refresh(self) -> None:
        if self._busy_lan:
            return
        self._busy_lan = True
        workers.run(discover_all, self.s, self.mdns, on_done=self._on_lan, on_error=self._on_lan_err)

    def _on_lan_err(self, err: str) -> None:
        self._busy_lan = False
        self.note(f"discovery: {err.splitlines()[0]}")

    def _on_lan(self, found) -> None:
        self._busy_lan = False
        self.apply_seen(found)

    def apply_seen(self, found: list) -> None:
        """Merge discovery results (also used by tests)."""
        for seen, st in found:
            f = seen.fields
            r = self.rows.get(seen.mac) or Row(seen.mac, seen.mac)
            r.ip, r.id, r.label = seen.ip, f.get("id") or r.id, f.get("label", r.label) or ""
            r.mode, r.boot = f.get("mode") or r.mode, f.get("boot") or r.boot
            r.app, r.rec = f.get("fw") or r.app, f.get("rec") or r.rec
            r.last_seen = seen.last_seen or time.time()
            if st:
                w = st.get("wifi", {})
                r.rssi = str(w.get("rssi")) if w.get("sta_connected") else "AP only"
            self.rows[seen.mac] = r
            # A configured board is no longer "needs config".
            for k in [k for k, v in self.rows.items() if v.mode == "needs-config" and v.id and v.id == r.id]:
                del self.rows[k]
        self._render()

    def _toggle_ap_scan(self, on: bool) -> None:
        self.s.scan_for_unconfigured = on
        self.s.save()
        if on:
            self.scan_aps()
            self.t_ap.start()
        else:
            self.t_ap.stop()

    def scan_aps(self) -> None:
        if self._busy_ap:
            return
        self._busy_ap = True
        workers.run(wifi_win.device_aps, on_done=self._on_aps, on_error=lambda e: setattr(self, "_busy_ap", False))

    def _on_aps(self, aps) -> None:
        self._busy_ap = False
        self.apply_aps(aps)

    def apply_aps(self, aps: list[tuple[str, str, str]]) -> None:
        lan_ids = {r.id for r in self.rows.values() if r.on_lan}
        seen_keys = set()
        for ssid, kind, dev_id in aps:
            if dev_id in lan_ids:
                continue  # already on the LAN: its AP is just the open config window
            key = f"AP:{ssid}"
            seen_keys.add(key)
            r = self.rows.get(key) or Row(key)
            r.id, r.ap_ssid, r.mode = dev_id, ssid, "needs-config"
            r.boot = "recovery AP" if kind == "REC" else "config AP"
            r.last_seen = time.time()
            self.rows[key] = r
        for k in [k for k, v in self.rows.items() if v.mode == "needs-config" and k not in seen_keys]:
            del self.rows[k]
        self._render()

    def _render(self) -> None:
        sel = self.selected()
        keys = sorted(self.rows, key=lambda k: (self.rows[k].mode == "needs-config", self.rows[k].label or k))
        self.table.setRowCount(len(keys))
        for i, k in enumerate(keys):
            r = self.rows[k]
            age = int(time.time() - r.last_seen) if r.last_seen else -1
            vals = [r.mac or "-", r.id, r.label, r.ip or "-", r.hostname, r.mode, r.boot, r.app, r.rec, r.rssi,
                    f"{age} s ago" if age >= 0 else ""]
            for c, v in enumerate(vals):
                it = QTableWidgetItem(v)
                it.setData(Qt.UserRole, k)
                if r.boot in HIGHLIGHT:
                    it.setBackground(QColor(255, 214, 153))
                    it.setToolTip(f"boot reason {r.boot}: check this board")
                elif r.mode == "needs-config":
                    it.setBackground(QColor(220, 232, 255))
                self.table.setItem(i, c, it)
            if sel and k == sel.key:
                self.table.selectRow(i)
        self._update_buttons()
        self.devices_changed.emit()

    def selected(self) -> Row | None:
        items = self.table.selectedItems()
        return self.rows.get(items[0].data(Qt.UserRole)) if items else None

    def lan_devices(self) -> list[Row]:
        return [r for r in self.rows.values() if r.on_lan]

    def _update_buttons(self) -> None:
        r = self.selected()
        lan = bool(r and r.on_lan)
        self.btn_configure.setEnabled(bool(r and r.mode == "needs-config"))
        for b in (self.btn_web, self.btn_name, self.btn_wifi, self.btn_reboot, self.btn_reset, self.btn_dump):
            b.setEnabled(lan)

    def api(self, r: Row) -> DeviceApi:
        return DeviceApi(r.ip, r.mac, self.s.api_key)

    def _call(self, what: str, fn, *args, on_done=None) -> None:
        def done(res):
            self.note(f"{what}: ok" + (f" ({res})" if res not in (None, {}) and on_done is None else ""))
            if on_done:
                on_done(res)
            self.refresh()

        def err(e):
            first = e.splitlines()[0]
            self.note(f"{what}: {first}")
            QMessageBox.warning(self, what, first)

        workers.run(fn, *args, on_done=done, on_error=err)

    # ---- actions ----
    def add_by_ip(self) -> None:
        ip, ok = QInputDialog.getText(self, "Add by IP", "Board IP address:")
        if not ok or not ip.strip():
            return
        ip = ip.strip()

        def done(seen):
            if seen is None:
                QMessageBox.warning(self, "Add by IP", f"No devlink board answers at {ip}.")
                return
            if ip not in self.s.manual_ips:
                self.s.manual_ips.append(ip)
                self.s.save()
            self.apply_seen([(seen, None)])
            self.note(f"added {seen.mac} at {ip}")

        workers.run(discovery.probe_ip, ip, on_done=done)

    def configure_ap(self) -> None:
        r = self.selected()
        if not r or r.mode != "needs-config":
            return
        dlg = ConfigureDialog(self, f"Configure {r.ap_ssid}", self.s.bench_ssid, r.label)
        dlg.say("This PC joins the board's AP for a few seconds (no internet meanwhile), sends the settings and "
                "returns to the current network.")

        def go():
            if not dlg.ssid.text():
                return
            self.s.bench_ssid = dlg.ssid.text()
            self.s.save()
            dlg.buttons.button(QDialogButtonBox.Ok).setEnabled(False)
            workers.run(configure.configure_via_ap, r.ap_ssid, dlg.ssid.text(), dlg.password.text(),
                        dlg.label.text(), self.s.api_key, self.s.ap_password, self.mdns,
                        on_progress=dlg.say, on_done=lambda res: self._configured(dlg, res),
                        on_error=lambda e: self._configured(dlg, None, e), progress_kw="say")

        dlg.buttons.accepted.connect(go)
        dlg.exec()

    def _configured(self, dlg: ConfigureDialog, res, err: str | None = None) -> None:
        dlg.buttons.button(QDialogButtonBox.Ok).setEnabled(True)
        if res is None:
            dlg.say(f"FAILED: {err.splitlines()[0] if err else '?'}")
            return
        dlg.say(("DONE: " if res.ok else "NOT DONE: ") + res.message)
        self.note(f"configure {res.mac or '?'}: {res.message}")
        self.refresh()

    def program_usb(self) -> None:
        ProgramDialog(self.s, self, on_done=self.refresh).exec()

    def configure_usb(self) -> None:
        self.note("looking for boards on USB (Improv)…")

        def found(boards):
            if not boards:
                QMessageBox.information(self, "Configure via USB", "No board answers Improv on a USB port.")
                return
            pick = QDialog(self)
            pick.setWindowTitle("Boards on USB")
            lst = QListWidget()
            for b in boards:
                lst.addItem(f"{b.port}  {b.name}  ({b.firmware} {b.version}, {b.serial_number})")
            lst.setCurrentRow(0)
            bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
            bb.accepted.connect(pick.accept)
            bb.rejected.connect(pick.reject)
            v = QVBoxLayout(pick)
            v.addWidget(QLabel("Pick the board to configure:"))
            v.addWidget(lst)
            v.addWidget(bb)
            if pick.exec() != QDialog.Accepted:
                return
            b = boards[lst.currentRow()]
            dlg = ConfigureDialog(self, f"Configure {b.port} via USB", self.s.bench_ssid, b.name)

            def go():
                self.s.bench_ssid = dlg.ssid.text()
                self.s.save()
                dlg.buttons.button(QDialogButtonBox.Ok).setEnabled(False)
                workers.run(configure.configure_via_usb, b.port, dlg.ssid.text(), dlg.password.text(),
                            dlg.label.text(), self.s.api_key, on_progress=dlg.say,
                            on_done=lambda res: self._configured(dlg, res),
                            on_error=lambda e: self._configured(dlg, None, e), progress_kw="say")

            dlg.buttons.accepted.connect(go)
            dlg.exec()

        workers.run(improv.find_boards, on_done=found)

    def open_web(self) -> None:
        r = self.selected()
        if r and r.on_lan:
            QDesktopServices.openUrl(QUrl(f"http://{r.ip}/"))

    def set_name(self) -> None:
        r = self.selected()
        if not r:
            return
        name, ok = QInputDialog.getText(self, "Friendly name", "Name (max 32 bytes):", text=r.label)
        if ok:
            self._call("set name", lambda: self.api(r).set_config(label=name))

    def change_wifi(self) -> None:
        r = self.selected()
        if not r:
            return
        dlg = QDialog(self)
        dlg.setWindowTitle(f"Change Wi-Fi of {r.label or r.mac}")
        ssid, pw = QLineEdit(self.s.bench_ssid), QLineEdit()
        pw.setEchoMode(QLineEdit.Password)
        form = QFormLayout(dlg)
        form.addRow(QLabel("Safe change: if the board cannot connect with the new settings within 30 s, it goes back "
                           "to the old ones and stays reachable."))
        form.addRow("SSID", ssid)
        form.addRow("Password", pw)
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.accepted.connect(dlg.accept)
        bb.rejected.connect(dlg.reject)
        form.addRow(bb)
        if dlg.exec() == QDialog.Accepted and ssid.text():
            self._call("change Wi-Fi", lambda: self.api(r).set_wifi(ssid.text(), pw.text()))

    def reboot(self, target: str) -> None:
        r = self.selected()
        if not r:
            return

        def do():
            api = self.api(r)
            t = target
            if t == "other":
                info = api.info()
                t = "slot1" if info["running"] == "ota_0" else "slot0"
            return api.reboot(t)

        if target == "other" and QMessageBox.question(self, "Boot other slot",
                                                      "Boot the app in the other slot (only if it holds a valid app)?") != QMessageBox.Yes:
            return
        self._call(f"reboot ({target})", do)

    def factory_reset(self) -> None:
        r = self.selected()
        if not r:
            return
        box = QMessageBox(QMessageBox.Warning, "Factory reset",
                          f"Erase the devlink config of {r.label or r.mac} (Wi-Fi, name)? It goes back to AP mode.",
                          QMessageBox.Yes | QMessageBox.Cancel, self)
        nvs = QCheckBox("Also erase the test app's NVS")
        box.setCheckBox(nvs)
        if box.exec() == QMessageBox.Yes:
            self._call("factory reset", lambda: self.api(r).factory_reset(nvs.isChecked()))

    def core_dump(self) -> None:
        r = self.selected()
        if not r:
            return

        def got(data):
            if data is None:
                QMessageBox.information(self, "Core dump", "The board has no core dump.")
                return
            default = str(data_dir() / f"coredump_{r.id}_{time.strftime('%Y%m%d_%H%M%S')}.elf")
            path, _ = QFileDialog.getSaveFileName(self, "Save core dump", default, "ELF core dump (*.elf)")
            if path:
                with open(path, "wb") as f:
                    f.write(data)
                self.note(f"core dump saved to {path} ({len(data)} bytes)")

        self._call("core dump", lambda: self.api(r).coredump(), on_done=got)


__all__ = ["DevicesTab", "Row", "discover_all", "ApiError"]
