# ESP32-S3 bench template (devlink)

Firmware and a Windows PC app for bringing up and testing ESP32-S3 boards over Wi-Fi. You flash a board once over USB;
after that, test apps are updated, configured and monitored over the network. A broken or hostile test app can't
brick the board: there is always a way back to a recovery image that accepts a new app.

The design is in [SPEC.md](SPEC.md). The acceptance results are in [test/ACCEPTANCE.md](test/ACCEPTANCE.md).

## What you get

- **Custom bootloader** (`bootloader_components/devlink_boot`). It keeps a boot log in flash and picks recovery when the
  app asks for it, after 3 quick resets or power cycles, or when the app crash-loops (SPEC §3.4).
- **Recovery image** (`recovery/`) in the factory partition. It has Wi-Fi with an always-on config AP, Improv over USB,
  discovery and the HTTP API, and it accepts a new app over Wi-Fi.
- **devlink** (`components/devlink`), the layer every test app links in:
  - Wi-Fi and provisioning: config AP, Improv serial, safe Wi-Fi change with automatic revert;
  - finding boards: mDNS and UDP discovery;
  - the HTTP API `/api/v1` with OTA, a self-test, and rollback when a new app is broken;
  - WebSocket streaming of channels and the log, plus binary block channels for high sample rates (up to 1 kHz
    tested);
  - the BOOT button.
- **Test app template** (`app/`) with `devlink_sysmon` (`sys.temp`, `sys.heap`, `sys.rssi`) and `mod_example`, a
  module to copy for each IC under test (it also has the block channel `example.wave`).
- **PC app** (`pc/`, PySide6), with three tabs:
  - **Devices**: find and configure boards, including over USB.
  - **Firmware**: update one board or many, and update the recovery image.
  - **Monitor**: live plots and the log.

Out of scope for v1: remote commands, CSV recording, TLS and secure boot (SPEC §1, §13).

## Requirements

- An ESP32-S3 board with at least 8 MB of flash and a native USB connector (USB-Serial/JTAG).
- ESP-IDF **v6.0.3**. Use this one release for the bootloader, the recovery image and every app.
- Windows with Python 3.11 or newer (3.12 is used here) for the PC app. Linux should work, but it is untested.
- A 2.4 GHz Wi-Fi network that the PC and the boards share, without client isolation.

## Setup

ESP-IDF on this bench is installed in `C:\projects\esp-idf` (see [tools/installed_tools.md](tools/installed_tools.md)).
Activate it in PowerShell:

```powershell
. C:\projects\esp-idf\export-v6.0.3.ps1
```

Set up the PC app once:

```powershell
py -3.12 -m venv pc\.venv
pc\.venv\Scripts\python -m pip install -r pc\requirements.txt
```

## First flash of a board (USB, once)

```powershell
py -3.12 tools/host_tests.py                          # host unit tests
idf.py -C recovery -B build/recovery build            # recovery + the devlink bootloader
idf.py -C app -B build/app build                      # your app (or test/fw/bootlab)
python tools/flash_all.py --app app --label "my board"
```

`flash_all.py` erases the whole flash and writes:

- the bootloader;
- the partition table;
- recovery, into factory;
- the app, into ota_0.

It then checks the board's report over USB. **The bootloader and the partition table can only be changed over USB**
(SPEC §3.6), so plan any change to them.

On a PC without ESP-IDF, use **Program blank board (USB)…** in the PC app's Devices tab instead. It writes the same
files from existing builds with esptool (installed with the PC app): pick the build folder, the recovery build and the
app build. It checks the board first (chip, MAC, flash size, and what it runs), asks before erasing, and can set the
label and Wi-Fi at the end. Copy the two build folders to that PC, or point it at this repo's `build/`.

## Getting a board onto the network

A board without Wi-Fi settings opens the access point `ESP32S3-CFG-<id>` (password `configure`). Here `<id>` is the
last 6 hex digits of the MAC address. Configure it from the PC app's **Devices** tab in one of three ways:

- **Configure via AP**: the PC joins the board's AP for a moment, sends the settings, and goes back to its own network.
- **Configure via USB**: uses Improv serial. This also works from ESP Web Tools in a browser.
- **Wi-Fi…** on a board that is already on the LAN: a safe change. The board reverts to the old settings after 30 s
  if it can't reach the new network.

The API key defaults to `devlink-bench` (`CONFIG_DEVLINK_API_KEY`). Every API call also sends the board's MAC address
in `X-Device-Id`, so a request can't reach the wrong board.

## Run the PC app

Double-click `devlink.bat` in the repo root. It starts a small local server and opens the web UI in your browser at
http://127.0.0.1:8470/. Close the server window to stop it. From a terminal:

```powershell
pc\.venv\Scripts\python pc\devlink_web.py
```

- `--port N` picks another port.
- `--host 0.0.0.0` lets other PCs on the network open the page. Only do this on a trusted network: anyone who can open
  the page can flash boards.
- The page has a dark/light switch (moon icon) and Settings (gear icon).

The older Qt desktop app has the same three tabs. Start it with `devlink-qt.bat` or
`pc\.venv\Scripts\python pc\devlink.py`.

- **Devices**:
  - lists boards found through mDNS and UDP broadcast, and the config APs in range;
  - shows the label, mode (app or recovery), versions, the boot reason, and the last rollback or rejected image;
  - actions: reboot, reboot to recovery, factory reset, and fetching a core dump;
  - **Program blank board (USB)…**: the first flash without ESP-IDF (see above).
