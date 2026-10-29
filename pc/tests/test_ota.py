"""OTA procedure (SPEC §7.2) against a simulated board: updated, rolled back, rejected, timeout, batch stop."""
from pathlib import Path

import pytest

from devlink_pc import ota
from devlink_pc.api import ApiError
from devlink_pc.discovery import Seen

ROOT = Path(__file__).resolve().parents[2]
MAC = "7C:DF:A1:F2:50:30"


@pytest.fixture
def app() -> bytes:
    p = ROOT / "build" / "bootlab" / "bootlab.bin"
    if not p.exists():
        pytest.skip("build/bootlab not built")
    return p.read_bytes()


class FakeBoard:
    """/info before the upload, then `script` entries one per call (the last one repeats)."""

    def __init__(self, script, refuse=None):
        self.script, self.calls, self.uploaded, self.refuse = list(script), 0, None, refuse

    @staticmethod
    def info_before():
        return {"mac": MAC, "mode": "app", "running": "ota_0", "fw": {"version": "old"}, "boot": {"reason": "normal"},
                "chip": {"revision": 1, "flash_size": 8 << 20}, "slots": [{"name": "ota_0", "state": "valid"}]}


def install(monkeypatch, board: FakeBoard):
    monkeypatch.setattr(ota, "find_by_mac", lambda mac, **kw: Seen(MAC, "10.0.0.5", {}, "udp", 0))
    monkeypatch.setattr(ota.time, "sleep", lambda s: None)

    class Api:
        def __init__(self, ip, mac, key, timeout=8.0):
            pass

        def info(self):
            if board.uploaded is None:
                return board.info_before()
            board.calls += 1
            return board.script[min(board.calls, len(board.script)) - 1]

        def ota(self, data, force=False, progress=None):
            if board.refuse:
                raise ApiError(400, board.refuse)
            board.uploaded = data
            if progress:
                progress(len(data), len(data))
            return {"ok": True, "slot": "ota_1", "version": "new"}

    monkeypatch.setattr(ota, "DeviceApi", Api)


def after(running, version, reason, state, mode="app", rejected=None):
    return {"mac": MAC, "mode": mode, "running": running, "fw": {"version": version}, "boot": {"reason": reason},
            "slots": [{"name": "ota_0", "state": "valid"}, {"name": "ota_1", "state": state}],
            "rejected_version": rejected, "reset_reasons": [4, 1]}


def test_updated(monkeypatch, app):
    b = FakeBoard([after("ota_1", "bootlab-1", "normal", "pending_verify"),
                   after("ota_1", "bootlab-1", "normal", "valid")])
    install(monkeypatch, b)
    r = ota.update_device(MAC, app, "k")
    assert r.ok and r.slot == "ota_1" and r.old_version == "old" and b.uploaded == app


def test_rolled_back(monkeypatch, app):
    b = FakeBoard([after("ota_1", "bootlab-1", "normal", "pending_verify"),
                   after("ota_0", "old", "rollback", "aborted", rejected="bootlab-1")])
    install(monkeypatch, b)
    r = ota.update_device(MAC, app, "k")
    assert r.state == "rolled_back" and r.rejected_version == "bootlab-1" and r.reset_reasons == [4, 1]


def test_rejected_locally(monkeypatch, app):
    b = FakeBoard([after("ota_1", "x", "normal", "valid")])
    install(monkeypatch, b)
    r = ota.update_device(MAC, b"\x7fELF" + app[4:], "k")
    assert r.state == "rejected" and b.uploaded is None
    foreign = app.replace(ota.img.MARKER_MAGIC, b"\0" * 16)
    assert ota.update_device(MAC, foreign, "k").state == "rejected"  # needs force
    assert ota.update_device(MAC, foreign, "k", force=True).state != "rejected"


def test_refused_by_board(monkeypatch, app):
    install(monkeypatch, FakeBoard([], refuse="ota_in_progress"))
    r = ota.update_device(MAC, app, "k")
    assert r.state == "rejected" and "ota_in_progress" in r.message


