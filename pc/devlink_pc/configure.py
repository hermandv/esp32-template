"""Configure flow for boards that are not on the LAN (SPEC §8.1).

Via the config AP: remember the current network -> join the board's AP with a temporary, non-auto-connect profile
-> POST label + Wi-Fi within a few seconds (Windows may leave a network without internet) -> restore the previous
network -> wait for the board on the LAN -> confirm. Via USB: Improv serial, same result.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass

import requests

from . import wifi_win
from .api import ApiError, DeviceApi
from .discovery import MdnsBrowser, find_by_mac, norm_mac
from .improv import ImprovPort

AP_IP = "192.168.4.1"
AP_PASSWORD = "configure"

Say = Callable[[str], None]


@dataclass
class ConfigureResult:
    ok: bool
    mac: str = ""
    ip: str = ""
    message: str = ""


def configure_via_ap(ap_ssid: str, ssid: str, password: str, label: str = "", key: str = "devlink-bench",
                     ap_password: str = AP_PASSWORD, mdns: MdnsBrowser | None = None, say: Say | None = None,
                     lan_wait_s: float = 60.0) -> ConfigureResult:
    say = say or (lambda _: None)
    before = wifi_win.current()
    say(f"current network: {before.ssid or 'none'}")
    mac = ""
    sent = False
    try:
        for attempt in range(3):  # retry if Windows drops the AP (no internet) before the POST
            say(f"joining {ap_ssid} (attempt {attempt + 1})")
            if not wifi_win.join_temp(ap_ssid, ap_password):
                continue
            try:
                info = DeviceApi(AP_IP, "", key, timeout=4).info()
                mac = norm_mac(info["mac"])
                api = DeviceApi(AP_IP, mac, key, timeout=4)
                if label:
                    api.set_config(label=label)
                r = api.set_wifi(ssid, password)  # from the AP: saved directly (SPEC §5.4)
                sent = True
                say(f"settings sent ({r.get('mode')})")
                break
            except (requests.RequestException, ApiError, KeyError, ValueError) as e:
                say(f"POST failed: {e}; retrying")
        if not sent:
            return ConfigureResult(False, mac, "", "could not reach the board through its AP")
    finally:
        wifi_win.remove_temp(ap_ssid)
        if before.profile:
            say(f"back to {before.profile}")
            wifi_win.connect(before.profile)

    say("waiting for the board on the LAN")
    seen = find_by_mac(mac, timeout=lan_wait_s, mdns=mdns)
    if seen is None:
        return ConfigureResult(False, mac, "", f"settings saved, but the board did not appear on this network within "
                                               f"{lan_wait_s:.0f} s (wrong password, other subnet, or client isolation)")
    try:
        st = DeviceApi(seen.ip, mac, key).status()
        ok = st["wifi"]["sta_connected"] and st["wifi"]["ssid"] == ssid
    except (requests.RequestException, ApiError, KeyError):
        ok = False
    return ConfigureResult(ok, mac, seen.ip, f"on the LAN at {seen.ip}" if ok else "seen, but status unclear")


def configure_via_usb(port: str, ssid: str, password: str, label: str = "", key: str = "devlink-bench",
                      say: Say | None = None, reach_s: float = 20.0) -> ConfigureResult:
    say = say or (lambda _: None)
    with ImprovPort(port) as p:
        info = p.info()
        if not info:
            return ConfigureResult(False, "", "", f"{port} does not answer Improv")
        if label:
            ok = p.line(f"devlink label {label}", "devlink: label")
            say(f"label: {ok or 'no answer'}")
        say(f"sending Wi-Fi settings for '{ssid}' over USB")
        try:
            url = p.provision(ssid, password)
        except RuntimeError as e:
            why = [ln for ln in p.log if "STA down" in ln or "wifi" in ln.lower()][-3:]
            return ConfigureResult(False, "", "", str(e) + (" | board log: " + " / ".join(why) if why else ""))
    ip = url.split("//")[-1].strip("/")
    say(f"board connected, URL {url}")
    end = time.time() + reach_s
    while time.time() < end:
        try:
            i = requests.get(f"http://{ip}/api/v1/info", timeout=3).json()
            return ConfigureResult(True, norm_mac(i["mac"]), ip, f"on the network at {ip}")
        except (requests.RequestException, ValueError, KeyError):
            time.sleep(1)
    return ConfigureResult(False, "", ip, f"board joined '{ssid}' as {ip}, but this PC cannot reach it "
                                          "(other subnet or client isolation)")