- **Firmware**:
  - picks an `.bin` file and checks it the same way the board does;
  - updates the selected boards one by one and reports `updated`, `rolled back` or `rejected` for each.
  - The **Recovery** target updates the factory image. The board then boots it once as a self-test.
- **Monitor**:
  - subscribes to channels, sets their rates, plots the last 30 s and shows the log;
  - block channels: one curve per field, with the received rate and lost blocks;
  - reconnects by itself after a reboot.

## Writing a test app

1. Copy `app/` (or build it as it is). `app_main.c` shows the required order:
   - `devlink_boot_early()` first;
   - then NVS;
   - then `devlink_console_start()` and `devlink_start()`;
   - then your modules.
2. Copy `components/mod_example` to `components/mod_<ic>`. A module:
   - registers its channels once (`devlink_channel_register`);
   - measures only while `devlink_channel_wanted_hz()` is above zero;
   - publishes with `devlink_publish_num` or `devlink_publish_arr`;
   - for high rates (hundreds of samples per second and more): registers a block channel
     (`devlink_block_register`) and publishes blocks of about 50 ms with `devlink_publish_block` (SPEC §5.7, §5.8;
     `example.wave` in `mod_example` shows how).
3. Module rules (SPEC §5.8). They are checked in review, not in code:
   - leave GPIO19 and GPIO20 (USB) and the button and LED pins alone;
   - don't use deep sleep without a timer wake-up (use `devlink_sleep()`);
   - never write `factory`, `dlcfg`, `dlboot` or `otadata`.
4. Give each build a new version: `CONFIG_APP_PROJECT_VER` in `sdkconfig.defaults` or the project version.
   - Build with `idf.py -C <project> -B build/<name> build`.
   - Upload `build/<name>/<project>.bin` from the Firmware tab.

A new app has 60 s to bring up its API and network. If it doesn't, or if it crashes or hangs before then, the board
rolls back to the previous app. While the new app is being verified, the RTC watchdog stays on.

## Getting a board back

| Situation | What to do |
|---|---|
| New app broken (crash, hang, no API) | Nothing. The board rolls back by itself, and the PC app shows "rolled back". |
| Valid app crash-loops | Nothing. After 5 crash loops the board boots recovery. |
| App sleeps forever, turns Wi-Fi off or disables USB | Power-cycle the board 4 times quickly (each within 5 s of the last), or press reset 4 times. It boots recovery. (The first one only ends the running app; an app that dies by itself within 5 s needs 3.) |
| App runs but you want recovery | Hold BOOT for 10 s, or use **Reboot to recovery** in the PC app. |
| Wi-Fi settings lost or wrong | Hold BOOT for 3 s to open the config AP. The AP also opens by itself when the STA can't connect. |
| In recovery | Upload a good app from the Firmware tab. Recovery's AP `ESP32S3-REC-<id>` is always on. |

Two things that surprise people on the bench:

- Five ordinary power cycles in a row, each less than 2 minutes after the previous boot (replugging while you wire a
  board), also count as a crash loop: the board boots recovery with reason `crash_loop`. Nothing is wrong. Reboot to
  the app from the PC app, or let recovery run 2 minutes and then power-cycle.
- A power cycle leaves recovery only after recovery has run 2 minutes. Earlier, the board comes back to recovery.

The last resort is a USB re-flash with `flash_all.py`.

## Repository layout

| Path | Contents |
|---|---|
| `bootloader_components/` | devlink bootloader override: boot log, decision rules, retained RTC block |
| `components/devlink` | devlink core (Wi-Fi, provisioning, API, OTA, streaming, console, button) |
| `components/devlink_sysmon`, `components/mod_example` | Built-in `sys.*` channels; the module template |
| `recovery/` | Recovery image project (factory partition) |
| `app/` | Test-app template project |
| `pc/` | PC app (`devlink_pc`: a Qt-free library plus the GUI) and its tests |
| `tools/` | `flash_all.py`, partition tables, shared sdkconfig defaults, host test runner, `devlink_tools.py` |
| `test/host` | Host unit tests (gcc), for code shared with the bootloader and devlink |
| `test/fw` | Test firmware: `bootlab` (fault-injection app), the test console, recovery variants |
| `test/hil` | Hardware-in-the-loop test scripts for milestones 1–8, plus the acceptance run |

## Tests

The command table is in [CLAUDE.md](CLAUDE.md). In short:

- `py -3.12 tools/host_tests.py`: host unit tests for the boot log, dlcfg, JSON, OTA checks, Improv, streaming,
  button and Wi-Fi policy.
- `pc\.venv\Scripts\python -m pytest pc/tests`: PC library and GUI tests.
- `test/hil/hil_m1.py` … `hil_m6_net.py`, `hil_m8_net.py`: the milestone suites, run on a real board.
- `test/hil/manual_m8_usb.py`: Program blank board (USB) on the bench board, including a cancel in the middle (erases
  the board).
- `test/hil/acceptance.py`: the SPEC §9 tests that the milestone suites don't already cover.
- `test/hil/manual_m7.py`: the hands-on tests (power cuts during an OTA, reset presses, BOOT button). Dialogs say
  what to do.

The acceptance record is in [test/ACCEPTANCE.md](test/ACCEPTANCE.md). It maps each of tests 1–26 to the run that
proved it.