def test_timeout(monkeypatch, app):
    install(monkeypatch, FakeBoard([after("ota_1", "bootlab-1", "normal", "pending_verify")]))
    r = ota.update_device(MAC, app, "k", reappear_s=0.05, valid_s=0.05)
    assert r.state == "timeout"


def test_batch_stops_on_first_failure(monkeypatch, app):
    install(monkeypatch, FakeBoard([], refuse="wrong_device"))
    rs = list(ota.run_batch([MAC, "AA:BB:CC:DD:EE:FF"], app, "k"))
    assert len(rs) == 1 and not rs[0].ok


def test_upload_is_not_chunked():
    """The board needs Content-Length; a chunked body would be read as image bytes."""
    import requests

    from devlink_pc.api import _Upload

    body = _Upload(b"a" * 1000, None)
    p = requests.Request("POST", "http://x/api/v1/ota", data=body, headers={"Content-Length": "1000"}).prepare()
    assert p.headers.get("Content-Length") == "1000" and "Transfer-Encoding" not in p.headers


# ---- recovery update (SPEC §7.3) ----

REC = ROOT / "build" / "recovery" / "devlink_recovery.bin"


def install_rec(monkeypatch, before: dict, script: list):
    state = {"uploaded": None, "calls": 0, "target": None}
    monkeypatch.setattr(ota, "find_by_mac", lambda mac, **kw: Seen(MAC, "10.0.0.5", {}, "udp", 0))
    monkeypatch.setattr(ota.time, "sleep", lambda s: None)

    class Api:
        def __init__(self, *a, **kw):
            pass

        def info(self):
            if state["uploaded"] is None:
                return before
            state["calls"] += 1
            return script[min(state["calls"], len(script)) - 1]

        def ota(self, data, target="app", force=False, progress=None):
            state["uploaded"], state["target"] = data, target
            return {"ok": True, "slot": "factory"}

    monkeypatch.setattr(ota, "DeviceApi", Api)
    return state


def rec_info(mode="app", reason="normal", slot_state="valid", st="none", ver=""):
    return {"mac": MAC, "mode": mode, "running": "ota_0", "fw": {"version": "app"}, "boot": {"reason": reason},
            "chip": {"revision": 1, "flash_size": 8 << 20}, "recovery": {"version": "old-rec"},
            "slots": [{"name": "ota_0", "state": slot_state}],
            "recovery_selftest": {"state": st, "version": ver}}


@pytest.fixture
def rec() -> bytes:
    if not REC.exists():
        pytest.skip("build/recovery not built")
    return REC.read_bytes()


def new_ver(rec_bytes):
    return ota.img.parse(rec_bytes).version


def test_recovery_ok(monkeypatch, rec):
    v = new_ver(rec)
    s = install_rec(monkeypatch, rec_info(), [rec_info(mode="recovery"), rec_info(st="ok", ver=v)])
    r = ota.update_recovery(MAC, rec, "k")
    assert r.ok and s["target"] == "recovery" and r.old_version == "old-rec"


def test_recovery_selftest_failed(monkeypatch, rec):
    v = new_ver(rec)
    install_rec(monkeypatch, rec_info(), [rec_info(st="pending", ver=v), rec_info(st="failed", ver=v)])
    r = ota.update_recovery(MAC, rec, "k")
    assert r.state == "selftest_failed" and "Re-flash" in r.message


@pytest.mark.parametrize("before,why", [(rec_info(mode="recovery"), "runs recovery"),
                                        (rec_info(slot_state="pending_verify"), "not marked valid"),
                                        (rec_info(reason="rollback"), "rolled back")])
def test_recovery_preconditions(monkeypatch, rec, before, why):
    s = install_rec(monkeypatch, before, [])
    r = ota.update_recovery(MAC, rec, "k")
    assert r.state == "rejected" and why in r.message and s["uploaded"] is None


def test_recovery_needs_a_recovery_image(monkeypatch, app):
    s = install_rec(monkeypatch, rec_info(), [])
    r = ota.update_recovery(MAC, app, "k")  # bootlab is not a recovery
    assert r.state == "rejected" and "not a recovery image" in r.message and s["uploaded"] is None
