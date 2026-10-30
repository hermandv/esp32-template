# ESP32-S3 Bench Template (devlink)

The design lives in `SPEC.md` (v0.12). Read it before changing anything; it is the source of truth.
`SPEC_v0.2.md` is an old version kept for reference only.

## Ground rules
- Target: ESP32-S3, ESP-IDF (one pinned release for bootloader, recovery and app). PC app: Python 3.11+, PySide6, Windows first.
- The custom bootloader (SPEC §3.4) and partition table (§3.7) are frozen once flashed over USB. Build and test them
  first (milestone 1), and treat every change to them as high risk.
- Keep the design invariants in SPEC §3.1 (I1–I6). Any change that weakens one must be called out explicitly.
- Out of scope for v1: remote commands, CSV recording, TLS/secure boot (SPEC §1, §13).
- When the design changes, update `SPEC.md` in the same change.

## Toolchain
- ESP-IDF **v6.0.3**, shared install in `C:\projects\esp-idf` (see `tools/installed_tools.md`).
  Activate in PowerShell with `. C:\projects\esp-idf\export-v6.0.3.ps1`, then `idf.py ...`. Don't use the stock `export.ps1`.
- Use `py -3.12`, not `python` (not on PATH outside the IDF environment).
- Log new installs in `tools/installed_tools.md`, and ask before installing anything new.

## Commands (PowerShell with ESP-IDF active, from the repo root)

| Step | Command |
|---|---|
| Host unit tests (run before flashing) | `py -3.12 tools/host_tests.py` |
| Build recovery (+ the devlink bootloader) | `idf.py -C recovery -B build/recovery build` |
| Build the app template | `idf.py -C app -B build/app build` |
| Build the bootlab test app | `idf.py -C test/fw/bootlab -B build/bootlab build` |
| Bootlab fault variants (tests 5/6/7) | `idf.py -C test/fw/bootlab -B build/bootlab_<v> -DDEVLINK_EXTRA_DEFAULTS=<repo>/test/fw/bootlab_<v>.defaults build`, `<v>` = panic, hang, noapi |
| Crash-test recovery (test 20) | `idf.py -C recovery -B build/recovery_crash -DDEVLINK_EXTRA_DEFAULTS=<repo>/test/fw/recovery_crash.defaults build` |
| First flash / full re-flash (erases everything) | `python tools/flash_all.py --mac 7c:df:a1:f2:50:30 --label "..."` |
| HIL tests, automatic | `python test/hil/hil_m1.py --mac 7c:df:a1:f2:50:30` |
| HIL tests needing power cycles | `python test/hil/hil_m1.py --port COM22 m_quick_power_cycles m_power_cycle_in_recovery m_dlcfg_power_cut` |
| HIL tests, recovery over USB (milestone 2) | `python test/hil/hil_m2.py --mac 7c:df:a1:f2:50:30` |
| HIL network tests via the board AP (PC offline meanwhile) | `powershell -File test/hil/run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X` |
| HIL app tests: OTA cycles, rollbacks (milestone 3, via the app AP) | `powershell -File test/hil/run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X -Target app -Script hil_m3_net.py` |
| HIL streaming tests (milestone 4, via the app AP) | `powershell -File test/hil/run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X -Target app -Script hil_m4_net.py` |
| PC app | `devlink.bat` (double-click) or `pc\.venv\Scripts\python pc\devlink.py` (setup: see `pc/requirements.txt`) |
| PC app unit + GUI tests | `pc\.venv\Scripts\python -m pytest pc/tests` |
| HIL PC-app tests (milestone 5; asks the 10X password) | `pc\.venv\Scripts\python test/hil/hil_m5_pc.py --mac 7c:df:a1:f2:50:30 --ssid 10X --home 10X` |
| Recovery variants for milestone 6 | `idf.py -C recovery -B build/recovery_v2 -DDEVLINK_EXTRA_DEFAULTS=<repo>/test/fw/recovery_v2.defaults build` (and `recovery_crash`) |
| HIL Monitor + recovery update (milestone 6, via the app AP) | `powershell -File test/hil/run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X -Target app -Script hil_m6_net.py` |
| Acceptance run (milestone 7; LAN, asks the 10X password; tests 2 and 3 take ~20 min and 1 h) | `pc\.venv\Scripts\python -u test/hil/acceptance.py --mac 7c:df:a1:f2:50:30 --ssid 10X --home 10X [test ...]` (test 13 needs the IDF hello_world example in `build/hello_world`; see the script) |
| Hands-on acceptance tests 8, 12, 14 (dialogs say what to do) | `pc\.venv\Scripts\python -u test/hil/manual_m7.py --mac 7c:df:a1:f2:50:30 [test ...]` |
| HIL block channels (milestone 8, test 27; board on the LAN) | `pc\.venv\Scripts\python -u test/hil/hil_m8_net.py --mac 7c:df:a1:f2:50:30 [test ...]` |
| Program blank board over USB, bench test (milestone 8, tests 28/29; ERASES the board, asks the 10X password) | `pc\.venv\Scripts\python -u test/hil/manual_m8_usb.py --mac 7c:df:a1:f2:50:30 --ssid 10X --label "test esp"` |
| Wi-Fi fixes bench check (factory reset, Improv, safe change revert; ERASES dlcfg, asks the 10X password) | `pc\.venv\Scripts\python -u test/hil/manual_review_wifi.py --mac 7c:df:a1:f2:50:30 --ssid 10X --label "test esp"` |

Board console over USB: `devlink info|status|restart|recovery|label <text>` and Improv Wi-Fi serial. Test builds add
the HIL test console (`info`, `restart`, `crash`, `mode ...`, ...).

`python` here is the IDF env's Python (on PATH once the IDF environment is active). Each build keeps its `sdkconfig`
in its build directory; never commit an `sdkconfig` in a project folder.

## Bench board
- ESP32-S3 rev v0.1, 8 MB quad flash, 2 MB PSRAM, native USB-Serial/JTAG, MAC 7c:df:a1:f2:50:30 (COM22 today; COM numbers can change).
- Bench Wi-Fi for the board: **10X** (2.4 GHz band), the same network as the PC (10X_Guest is not reachable from
  this PC). The password is typed into a masked dialog when a test needs it, never stored.
- No switchable USB power: when a test needs real power cycles, stop and ask the user to unplug/replug, saying how many
  and how fast, and wait for them to confirm. An RTS/esptool reset is not a power cycle.

## Layout (SPEC §5.1)
Acceptance results: `test/ACCEPTANCE.md`. User-facing overview: `README.md`.

bootloader_components/devlink_boot, components/devlink, components/devlink_sysmon, components/mod_<ic>,
recovery/, app/, pc/, tools/
