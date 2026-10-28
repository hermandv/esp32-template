"""Improv frames (SPEC §5.4) and netsh output parsing (SPEC §8.1)."""
from devlink_pc import improv, wifi_win
from devlink_pc.discovery import norm_mac


def test_improv_roundtrip():
    payload = bytes([3, 8, 3]) + b"abc" + bytes([3]) + b"xyz"
    f = improv.frame(improv.RPC_RESULT, payload)
    frames, rest = improv.parse(b"log text\nIMPR" + f + b"more IMPRO")
    assert frames == [(4, payload)] and rest == b"IMPRO"
    assert improv.strings(frames[0][1]) == ["abc", "xyz"]
    bad = bytearray(f)
    bad[-1] ^= 1
    assert improv.parse(bytes(bad))[0] == []


def test_norm_mac():
    assert norm_mac("7c:df:a1:f2:50:30") == "7C:DF:A1:F2:50:30"
    assert norm_mac("7CDFA1F25030") == "7C:DF:A1:F2:50:30"


IFACES = """
There is 1 interface on the system:

    Name                   : Wi-Fi
    Description            : Intel(R) Wi-Fi 6E AX211 160MHz
    State                  : connected
    SSID                   : 10X
    BSSID                  : f4:e2:c6:f7:55:ef
    Profile                : 10X
"""
NETS = """
Interface name : Wi-Fi
There are 3 networks currently visible.

SSID 1 : 10X
    Network type            : Infrastructure
SSID 2 : ESP32S3-CFG-F25030
    Network type            : Infrastructure
SSID 3 : ESP32S3-REC-0A1B2C
SSID 4 : ESP32S3-CFG-short
"""


def test_netsh_parsing(monkeypatch):
    monkeypatch.setattr(wifi_win, "_run", lambda *a: IFACES if a[0:2] == ("show", "interfaces") else NETS)
    st = wifi_win.current()
    assert st.connected and st.ssid == "10X" and st.profile == "10X"
    assert wifi_win.device_aps(fresh=False) == [("ESP32S3-CFG-F25030", "CFG", "F25030"),
                                                ("ESP32S3-REC-0A1B2C", "REC", "0A1B2C")]


def test_profile_xml_escapes():
    x = wifi_win._profile_xml("a<b", "p&w")
    assert "a&lt;b" in x and "p&amp;w" in x and "<connectionMode>manual</connectionMode>" in x
