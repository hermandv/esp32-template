"""OTA procedure, PC side (SPEC §7.2): re-check the file, rediscover the board by MAC, upload with X-Device-Id, wait for
the same MAC to come back, verify the result or report "rolled back". Batches run one device at a time and stop on
the first failure."""

from __future__ import annotations

import time
from collections.abc import Callable, Iterable, Iterator
from dataclasses import dataclass, field

import requests

from . import image as img
from .api import ApiError, DeviceApi
from .discovery import MdnsBrowser, find_by_mac, norm_mac

REAPPEAR_S = 180   # SPEC §7.2: a hang (90 s RTC WDT) + rollback + a slow Wi-Fi join
VALID_S = 75       # the new app's self-test (60 s) + margin


@dataclass
class OtaResult:
    mac: str
    state: str = "pending"          # updated / rolled_back / rejected / failed / timeout
    message: str = ""
    old_version: str = ""
    new_version: str = ""
    slot: str = ""
    boot_reason: str = ""
    rejected_version: str | None = None
    reset_reasons: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.state == "updated"


Progress = Callable[[str, str, int, int], None]  # (mac, phase, done, total)


def _slot_state(info: dict, name: str) -> str:
    return {s["name"]: s["state"] for s in info.get("slots", [])}.get(name, "?")


def update_device(mac: str, data: bytes, key: str, force: bool = False, hint_ip: str | None = None,
                  mdns: MdnsBrowser | None = None, progress: Progress | None = None,
                  reappear_s: float = REAPPEAR_S, valid_s: float = VALID_S) -> OtaResult:
    mac = norm_mac(mac)
    res = OtaResult(mac)
    say = progress or (lambda *_: None)

    say(mac, "finding", 0, 0)
    seen = find_by_mac(mac, timeout=6, mdns=mdns, hint_ip=hint_ip)
    if seen is None:
        res.state, res.message = "failed", "board not found on the network"
        return res
    api = DeviceApi(seen.ip, mac, key)
    try:
        before = api.info()
    except (requests.RequestException, ApiError) as e:
        res.state, res.message = "failed", f"/info: {e}"
        return res
    res.old_version = before["fw"]["version"]
    old_slot = before["running"]

    info = img.parse(data, chip_rev=before["chip"]["revision"], flash_size=before["chip"]["flash_size"])
    res.new_version = info.version
    if info.errors:
        res.state, res.message = "rejected", "; ".join(info.errors)
        return res
    if info.warnings and not force:
        res.state, res.message = "rejected", "; ".join(info.warnings) + " (use force)"
        return res

    say(mac, "uploading", 0, len(data))
    sent = [0]

    def progress(done: int, total: int) -> None:
        sent[0] = done
        say(mac, "uploading", done, total)

    try:
        r = api.ota(data, force=force or bool(info.warnings), progress=progress)
        res.slot = r.get("slot", "")
    except ApiError as e:
        res.state, res.message = "rejected", f"board refused the image: {e.error}"
        return res
    except requests.RequestException as e:
        if sent[0] < len(data):
            res.state, res.message = "failed", f"upload: {e}"
            return res
        # The whole image went out but the reply was lost (the board restarts 0.5 s after replying; a weak link
        # can drop the last packets): judge by what the board runs afterwards.
        say(mac, "reply lost, checking the board", 0, 0)
        res.slot = "ota_1" if old_slot == "ota_0" else "ota_0"

    # The board restarts; wait for the same MAC (its IP may change) and check what runs.
    say(mac, "restarting", 0, 0)
    end_seen = time.time() + reappear_s
    end_valid = end_seen + valid_s
    time.sleep(2)
    last = None
    while time.time() < end_valid:
        s = find_by_mac(mac, timeout=4, mdns=mdns, hint_ip=seen.ip)
        if s is None:
            if time.time() > end_seen:
                break
            continue
        try:
            last = DeviceApi(s.ip, mac, key).info()
        except (requests.RequestException, ApiError, ValueError):
            continue
        reason = last["boot"]["reason"]
        if last["running"] == res.slot and last["mode"] == "app":
            state = _slot_state(last, res.slot)
            say(mac, f"verifying ({state})", 0, 0)
            if state == "valid":
                res.state, res.boot_reason = "updated", reason
                res.message = f"{res.new_version} running in {res.slot}" + ("" if reason == "normal" else f", boot reason {reason}")
                return res
        elif last["running"] == old_slot and reason == "rollback":
            res.state, res.boot_reason = "rolled_back", reason
            res.rejected_version = last.get("rejected_version")
            res.reset_reasons = last.get("reset_reasons", [])
            res.message = f"rolled back to {last['fw']['version']}; rejected {res.rejected_version}"
            return res
        elif last["mode"] == "recovery":
            say(mac, f"in recovery ({reason})", 0, 0)
        time.sleep(2)
    res.state = "timeout"
    res.message = (f"board did not come back within {reappear_s:.0f} s" if last is None else
                   f"no verified result: running {last['running']} {last['fw']['version']}, reason {last['boot']['reason']}")
    return res


