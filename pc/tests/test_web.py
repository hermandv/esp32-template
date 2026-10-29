"""Web UI server (devlink_pc.web.server): routes, request guards, jobs. No network: discovery is off and the library
calls that would reach a board are replaced."""

import json
import time
from pathlib import Path

import pytest

from devlink_pc import configure, ota
from devlink_pc.discovery import Seen
from devlink_pc.settings import Settings
from devlink_pc.web import server

ROOT = Path(__file__).resolve().parents[2]
H = {"X-Devlink": "1"}


@pytest.fixture
def hub(monkeypatch, tmp_path):
    monkeypatch.setattr(Settings, "save", lambda self, path=None: None)
    monkeypatch.setattr(server, "data_dir", lambda: tmp_path)
    return server.Hub(Settings(), scan=False)


@pytest.fixture
def client(hub):
    return server.create_app(hub).test_client()


def seen(mac="7C:DF:A1:F2:50:30", ip="10.0.0.5", boot="normal"):
    return Seen(mac, ip, {"id": mac.replace(":", "")[-6:], "label": "bench", "mode": "app", "fw": "1.0",
                          "rec": "0.2", "boot": boot}, "udp", 0)


def wait_job(hub, job_id, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if hub.jobs[job_id].state != "running":
            return hub.jobs[job_id]
        time.sleep(0.02)
    raise AssertionError("job did not finish")


def test_page_and_assets(client):
    assert b"devlink" in client.get("/").data
    for p in ("/static/app.js", "/static/app.css", "/static/vendor/tabler/tabler.min.css",
              "/static/vendor/uplot/uPlot.iife.min.js", "/static/vendor/tabler-icons/fonts/tabler-icons.woff2"):
        assert client.get(p).status_code == 200, p


def test_guards(client):
    assert client.post("/api/refresh", json={}).status_code == 403              # no X-Devlink header
    assert client.post("/api/refresh", json={}, headers=H).status_code == 200
    r = client.get("/api/state", headers={"Host": "evil.example:8470"})          # DNS rebinding
    assert r.status_code == 403 and "localhost" in r.get_json()["error"]


def test_devices_and_state(hub, client):
    hub.apply_seen([(seen(), {"wifi": {"sta_connected": True, "rssi": -50}}),
                    (seen("AA:BB:CC:DD:EE:01", "10.0.0.6", boot="rollback"), None)])
    hub.apply_aps([("ESP32S3-CFG-0A0B0C", "CFG", "0A0B0C"), ("ESP32S3-CFG-F25030", "CFG", "F25030")])
    devs = client.get("/api/devices").get_json()
    assert len(devs) == 3  # the second AP belongs to a board already on the LAN
    by_mac = {d["mac"]: d for d in devs}
    assert by_mac["7C:DF:A1:F2:50:30"]["rssi"] == "-50" and by_mac["AA:BB:CC:DD:EE:01"]["warn"]
    assert [d["mode"] for d in devs].count("needs-config") == 1
    st = client.get("/api/state").get_json()
    assert len(st["devices"]) == 3 and "api_key" in st["settings"]
    assert client.get("/api/dev/11:22:33:44:55:66/info").status_code == 404     # unknown board


def test_events_reach_subscribers(hub):
    q = hub.subscribe()
    hub.note("hello")
    msg = q.get(timeout=1)
    assert msg.startswith("event: note") and "hello" in msg
    hub.unsubscribe(q)


def test_configure_job_keeps_no_password(hub, client, monkeypatch):
    calls = {}

    def fake(ap, ssid, pw, label, key, ap_pw, mdns, say):
        calls.update(ap=ap, ssid=ssid, pw=pw)
        say("joining")
        return configure.ConfigureResult(True, "7C:DF:A1:F2:50:30", "10.0.0.5", "on the LAN at 10.0.0.5")

    monkeypatch.setattr(configure, "configure_via_ap", fake)
    r = client.post("/api/configure/ap", headers=H,
                    json={"ap_ssid": "ESP32S3-CFG-F25030", "ssid": "10X", "password": "s3cret", "label": "b1"})
    job = wait_job(hub, r.get_json()["id"])
    assert job.state == "done" and job.result["ok"] and calls == {"ap": "ESP32S3-CFG-F25030", "ssid": "10X", "pw": "s3cret"}
    everything = json.dumps(client.get("/api/state").get_json())
    assert "s3cret" not in everything and hub.s.bench_ssid == "10X"


def test_firmware_inspect_and_update(hub, client, monkeypatch):
    p = ROOT / "build" / "bootlab" / "bootlab.bin"
    if not p.exists():
        pytest.skip("build/bootlab not built")
    r = client.post("/api/firmware/inspect", headers=H, json={"path": str(p), "target": "app"}).get_json()
    assert r["info"]["project"] == "bootlab" and not r["info"]["errors"]
    assert client.post("/api/firmware/update", headers=H, json={"token": r["token"], "macs": []}).status_code == 400
    hub.apply_seen([(seen(), None), (seen("AA:BB:CC:DD:EE:01", "10.0.0.6"), None)])
    done = []

    def fake_update(mac, data, key, force, hint, mdns, progress):
        progress(mac, "uploading", 50, 100)
        done.append(mac)
        res = ota.OtaResult(mac, state="updated" if mac.startswith("7C") else "rolled_back", message="x")
        return res

    monkeypatch.setattr(ota, "update_device", fake_update)
    j = client.post("/api/firmware/update", headers=H,
                    json={"token": r["token"], "macs": ["7c:df:a1:f2:50:30", "AA:BB:CC:DD:EE:01", "11:22:33:44:55:66"]})
    job = wait_job(hub, j.get_json()["id"])
    b = job.snapshot()["boards"]
    assert b["7C:DF:A1:F2:50:30"]["state"] == "updated" and b["AA:BB:CC:DD:EE:01"]["state"] == "rolled_back"
    assert b["11:22:33:44:55:66"]["phase"] == "skipped" and len(done) == 2  # stops at the first failure
    assert client.get("/api/firmware/recent").get_json()["app"][0] == str(p)


def test_firmware_upload_rejects_garbage(client):
    from io import BytesIO
    r = client.post("/api/firmware/inspect", headers=H,
                    data={"target": "app", "file": (BytesIO(b"\x00" * 300), "junk.bin")},
                    content_type="multipart/form-data").get_json()
    assert r["info"]["errors"]
    assert client.post("/api/firmware/update", headers=H, json={"token": r["token"], "macs": ["7C:DF:A1:F2:50:30"]}
                       ).status_code == 400
