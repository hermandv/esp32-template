"""Monitor tab (SPEC §8.3): channel list from /channels, subscribe and set rates, live plots (pyqtgraph: follows the
last 30 s, or zoom/pan freely with the mouse, values under the cursor), readouts, log console with level/text filter
and save, status bar with auto-reconnect after a reboot. Block channels (binary, high rate) plot one curve per field
on the board's sample clock, with the received rate and lost blocks in the channel table."""

from __future__ import annotations

import time
from collections import deque

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QFileDialog, QHBoxLayout, QHeaderView,
                               QLabel, QLineEdit, QPlainTextEdit, QPushButton, QSpinBox, QSplitter, QTableWidget,
                               QTableWidgetItem, QVBoxLayout, QWidget)

from .. import blocks
from ..api import DeviceApi
from ..settings import data_dir
from ..stream import MonitorSession
from . import workers

WINDOW_S = 30.0
MAX_POINTS = 20000  # per series; kept for zooming back (pyqtgraph clips and downsamples what is drawn)
MAX_BLOCK_POINTS = 120000  # per block-channel field: 2 min at 1 kHz
LEVELS = ["V", "D", "I", "W", "E"]


class MonitorTab(QWidget):
    def __init__(self, settings, devices, parent=None):
        super().__init__(parent)
        self.s, self.devices = settings, devices
        self.session: MonitorSession | None = None
        self.channels: dict[str, dict] = {}
        self.points: dict[str, deque] = {}       # series name -> (t, v)
        self.series: dict[str, pg.PlotDataItem] = {}
        self.bpoints: dict[str, deque] = {}      # block series name -> chunks of (t array, v array)
        self.bcount: dict[str, int] = {}         # block series name -> points kept
        self.brx: dict[str, int] = {}            # block channel -> samples received since the last rate update
        self.brate: dict[str, float] = {}        # block channel -> received samples/s
        self.t_brate = time.time()
        self.board_offset: float | None = None   # PC time (s since t0) minus board time (s since boot)
        self.board_last = 0.0
        self.log_lines: deque = deque(maxlen=20000)
        self.t0 = time.time()

        # -- top: board + connect + status --
        self.board = QComboBox()
        self.board.setMinimumWidth(320)
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.toggle)
        self.state = QLabel("not connected")
        self.status = QLabel("")
        top = QHBoxLayout()
        top.addWidget(QLabel("Board"))
        top.addWidget(self.board, 1)
        top.addWidget(self.connect_btn)
        top.addWidget(self.state)
        top.addStretch(1)

        # -- channels --
        self.table = QTableWidget(0, 5)
        self.table.setHorizontalHeaderLabels(["Channel", "Unit", "Sub", "Rate (Hz)", "Value"])
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.setSelectionMode(QAbstractItemView.NoSelection)
        self.table.verticalHeader().setVisible(False)

        # -- plot --
        self.plot = pg.PlotWidget(background="w")
        self.plot.setMinimumHeight(220)
        self.plot.showGrid(x=True, y=True, alpha=0.3)
        self.plot.setLabel("bottom", "s")
        for side in ("left", "bottom"):
            self.plot.getAxis(side).setTextPen("#333")
        self.plot.addLegend(offset=(10, 10), labelTextColor="#222", brush=(255, 255, 255, 200))
        self.plot.getViewBox().sigRangeChangedManually.connect(self.on_manual_range)
        self.plot.scene().sigMouseMoved.connect(self.on_mouse)
        self.follow = QCheckBox(f"Follow last {WINDOW_S:.0f} s")
        self.follow.setChecked(True)
        self.follow.setToolTip("Untick by zooming or panning with the mouse; tick again to follow the newest data")
        self.follow.toggled.connect(self.on_follow)
        self.show_all = False  # "Show all" mode: x spans all data and grows with it, y fits
        self.all_btn = QPushButton("Show all")
        self.all_btn.setToolTip("Reset the view: all data on x (keeps growing with new data), y fitted to it")
        self.all_btn.clicked.connect(self.show_all_data)
        self.hover = QLabel("")
        pt = QHBoxLayout()
        pt.addWidget(self.follow)
        pt.addWidget(self.all_btn)
        pt.addWidget(self.hover, 1)
        view = QWidget()
        pv = QVBoxLayout(view)
        pv.setContentsMargins(0, 0, 0, 0)
        pv.addWidget(self.plot, 1)
        pv.addLayout(pt)

        # -- log --
        self.level = QComboBox()
        for lv, name in zip(LEVELS, ["verbose", "debug", "info", "warning", "error"]):
            self.level.addItem(f"{name} and above", lv)
        self.level.setCurrentIndex(2)
        self.level.currentIndexChanged.connect(self.rerender_log)
        self.filter = QLineEdit()
        self.filter.setPlaceholderText("filter text / tag")
        self.filter.textChanged.connect(self.rerender_log)
        self.pause = QCheckBox("Pause")
        save = QPushButton("Save log…")
        save.clicked.connect(self.save_log)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        self.log.setLineWrapMode(QPlainTextEdit.NoWrap)
        lt = QHBoxLayout()
        for w in (QLabel("Log"), self.level, self.filter, self.pause, save):
            lt.addWidget(w)
        logw = QWidget()
        lv = QVBoxLayout(logw)
        lv.setContentsMargins(0, 0, 0, 0)
        lv.addLayout(lt)
        lv.addWidget(self.log)

        right = QSplitter(Qt.Vertical)
        right.addWidget(view)
        right.addWidget(logw)
        split = QSplitter(Qt.Horizontal)
        split.addWidget(self.table)
        split.addWidget(right)
        split.setStretchFactor(1, 3)
        lay = QVBoxLayout(self)
        lay.addLayout(top)
        lay.addWidget(split, 1)
        lay.addWidget(self.status)

        self.t_ui = QTimer(self, interval=150, timeout=self.pump)
        self.t_status = QTimer(self, interval=2000, timeout=self.poll_status)
        devices.devices_changed.connect(self.refresh_boards)
        self.refresh_boards()

    # ---- boards / connection ----
    def refresh_boards(self) -> None:
        cur = self.board.currentData()
        self.board.blockSignals(True)
        self.board.clear()
        for r in sorted(self.devices.lan_devices(), key=lambda r: r.label or r.mac):
            self.board.addItem(f"{r.label or '(no name)'}  {r.mac}  {r.ip}  ({r.mode})", (r.mac, r.ip))
        i = next((k for k in range(self.board.count()) if cur and self.board.itemData(k)[0] == cur[0]), 0)
        self.board.setCurrentIndex(i)
        self.board.blockSignals(False)

    def toggle(self) -> None:
        if self.session is not None:
            self.disconnect_board()
            return
        data = self.board.currentData()
        if not data:
            return
        mac, ip = data
        self.connect_btn.setText("Disconnect")
        self.state.setText("loading channels…")
        workers.run(lambda: DeviceApi(ip, mac, self.s.api_key).channels(), on_done=lambda j: self._start(mac, ip, j),
                    on_error=lambda e: self._failed(e))

    def _failed(self, err: str) -> None:
        self.state.setText(f"failed: {err.splitlines()[0]}")
        self.connect_btn.setText("Connect")

    def _start(self, mac: str, ip: str, j: dict) -> None:
        self.channels = {c["name"]: c for c in j.get("channels", [])}
        self.fill_channels()
        self.t0 = time.time()
        self.board_offset = None
        self.session = MonitorSession(mac, ip, self.devices.mdns)
        self.session.subscribe("log")  # the log console is always on
        self.t_ui.start()
        self.t_status.start()
        self.poll_status()

    def disconnect_board(self) -> None:
        if self.session:
            self.session.close()
        self.session = None
        self.t_ui.stop()
        self.t_status.stop()
        self.connect_btn.setText("Connect")
        self.state.setText("not connected")

    def stop(self) -> None:
        self.disconnect_board()

    # ---- channel table ----
    def fill_channels(self) -> None:
        names = [n for n in self.channels if n != "log"]
        self.table.setRowCount(len(names))
        for row, name in enumerate(names):
            c = self.channels[name]
            self.table.setItem(row, 0, QTableWidgetItem(name))
            self.table.setItem(row, 1, QTableWidgetItem(c.get("unit", "")))
            chk = QCheckBox()
            chk.toggled.connect(lambda on, n=name: self.set_sub(n, on))
            self.table.setCellWidget(row, 2, chk)
            spin = QSpinBox()
            if c.get("type") == "block":
                spin.setRange(1, c.get("max_hz") or 1000)
                spin.setToolTip("Sample rate: the board runs at the highest rate any client asks for")
            else:
                spin.setRange(0, c.get("max_hz") or 1000)
                spin.setToolTip("0 = every value")
            spin.setValue(c.get("default_hz", 1) or spin.minimum())
            spin.valueChanged.connect(lambda hz, n=name: self.session and self.session.set_rate(n, hz))
            self.table.setCellWidget(row, 3, spin)
            self.table.setItem(row, 4, QTableWidgetItem(""))

    def _row(self, name: str) -> int:
        for r in range(self.table.rowCount()):
            if self.table.item(r, 0).text() == name:
                return r
        return -1

    def set_sub(self, name: str, on: bool) -> None:
        if self.session is None:
            return
        if on:
            spin = self.table.cellWidget(self._row(name), 3)
            self.session.subscribe(name, spin.value())
        else:
            self.session.unsubscribe(name)
            for key in [k for k in self.series if k == name or k.startswith(name + "[")]:
                self.plot.removeItem(self.series.pop(key))
                self.points.pop(key, None)
                self.bpoints.pop(key, None)
                self.bcount.pop(key, None)

    # ---- frames ----
    def pump(self) -> None:
        if self.session is None:
            return
        st = self.session.state
        self.state.setText({"connected": "connected", "reconnecting": f"reconnecting… ({self.session.reconnects})",
                            "refused": "refused: the board already has 2 clients"}.get(st, st))
        self.handle_blocks(self.session.drain_blocks(), self.session.seq.lost)
        self.handle_frames(self.session.drain())

    def handle_frames(self, frames: list[dict]) -> None:
        """Apply frames to the readouts, the plot buffers and the log (also used by tests)."""
        now = time.time() - self.t0
        new_log = []
        for f in frames:
            ch = f.get("ch")
            if ch == "log":
                line = f"{f.get('ts', 0) / 1e6:10.3f} {f.get('lvl', 'I')} {f.get('tag', '')}: {f.get('msg', '')}"
                self.log_lines.append((f.get("lvl", "I"), line))
                new_log.append((f.get("lvl", "I"), line))
                continue
            v = f.get("v")
            row = self._row(ch)
            if row >= 0:
                self.table.item(row, 4).setText(str(v) if not isinstance(v, float) else f"{v:.4g}")
            vals = [(ch, v)] if isinstance(v, (int, float)) else (
                [(f"{ch}[{i}]", x) for i, x in enumerate(v) if isinstance(x, (int, float))] if isinstance(v, list) else [])
            for key, x in vals:
                dq = self.points.setdefault(key, deque(maxlen=MAX_POINTS))
                dq.append((now, float(x)))
                self._curve(key)
        if new_log and not self.pause.isChecked():
            for lvl, line in new_log:
                if self._log_ok(lvl, line):
                    self.log.appendPlainText(line)
        self.redraw(now)

    def _curve(self, key: str) -> None:
        if key not in self.series:
            pen = pg.mkPen(pg.intColor(len(self.series), hues=9, values=1, maxValue=200), width=1.5)
            curve = self.plot.plot(name=key, pen=pen)
            curve.setClipToView(True)
            curve.setDownsampling(auto=True, method="peak")
            self.series[key] = curve

    def handle_blocks(self, blks: list[blocks.Block], lost: dict[int, int] | None = None) -> None:
        """Add decoded blocks to the plot buffers (also used by tests). Board time is mapped onto the PC time axis
        with one offset, set from the first block and again after the board rebooted (its clock went back)."""
        by_id = {c.get("id"): (name, c) for name, c in self.channels.items() if c.get("type") == "block"}
        for b in blks:
            if b.ch_id not in by_id or len(b.values) == 0:
                continue
            name, meta = by_id[b.ch_id]
            t = b.times_us / 1e6
            if self.board_offset is None or t[0] < self.board_last - 1.0:
                self.board_offset = (time.time() - self.t0) - float(t[-1])
            self.board_last = float(t[-1])
            t = t + self.board_offset
            fields = blocks.field_names(meta)
            for i, field in enumerate(fields[: b.values.shape[1]]):
                key = f"{name}[{field}]"
                self._curve(key)
                dq = self.bpoints.setdefault(key, deque())
                dq.append((t, b.values[:, i].astype(float)))
                self.bcount[key] = self.bcount.get(key, 0) + len(t)
                while self.bcount[key] - len(dq[0][0]) >= MAX_BLOCK_POINTS:
                    self.bcount[key] -= len(dq.popleft()[0])
            self.brx[name] = self.brx.get(name, 0) + len(b.values)
        dt = time.time() - self.t_brate
        if dt >= 1.0:
            for name in by_id.values():
                self.brate[name[0]] = self.brx.pop(name[0], 0) / dt
            self.t_brate = time.time()
            for cid, (name, _) in by_id.items():
                row = self._row(name)
                chk = self.table.cellWidget(row, 2) if row >= 0 else None
                if chk is not None and chk.isChecked():
                    self.table.item(row, 4).setText(
                        f"{self.brate.get(name, 0):.0f} samples/s, lost {(lost or {}).get(cid, 0)} blocks")

    def series_data(self, key: str) -> tuple[np.ndarray, np.ndarray]:
        """(t, v) of a series: JSON points or block chunks."""
        if key in self.bpoints:
            dq = self.bpoints[key]
            if not dq:
                return np.empty(0), np.empty(0)
            return np.concatenate([c[0] for c in dq]), np.concatenate([c[1] for c in dq])
        pts = self.points.get(key)
        if not pts:
            return np.empty(0), np.empty(0)
        a = np.asarray(pts, dtype=float)
        return a[:, 0], a[:, 1]

    def redraw(self, now: float) -> None:
        for key, curve in self.series.items():
            t, v = self.series_data(key)
            if len(t):
                curve.setData(t, v)
        if self.follow.isChecked():
            self.plot.setXRange(max(0.0, now - WINDOW_S), max(WINDOW_S, now), padding=0)
            # y follows the visible part only (auto-range over the whole buffer would include old data)
            self.plot.getViewBox().setAutoVisible(y=True)
            self.plot.enableAutoRange(axis="y")
        elif self.show_all:
            # x from the data itself: curves clip to the current view, so pyqtgraph's own auto-range would not see
            # points that arrived past the right edge and the view would stop growing
            ts = [p[0] for pts in self.points.values() if pts for p in (pts[0], pts[-1])]
            ts += [x for dq in self.bpoints.values() if dq for x in (dq[0][0][0], dq[-1][0][-1])]
            if ts:
                self.plot.setXRange(min(ts), max(ts), padding=0.02)
            self.plot.getViewBox().setAutoVisible(y=True)
            self.plot.enableAutoRange(axis="y")

    # ---- view modes: follow the last 30 s, show all, or free (after zoom/pan with the mouse) ----
    def on_follow(self, on: bool) -> None:
        if on:
            self.show_all = False
            self.plot.enableAutoRange(axis="y")

    def show_all_data(self) -> None:
        self.show_all = True
        self.follow.setChecked(False)
        self.redraw(time.time() - self.t0)

    def on_manual_range(self, *_) -> None:
        self.show_all = False
        self.follow.setChecked(False)

    def on_mouse(self, pos) -> None:
        """Values of every series at the time under the mouse."""
        vb = self.plot.getViewBox()
        if not self.series or not vb.sceneBoundingRect().contains(pos):
            self.hover.setText("")
            return
        t = vb.mapSceneToView(pos).x()
        parts = [f"t = {t:.2f} s"]
        for key in self.series:
            ts, vs = self.series_data(key)
            if not len(ts):
                continue
            i = int(np.clip(np.searchsorted(ts, t), 0, len(ts) - 1))
            if i > 0 and abs(ts[i - 1] - t) < abs(ts[i] - t):
                i -= 1
            parts.append(f"{key} = {vs[i]:.4g}")
        self.hover.setText("   ".join(parts))

    # ---- log ----
    def _log_ok(self, lvl: str, line: str) -> bool:
        min_lvl = self.level.currentData()
        text = self.filter.text().strip().lower()
        return (LEVELS.index(lvl) if lvl in LEVELS else 2) >= LEVELS.index(min_lvl) and (not text or text in line.lower())

    def rerender_log(self) -> None:
        self.log.setPlainText("\n".join(line for lvl, line in self.log_lines if self._log_ok(lvl, line)))
        self.log.verticalScrollBar().setValue(self.log.verticalScrollBar().maximum())

    def save_log(self) -> None:
        default = str(data_dir() / f"log_{time.strftime('%Y%m%d_%H%M%S')}.txt")
        path, _ = QFileDialog.getSaveFileName(self, "Save log", default, "Text (*.txt)")
        if path:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("\n".join(line for _, line in self.log_lines) + "\n")

    # ---- status bar ----
    def poll_status(self) -> None:
        if self.session is None:
            return
        ip, mac = self.session.ip, self.session.mac

        def show(st):
            w = st.get("wifi", {})
            self.status.setText(
                f"uptime {st['uptime_ms'] // 1000} s   heap {st['heap_free'] // 1024} KiB   "
                f"RSSI {w.get('rssi') if w.get('sta_connected') else 'AP'}   drops {st.get('drops', 0)}"
                f"{' (+' + str(self.session.lost_frames) + ' on PC)' if self.session and self.session.lost_frames else ''}"
                f"   temp {st.get('temp')}   error {st.get('last_error') or '-'}")

        workers.run(lambda: DeviceApi(ip, mac, self.s.api_key, timeout=2).status(), on_done=show,
                    on_error=lambda e: self.status.setText("status: no answer"))