def run_batch(macs: Iterable[str], data: bytes, key: str, force: bool = False, hints: dict[str, str] | None = None,
              mdns: MdnsBrowser | None = None, progress: Progress | None = None,
              stop_on_failure: bool = True, on_result: Callable[[OtaResult], None] | None = None) -> Iterator[OtaResult]:
    """One device at a time (SPEC §7.2); stops at the first failure unless told otherwise."""
    for mac in macs:
        r = update_device(mac, data, key, force, (hints or {}).get(norm_mac(mac)), mdns, progress)
        if on_result:
            on_result(r)
        yield r
        if not r.ok and stop_on_failure:
            return


FACTORY_SIZE = 0x180000
RECOVERY_BACK_S = 150  # write + verify, self-test boot (Wi-Fi + HTTP up), back into the app


def update_recovery(mac: str, data: bytes, key: str, force: bool = False, hint_ip: str | None = None,
                    mdns: MdnsBrowser | None = None, progress: Progress | None = None,
                    back_s: float = RECOVERY_BACK_S) -> OtaResult:
    """Recovery update (SPEC §7.3, I5): only from a valid app that was not just rolled back, then the self-test boot.
    States: updated (self-test ok) / selftest_failed / rejected / failed / timeout."""
    mac = norm_mac(mac)
    res = OtaResult(mac)
    say = progress or (lambda *_: None)
    say(mac, "finding", 0, 0)
    seen = find_by_mac(mac, timeout=6, mdns=mdns, hint_ip=hint_ip)
    if seen is None:
        res.state, res.message = "failed", "board not found on the network"
        return res
    api = DeviceApi(seen.ip, mac, key)
    try:
        before = api.info()
    except (requests.RequestException, ApiError) as e:
        res.state, res.message = "failed", f"/info: {e}"
        return res
    res.old_version = (before.get("recovery") or {}).get("version", "")
    if before["mode"] != "app":
        res.state, res.message = "rejected", "the board runs recovery: a recovery update is only done from a running app"
        return res
    if _slot_state(before, before["running"]) not in ("valid", "undefined"):
        res.state, res.message = "rejected", "the running app is not marked valid yet"
        return res
    if before["boot"]["reason"] == "rollback":
        res.state, res.message = "rejected", "the app was just rolled back: fix the app first"
        return res

    info = img.parse(data, chip_rev=before["chip"]["revision"], flash_size=before["chip"]["flash_size"],
                     slot_size=FACTORY_SIZE)
    res.new_version = info.version
    warnings = list(info.warnings) + ([] if "recovery" in info.project else [f"'{info.project}' is not a recovery image"])
    if info.errors:
        res.state, res.message = "rejected", "; ".join(info.errors)
        return res
    if warnings and not force:
        res.state, res.message = "rejected", "; ".join(warnings) + " (use force)"
        return res

    say(mac, "uploading recovery", 0, len(data))
    try:
        api.ota(data, target="recovery", force=force or bool(warnings),
                progress=lambda d, t: say(mac, "uploading recovery", d, t))
    except ApiError as e:
        res.state, res.message = "rejected", f"board refused: {e.error}"
        return res
    except requests.RequestException as e:
        res.state, res.message = "failed", f"upload: {e}"
        return res

    say(mac, "self-test boot", 0, 0)
    end = time.time() + back_s
    time.sleep(3)
    last = None
    while time.time() < end:
        s = find_by_mac(mac, timeout=4, mdns=mdns, hint_ip=seen.ip)
        if s is None:
            continue
        try:
            last = DeviceApi(s.ip, mac, key).info()
        except (requests.RequestException, ApiError, ValueError):
            continue
        st = last.get("recovery_selftest", {})
        if last["mode"] == "recovery":
            say(mac, "self-test running in recovery", 0, 0)
        elif st.get("version") == info.version and st.get("state") in ("ok", "failed"):
            res.boot_reason = last["boot"]["reason"]
            if st["state"] == "ok":
                res.state, res.message = "updated", f"recovery {info.version} written, self-test passed"
            else:
                res.state = "selftest_failed"
                res.message = (f"recovery {info.version} FAILED its self-test: the board has no working recovery. "
                               "Re-flash the previous recovery.")
            return res
        time.sleep(2)
    res.state = "timeout"
    res.message = "no self-test result within the time limit" if last else "board did not come back"
    return res
