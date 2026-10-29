"""devlink web UI (SPEC §8): a local Flask server that does the PC-side work with the devlink_pc library (discovery,
configure via AP / USB, OTA batches, board actions) and serves a browser page. Live updates reach the page as
Server-Sent Events; the Monitor page talks to the board's WebSocket directly.

Bound to 127.0.0.1 by default: the server can flash boards, so it is only opened to the LAN on request (--host).
Every state-changing request needs the header "X-Devlink: 1" (a cross-site form or script cannot send it without a
CORS preflight, which this server never answers), and the Host header must name this server (DNS rebinding).
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import itertools
import json
import queue
import threading
import time
import traceback
import webbrowser
from collections import deque
from collections.abc import Callable
from pathlib import Path

import requests
from flask import Flask, Response, abort, jsonify, request, send_from_directory

from .. import __version__, configure, discovery, image, improv, ota, wifi_win
from ..api import ApiError, DeviceApi
from ..devices import HIGHLIGHT, Row, discover_all, merge_aps, merge_seen
from ..settings import Settings, data_dir

STATIC = Path(__file__).resolve().parent / "static"
DEFAULT_PORT = 8470
EDITABLE_SETTINGS = ("api_key", "ap_password", "bench_ssid", "discovery_s", "ap_scan_s", "scan_for_unconfigured")


class Job:
    """A long action (configure, OTA batch) run in a thread; its log and per-board progress go to the page."""

    _ids = itertools.count(1)

    def __init__(self, hub: Hub, kind: str, title: str):
        self.hub, self.kind, self.title = hub, kind, title
        self.id = next(self._ids)
        self.state = "running"          # running / done / failed
        self.log: list[str] = []
        self.boards: dict[str, dict] = {}
        self.result = None
        self._last_push = 0.0

    def snapshot(self) -> dict:
        return {"id": self.id, "kind": self.kind, "title": self.title, "state": self.state, "log": self.log[-200:],
                "boards": self.boards, "result": self.result}

    def push(self, force: bool = True) -> None:
        now = time.time()
        if force or now - self._last_push > 0.25:  # progress events at most 4 per second
            self._last_push = now
            self.hub.publish("job", self.snapshot())

    def say(self, text: str) -> None:
        self.log.append(time.strftime("%H:%M:%S ") + text)
        self.push()


class Hub:
    """Server state: the board list (refreshed by background threads), jobs, notes, loaded firmware files, and the
    event subscribers (one queue per open page)."""

    def __init__(self, settings: Settings, scan: bool = True):
        self.s = settings
        self.rows: dict[str, Row] = {}
        self.lock = threading.RLock()
        self.mdns = discovery.MdnsBrowser() if scan else None
        self.scan = scan
        self.subs: list[queue.Queue] = []
        self.jobs: dict[int, Job] = {}
        self.notes: deque = deque(maxlen=300)
        self.firmware: dict[str, dict] = {}   # token -> {"data", "info", "path", "target"}
        self.local_only = True
        self._wake = threading.Event()
        self._stop = threading.Event()

    # ---- lifecycle ----
    def start(self) -> None:
        if self.scan:
            threading.Thread(target=self._lan_loop, name="devlink-lan", daemon=True).start()
            threading.Thread(target=self._ap_loop, name="devlink-aps", daemon=True).start()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()
        if self.mdns:
            self.mdns.close()

    def _lan_loop(self) -> None:
        while not self._stop.is_set():
            try:
                self.apply_seen(discover_all(self.s, self.mdns))
            except Exception as e:  # noqa: BLE001
                self.note(f"discovery: {e}")
            self._wake.wait(max(2, self.s.discovery_s))
            self._wake.clear()

    def _ap_loop(self) -> None:
        while not self._stop.is_set():
            if self.s.scan_for_unconfigured:
                try:
                    self.apply_aps(wifi_win.device_aps())
                except Exception as e:  # noqa: BLE001
                    self.note(f"AP scan: {e}")
            elif any(r.mode == "needs-config" for r in self.rows.values()):
                self.apply_aps([])
            self._stop.wait(max(5, self.s.ap_scan_s))

    def refresh(self) -> None:
        self._wake.set()

    # ---- events ----
    def subscribe(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=500)
        with self.lock:
            self.subs.append(q)
        return q

    def unsubscribe(self, q: queue.Queue) -> None:
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)

    def publish(self, kind: str, data) -> None:
        msg = f"event: {kind}\ndata: {json.dumps(data)}\n\n"
        with self.lock:
            subs = list(self.subs)
        for q in subs:
            try:
                q.put_nowait(msg)
            except queue.Full:
                pass  # a stalled page; it reloads the state when it reconnects

    def note(self, text: str) -> None:
        line = time.strftime("%H:%M:%S ") + text
        self.notes.append(line)
        self.publish("note", line)

    # ---- board list ----
    def apply_seen(self, found: list) -> None:
        with self.lock:
            merge_seen(self.rows, found)
        self.publish("devices", self.devices_json())

    def apply_aps(self, aps: list) -> None:
        with self.lock:
            merge_aps(self.rows, aps)
        self.publish("devices", self.devices_json())

    def devices_json(self) -> list[dict]:
        with self.lock:
            rows = sorted(self.rows.values(), key=lambda r: (r.mode == "needs-config", r.label or r.key))
            now = time.time()
            return [{"key": r.key, "mac": r.mac, "id": r.id, "label": r.label, "ip": r.ip, "hostname": r.hostname,
                     "mode": r.mode, "boot": r.boot, "app": r.app, "rec": r.rec, "rssi": r.rssi, "ap_ssid": r.ap_ssid,
                     "on_lan": r.on_lan, "warn": r.boot in HIGHLIGHT,
                     "age_s": int(now - r.last_seen) if r.last_seen else None} for r in rows]

    def row(self, mac: str) -> Row:
        mac = discovery.norm_mac(mac)
        with self.lock:
            r = self.rows.get(mac)
        if r is None or not r.on_lan:
            abort(404, description=f"board {mac} is not on the network")
        return r

    def api(self, mac: str, timeout: float = 8.0) -> DeviceApi:
        r = self.row(mac)
        return DeviceApi(r.ip, r.mac, self.s.api_key, timeout=timeout)

    # ---- jobs ----
    def run_job(self, kind: str, title: str, fn: Callable[[Job], object]) -> Job:
        job = Job(self, kind, title)
        self.jobs[job.id] = job
        for old in sorted(self.jobs)[:-20]:  # keep the last 20
            del self.jobs[old]

        def run():
            try:
                job.result = fn(job)
                job.state = "done"
            except Exception as e:  # noqa: BLE001
                job.state = "failed"
                job.say(f"FAILED: {e}")
                traceback.print_exc()
            job.push()
            self.refresh()

        job.push()
        threading.Thread(target=run, name=f"devlink-job-{job.id}", daemon=True).start()
        return job


def _err(status: int, message: str):
    return jsonify({"error": message}), status


def _info_json(i: image.ImageInfo) -> dict:
    d = dataclasses.asdict(i)
    d["summary"] = i.summary
    return d


def create_app(hub: Hub) -> Flask:
    app = Flask(__name__, static_folder=None)
    app.config["MAX_CONTENT_LENGTH"] = 16 * 1024 * 1024

    @app.before_request
    def guard():
        if hub.local_only:
            host = (request.host or "").rsplit(":", 1)[0].strip("[]")
            if host not in ("127.0.0.1", "localhost", "::1"):
                abort(403, description="this server only answers on localhost")
        if request.method not in ("GET", "HEAD", "OPTIONS") and request.headers.get("X-Devlink") != "1":
            abort(403, description="missing X-Devlink header")

    @app.errorhandler(404)
    @app.errorhandler(403)
    @app.errorhandler(400)
    def http_error(e):
        if request.path.startswith("/api/"):
            return _err(e.code, e.description)
        return e

    @app.errorhandler(ApiError)
    def api_error(e: ApiError):
        return _err(502, f"board: {e.error} ({e.status})")

    @app.errorhandler(requests.RequestException)
    def net_error(e):
        return _err(504, f"board not reachable: {e.__class__.__name__}")

    # ---- page ----
    @app.get("/")
    def index():
        return send_from_directory(STATIC, "index.html", max_age=0)

    @app.get("/static/<path:name>")
    def static_files(name: str):
        return send_from_directory(STATIC, name, max_age=0)

    # ---- state + events ----
    @app.get("/api/ping")
    def ping():
        return jsonify({"app": "devlink", "version": __version__})

    @app.get("/api/state")
    def state():
        return jsonify({"version": __version__, "devices": hub.devices_json(), "notes": list(hub.notes),
                        "jobs": [j.snapshot() for j in hub.jobs.values()], "settings": settings_json()})

    @app.get("/api/events")
    def events():
        q = hub.subscribe()

        def stream():
            try:
                yield "retry: 2000\n\n"
                while True:
                    try:
                        yield q.get(timeout=15)
                    except queue.Empty:
                        yield ": keep-alive\n\n"
            finally:
                hub.unsubscribe(q)

        return Response(stream(), mimetype="text/event-stream", headers={"Cache-Control": "no-cache"})

    # ---- settings ----
    def settings_json() -> dict:
        return {k: getattr(hub.s, k) for k in EDITABLE_SETTINGS} | {"data_dir": str(data_dir()),
                                                                     "manual_ips": hub.s.manual_ips}

    @app.get("/api/settings")
    def get_settings():
        return jsonify(settings_json())

    @app.post("/api/settings")
    def set_settings():
        body = request.get_json(force=True) or {}
        for k in EDITABLE_SETTINGS:
            if k in body:
                cur = getattr(hub.s, k)
                setattr(hub.s, k, type(cur)(body[k]) if not isinstance(cur, bool) else bool(body[k]))
        hub.s.save()
        hub.refresh()
        return jsonify(settings_json())

    # ---- devices ----
    @app.get("/api/devices")
    def devices():
        return jsonify(hub.devices_json())

    @app.post("/api/refresh")
    def refresh():
        hub.refresh()
        return jsonify({"ok": True})

    @app.post("/api/devices/add_ip")
    def add_ip():
        ip = str((request.get_json(force=True) or {}).get("ip", "")).strip()
        seen = discovery.probe_ip(ip, timeout=3) if ip else None
        if seen is None:
            return _err(404, f"no devlink board answers at {ip or '?'}")
        if ip not in hub.s.manual_ips:
            hub.s.manual_ips.append(ip)
            hub.s.save()
        hub.apply_seen([(seen, None)])
        hub.note(f"added {seen.mac} at {ip}")
        return jsonify({"mac": seen.mac})

    @app.post("/api/configure/ap")
    def configure_ap():
        b = request.get_json(force=True) or {}
        ap, ssid, pw, label = b.get("ap_ssid", ""), b.get("ssid", ""), b.get("password", ""), b.get("label", "")
        if not ap or not ssid:
            return _err(400, "ap_ssid and ssid are needed")
        hub.s.bench_ssid = ssid
        hub.s.save()

        def work(job: Job):
            job.say("This PC joins the board's AP for a few seconds (no internet meanwhile), sends the settings and "
                    "returns to the current network.")
            res = configure.configure_via_ap(ap, ssid, pw, label, hub.s.api_key, hub.s.ap_password, hub.mdns,
                                             say=job.say)
            job.say(("DONE: " if res.ok else "NOT DONE: ") + res.message)
            hub.note(f"configure {res.mac or ap}: {res.message}")
            return dataclasses.asdict(res)

        return jsonify(hub.run_job("configure", f"Configure {ap}", work).snapshot())  # password not kept

    @app.get("/api/usb/boards")
    def usb_boards():
        return jsonify([dataclasses.asdict(b) for b in improv.find_boards()])

    @app.post("/api/configure/usb")
    def configure_usb():
        b = request.get_json(force=True) or {}
        port, ssid, pw, label = b.get("port", ""), b.get("ssid", ""), b.get("password", ""), b.get("label", "")
        if not port or not ssid:
            return _err(400, "port and ssid are needed")
        hub.s.bench_ssid = ssid
        hub.s.save()

        def work(job: Job):
            res = configure.configure_via_usb(port, ssid, pw, label, hub.s.api_key, say=job.say)
            job.say(("DONE: " if res.ok else "NOT DONE: ") + res.message)
            hub.note(f"configure {port}: {res.message}")
            return dataclasses.asdict(res)

        return jsonify(hub.run_job("configure", f"Configure {port} via USB", work).snapshot())

    # ---- one board ----
    def done(what: str, mac: str, res):
        hub.note(f"{what} {mac}: ok")
        hub.refresh()
        return jsonify(res if res is not None else {"ok": True})

    @app.get("/api/dev/<mac>/info")
    def dev_info(mac: str):
        return jsonify(hub.api(mac).info())

    @app.get("/api/dev/<mac>/status")
    def dev_status(mac: str):
        return jsonify(hub.api(mac, timeout=3).status())

    @app.get("/api/dev/<mac>/channels")
    def dev_channels(mac: str):
        return jsonify(hub.api(mac).channels())

    @app.post("/api/dev/<mac>/label")
    def dev_label(mac: str):
        label = str((request.get_json(force=True) or {}).get("label", ""))
        return done("set name", mac, hub.api(mac).set_config(label=label))

    @app.post("/api/dev/<mac>/wifi")
    def dev_wifi(mac: str):
        b = request.get_json(force=True) or {}
        if not b.get("ssid"):
            return _err(400, "ssid is needed")
        return done("change Wi-Fi", mac, hub.api(mac).set_wifi(b["ssid"], b.get("password", "")))

    @app.post("/api/dev/<mac>/reboot")
    def dev_reboot(mac: str):
        target = str((request.get_json(force=True) or {}).get("target", "app"))
        api = hub.api(mac)
        if target == "other":
            target = "slot1" if api.info()["running"] == "ota_0" else "slot0"
        if target not in ("app", "recovery", "slot0", "slot1"):
            return _err(400, f"unknown target {target}")
        return done(f"reboot ({target})", mac, api.reboot(target))

    @app.post("/api/dev/<mac>/factory_reset")
    def dev_factory_reset(mac: str):
        nvs = bool((request.get_json(force=True) or {}).get("nvs", False))
        return done("factory reset", mac, hub.api(mac).factory_reset(nvs))

    @app.get("/api/dev/<mac>/coredump")
    def dev_coredump(mac: str):
        r = hub.row(mac)
        data = hub.api(mac).coredump()
        if data is None:
            return _err(404, "the board has no core dump")
        name = f"coredump_{r.id}_{time.strftime('%Y%m%d_%H%M%S')}.elf"
        hub.note(f"core dump of {r.mac}: {len(data)} bytes")
        return Response(data, mimetype="application/octet-stream",
                        headers={"Content-Disposition": f'attachment; filename="{name}"'})

    # ---- firmware ----
    @app.get("/api/firmware/recent")
    def fw_recent():
        return jsonify({"app": hub.s.recent_app, "recovery": hub.s.recent_recovery})

    @app.post("/api/firmware/inspect")
    def fw_inspect():
        target = request.form.get("target") or (request.get_json(silent=True) or {}).get("target") or "app"
        if "file" in request.files:
            f = request.files["file"]
            data = f.read()
            up = data_dir() / "uploads"
            up.mkdir(parents=True, exist_ok=True)
            name = Path(f.filename or "image.bin").name
            path = up / f"{hashlib.sha256(data).hexdigest()[:8]}_{name}"
            path.write_bytes(data)
        else:
            raw = str((request.get_json(silent=True) or {}).get("path", "")).strip().strip('"')
            path = Path(raw)
            try:
                data = path.read_bytes()
            except OSError as e:
                return _err(400, f"cannot read {raw}: {e.strerror or e}")
        rec = target == "recovery"
        info = image.parse(data, str(path), slot_size=ota.FACTORY_SIZE if rec else 0x300000)
        if rec and info.ok and "recovery" not in info.project:
            info.warnings.append(f"'{info.project}' is not a recovery image")
        if info.ok and not info.errors:
            hub.s.remember_file(str(path), recovery=rec)
            hub.s.save()
        token = hashlib.sha256(data).hexdigest()[:16] + target
        hub.firmware[token] = {"data": data, "info": info, "path": str(path), "target": target}
        return jsonify({"token": token, "path": str(path), "target": target, "info": _info_json(info)})

    @app.post("/api/firmware/update")
    def fw_update():
        b = request.get_json(force=True) or {}
        fw = hub.firmware.get(b.get("token", ""))
        macs = [discovery.norm_mac(m) for m in b.get("macs", [])]
        force = bool(b.get("force", False))
        if fw is None:
            return _err(400, "load the firmware file again")
        if not macs:
            return _err(400, "pick at least one board")
        info: image.ImageInfo = fw["info"]
        if info.errors:
            return _err(400, "this file cannot be uploaded: " + "; ".join(info.errors))
        if info.warnings and not force:
            return _err(400, "; ".join(info.warnings) + " (tick 'Upload anyway' to override)")
        if any(j.state == "running" and j.kind == "ota" for j in hub.jobs.values()):
            return _err(409, "an update is already running")
        rec = fw["target"] == "recovery"
        step = ota.update_recovery if rec else ota.update_device
        with hub.lock:
            hints = {r.mac: r.ip for r in hub.rows.values() if r.on_lan}

        def work(job: Job):
            for m in macs:
                job.boards[m] = {"phase": "waiting", "pct": None, "state": "", "message": ""}
            job.say(f"{'Recovery' if rec else 'App'} {info.summary} → {len(macs)} board(s), one at a time")

            def progress(mac, phase, done_, total):
                job.boards[mac].update(phase=phase, pct=(100 * done_ // total) if total else None)
                job.push(force=False)

            results = []
            for mac in macs:  # one board at a time, stop at the first failure (SPEC §7.2)
                r = step(mac, fw["data"], hub.s.api_key, force, hints.get(mac), hub.mdns, progress)
                job.boards[mac].update(phase=r.state, state=r.state, message=r.message,
                                       pct=100 if r.ok else job.boards[mac]["pct"])
                job.say(f"{mac}: {r.state} {r.message}")
                hub.note(f"{'recovery' if rec else 'app'} update {mac}: {r.state} {r.message}")
                results.append(dataclasses.asdict(r))
                if not r.ok:
                    break
            for m in macs:
                if job.boards[m]["phase"] == "waiting":
                    job.boards[m].update(phase="skipped", message="not started: an earlier board failed")
            failed = [r for r in results if r["state"] == "selftest_failed"]
            prev = hub.s.recent_recovery[1] if rec and len(hub.s.recent_recovery) > 1 else None
            return {"results": results, "selftest_failed": bool(failed), "previous_recovery": prev}

        title = f"{'Recovery' if rec else 'App'} update: {info.project} {info.version}"
        return jsonify(hub.run_job("ota", title, work).snapshot())

    return app


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="devlink web UI: a local server; open the page in a browser.")
    ap.add_argument("--host", default="127.0.0.1",
                    help="address to listen on (default 127.0.0.1; 0.0.0.0 lets other PCs on the LAN use it, "
                         "including uploading firmware)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--no-browser", action="store_true", help="don't open the page in the browser")
    a = ap.parse_args(argv)
    url = f"http://127.0.0.1:{a.port}/"
    try:  # already running? just open the page
        if requests.get(url + "api/ping", timeout=1).json().get("app") == "devlink":
            print(f"devlink web UI already running: {url}")
            if not a.no_browser:
                webbrowser.open(url)
            return 0
    except (requests.RequestException, ValueError):
        pass
    hub = Hub(Settings.load())
    hub.local_only = a.host in ("127.0.0.1", "localhost", "::1")
    hub.start()
    print(f"devlink web UI {__version__}: {url}  (Ctrl+C to stop)")
    if not a.host.startswith("127.") and a.host != "localhost":
        print(f"WARNING: listening on {a.host}: anyone on this network can reach it and flash boards.")
    if not a.no_browser:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    try:
        create_app(hub).run(host=a.host, port=a.port, threaded=True, debug=False, use_reloader=False)
    except OSError as e:
        print(f"Cannot listen on {a.host}:{a.port} ({e.strerror or e}). Another program uses that port: "
              f"start with --port <other>.")
        return 1
    finally:
        hub.stop()
    return 0
