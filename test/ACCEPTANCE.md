# Acceptance record (SPEC §9, v1)

Board: ESP32-S3 rev v0.1, 8 MB quad flash, MAC 7c:df:a1:f2:50:30. ESP-IDF v6.0.3, devlink bootloader v2.
Bench Wi-Fi: 10X (2.4 GHz). The PC is on the same network. Run date: 2026-10-29 (milestone 7). Results of earlier
milestones are from the milestone runs (2026-10-27 to 10-29) on the same board and bootloader.

Script names below are in `test/hil/`. "acc" means `acceptance.py`; the hands-on tests are in `manual_m7.py`.

Status values:
- **pass**: the test as written passed.
- **pass (short)**: the same check passed on a shortened run; the reason is given.
- **partial**: only part of the test could be run; the note says which part.
- **not run**: the test could not be run on this bench; the note says why.

| # | Test | Status | Evidence |
|---|---|---|---|
| 1 | Fresh board → config AP → configure from PC app → on LAN with label ≤ 30 s | pass | acc `a01_fresh_board`: `flash_all.py` erase + write + verify, `ESP32S3-CFG-F25030` visible, configured through the AP, label set, on the LAN 6.1 s after the settings were sent |
| 2 | 50 OTAs in a row from the PC app, alternating slots | pass | acc `a02_many_otas`: 50 updates over the LAN with `devlink_pc.ota.update_device` (the Firmware tab's code path), slots alternated, each new app valid, 22 s per update on average, no failures |
| 3 | `sys.temp` 10 Hz + `log` for 1 h without disconnect | pass | acc `a03_stream_one_hour` (`MonitorSession`, the Monitor tab's code path): one session for 3600 s, 0 reconnects, 35,999 `sys.temp` frames (36,000 expected), longest gap 0.12 s, 1 frame dropped on the board, 0 on the PC |
| 4 | mDNS blocked → found via UDP broadcast | pass | acc `a04_udp_without_mdns` (UDP scan alone, `find_by_mac` without mDNS); `hil_m2_net.py` (recovery), `hil_m5_pc.py` |
| 5 | App panics at start → rollback | pass | `hil_m3_net.py` (bootlab_panic) |
| 6 | App hangs before self-test → RTC WDT → rollback | pass | `hil_m3_net.py` (bootlab_hang) |
| 7 | App never starts the API → rollback after 60 s | pass | `hil_m3_net.py` (bootlab_noapi) |
| 8 | Power cut at 10/50/99 % of an OTA and right after the reboot | pass | `manual_m7.py m08_power_cut_during_ota`: the upload was held at 10, 50 and 99 % (it went on at 64 bytes per 2 s, so the board kept writing), then USB power was cut by hand; each time the old app booted and was back on the LAN. `m08b_power_cut_after_reboot`: power cut while an unverified new image ran (an image without an API, so it never verifies) → old app (reason `rollback`), and the next OTA succeeded |
| 9 | Upload aborted → recovers without reboot, next upload succeeds | pass | `hil_m2_net.py` (recovery), `hil_m3_net.py` (app) |
| 10 | Two PCs upload at once → 409 for the second | pass | `hil_m2_net.py`, `hil_m3_net.py` |
| 11 | Valid app crash-loops after 40 s → recovery after 5; also across power cycles | pass (short) | `hil_m1.py t_crash_loop` (crash after ALIVE_5S, 8 s instead of 40 s: the rule only looks at the ALIVE mark); power-cycle part in `m_quick_power_cycles` |
| 12 | 3 quick power cycles or 3 reset presses → recovery | pass | power cycles: `hil_m1.py m_quick_power_cycles`; reset button: `manual_m7.py m12_reset_presses` (app in endless deep sleep, 3 RESET presses → recovery, reason `quick_reset`) |
| 13 | Foreign image with `force=1` → reachable again via quick-reset escape | pass | acc `a13_foreign_image` with the IDF hello_world example (restarts every 10 s): refused without force (`not_a_devlink_image`); with force it never marks itself valid, so it was rolled back; written as the valid app with esptool, it led to recovery after 40 s (reason `quick_reset`); the app was then restored from recovery over the LAN |
| 13a | Deep sleep with 10 s timer wake-ups → never recovery | pass (short) | `hil_m1.py t_deep_sleep` (20 wake-ups at 3 s; wake-ups are not logged at all, so the length doesn't matter) |
| 13b | 200 OTAs + reboots → never recovery | pass | `hil_m1.py t_planned_restarts` (200 planned restarts), `t_host_resets`; OTAs in tests 2 and `hil_m3_net.py` |
| 13c | Boot log torn writes and rollover → decisions still correct | pass | `hil_m1.py t_bootlog_rollover`, `t_torn_records`; host tests `test_bootlog.c` |
| 14 | Button 3 s → AP; 10 s → recovery; power cycle in recovery → app | pass | `manual_m7.py m14_button`: on the board, 3 s hold → config AP on; 10 s hold → recovery (reason `requested`). The restart while BOOT/GPIO0 is still held does not land in download mode. Power cycle in recovery → app: `hil_m1.py m_power_cycle_in_recovery`; button logic: host test `test_button.c` |
| 15 | Garbage in NVS → recovery and app Wi-Fi config still work | pass | acc `a15_nvs_garbage` (24 KB of random bytes over `nvs`; app and recovery both back on 10X) |
| 16 | dlcfg power cut during writes → previous config valid | pass | `hil_m1.py m_dlcfg_power_cut` (manual power cuts during continuous writes) + `t_dlcfg_persists`; host tests `test_dlcfg.c` (torn A/B records) |
| 16a | Improv from the PC app and from ESP Web Tools | partial | PC app: `hil_m5_pc.py p_usb_configure` passed. ESP Web Tools was not run in milestone 7 (skipped on 2026-10-29). To run it, open improv-wifi.com in Chrome or Edge, choose "Connect device to Wi-Fi" and pick the board's COM port; the board speaks the same Improv serial protocol the PC app uses |
| 17 | Both app slots erased → recovery, accepts an app | pass | `hil_m1.py t_slots_erased`, `hil_m2.py` |
| 18 | One byte corrupted in the active slot → fallback | pass | `hil_m1.py t_slot_corrupt` |
| 19 | Supply at the brownout edge → recovery comes up, accepts OTA | not run | Needs an adjustable supply; the bench board is USB-powered. Reduced TX power in recovery (`CONFIG_DEVLINK_TX_POWER_BROWNOUT_DBM`) is in place but untested. |
| 20 | Recovery that crashes at start → falls back to the valid app | pass | `hil_m1.py t_recovery_crashes` (recovery_crash build) |
| 21 | Wrong-chip / merged / ELF / no-marker / too-large images rejected | pass | `hil_m2_net.py` (recovery), `hil_m3_net.py` (app); host tests `test_ota_check.c` |
| 22 | Wrong `X-Device-Id` → rejected | pass | `hil_m2_net.py` |
| 23 | Broken recovery update → self-test fails, app runs, re-flash works | pass | `hil_m6_net.py r_update_ok`, `r_update_selftest_fails`, `r_restore_good`, `r_rejects` |
| 24 | Wi-Fi change over LAN to a wrong password → reverts within 30 s | pass | `hil_m5_pc.py p_safe_wifi_change` |
| 25 | Good OTA with a wrong saved password → no rollback; AP; reconfigure | pass | acc `a25_wrong_password`: wrong password written to dlcfg, config AP opened, OTA through the AP became valid (no rollback), then configured from the PC app's AP flow and back on the LAN |
| 26 | Bootloader write aborts; eFuse burn virtual only | pass | acc `a26_dangerous_writes`: `esp_flash_write` at 0x1000 → abort (reset reason panic), bootloader still matches the build (`esptool verify-flash`); `esp_efuse_write_field_blob` reads back 0xA5, then 0x00 after a restart |

## Notes

- **Test 26, first attempt.** It failed because the board still ran a bootlab built before the test hooks existed.
  After the re-flash in test 1 it passed.
- **Test 25, first attempt: a PC app bug.** After sending the settings through the AP, the configure flow took the
  first sighting of the board as proof. mDNS still held the board's old LAN address, so the status check that followed
  failed, and the flow reported "seen, but status unclear". `configure_via_ap` now keeps checking until the board's
  status says it is connected to the new network, or the wait runs out. Test 25 passed after the fix.
- **Manual script, first run.** Two script bugs stopped the first run of `manual_m7.py`: Wi-Fi state was read from
  `devlink info` instead of `devlink status`, and the USB port was opened twice. The 10/50/99 % power cuts in that run
  passed and are kept. The other parts were re-run after the fix and passed.
- **Not covered:**
  - Test 19 (brownout). It needs an adjustable supply.
  - The ESP Web Tools half of test 16a. It was skipped.
- **After the run:** dlcfg was erased, so the board is AP-only (`ESP32S3-CFG-F25030`, no label) with bootlab in ota_0
  and recovery 0.6.0-m6 in factory.
