# ESP32-S3 Bench Template – Specification (v0.10 draft)

A reusable firmware + PC tool template for bringing up and testing ESP32-S3 hardware boards over Wi-Fi.
Working name for the shared firmware layer: **devlink**.

Changes in v0.3 (brick review): custom bootloader with three app-independent ways into recovery, devlink config moved
out of NVS into its own partition, hardware watchdog during the verify window, recovery hardening rules, OTA hardening,
recovery self-test after a recovery update, safe Wi-Fi change, fixed API key, failure-mode table (§10).

Changes in v0.4: boot counters moved to a flash **boot log** that survives power loss (ESPHome-style, §3.4);
reset-button presses count like power cycles; USB Wi-Fi setup uses the **Improv serial** standard (§5.4).

Changes in v0.5 (milestone 1 implementation): **recovery-loop check now also suppresses the quick-reset escape**
(a recovery that crashed before 5 s re-entered itself forever); records with `CLEAN_RESTART` don't count as quick
resets (test 13b); boot reasons `recovery_loop` and `deep_sleep` added; recovery marks its own record `RECOVERY`;
retained-RTC block and `dlcfg` record layouts fixed (§3.4, §5.3); bootloader flash mode DIO; ESP-IDF v6.0.3 pinned;
a chip reset by the PC over USB (esptool / `idf.py flash`) counts as a planned restart (devlink bootloader v2).

Changes in v0.6 (milestone 2, recovery image): Improv serial implemented in devlink itself (no registry component);
console lines `devlink info|status|restart` added; no JSON library (small flat-object reader, §5.6); API details
fixed (wrong `X-Device-Id` → 409, `factory-reset` option `nvs`, `/channels` lists `log` until streaming arrives);
dlcfg field `sta_retry_s` renamed `button_window_s` (same position, defaults 20 s / 120 s); bootloader version also
in the IDF bootloader descriptor; recovery RTC WDT 30 s. OTA uploads run in their own task (§7.1).

Changes in v0.7 (milestone 3, devlink in the app): app self-test and rollback implementation fixed (§3.5), button
implemented (§5.9), `app/` template project added.

Changes in v0.8 (milestone 4, streaming): WebSocket protocol details, batching, rates and slow-client handling fixed
(§5.7); `devlink_sysmon` and `mod_example` added; `devlink_sleep()` and `devlink_channel_wanted_hz()` in the module API.

Changes in v0.9 (milestone 5, PC app Devices + Firmware): PC stack without httpx (requests in worker threads); a LAN
Wi-Fi change takes effect 1 s after the reply (§5.4); Improv frames bypass stdout newline conversion (§5.4); chunked
OTA uploads are refused with 411 (§7.1); PC app implementation notes (§8).

Changes in v0.10 (milestone 6): recovery update + self-test implemented with the self-test state in dlcfg instead of
retained RTC (§7.3); Monitor tab with Qt Charts instead of pyqtgraph (§8).

---

## 1. Goals and non-goals

**Goals**
- Update firmware over Wi-Fi, with a recovery path that survives a broken *or hostile* test firmware.
- A small, reusable device layer (Wi-Fi, provisioning, discovery, OTA, info, streaming) that every test app links in.
- **v1 focus: reliable Wi-Fi upgrades with a basic app** (info, versions, MCU temperature, debug/log streaming).
- Test apps for individual ICs plug in as modules that register **stream channels** (data out).
- A Python PC app to find, configure, update and monitor boards on the local network.

**Non-goals (v1)**
- Secure boot, flash encryption, TLS, user accounts, cloud connectivity.
- Updating the 2nd-stage bootloader or partition table over Wi-Fi (USB only, see §3.6).
- Production/field deployment hardening. This is a lab/bench tool.
- Remote commands to the device, CSV recording in the PC app (planned, see §12).

---

## 2. Target and toolchain

| Item | Choice |
|---|---|
| MCU | ESP32-S3 (any module), **8 MB flash minimum** (4 MB layout in §3.7) |
| Framework | ESP-IDF, **one pinned release** for bootloader, recovery and apps (see §3.6 on upgrades): **v6.0.3** |
| Language | C (C++ allowed in test modules) |
| PC app | Python 3.11+, Windows first, Linux should work |

Modules with **octal flash** need their own bootloader/recovery build (flash mode is fixed in the bootloader).

---

## 3. Boot architecture

### 3.1 Design invariants

These are the rules every later section must keep. Each acceptance test in §9 maps to one of them.

| # | Invariant |
|---|---|
| I1 | **A power cycle never makes things worse.** Any stuck state is cleared by at most: one power cycle, or three quick power cycles. |
| I2 | **Recovery can be reached without the app's cooperation.** All entry paths that matter are decided in the bootloader, not the app. |
| I3 | **Recovery depends on nothing a test app can break**: not the app's NVS, not PSRAM, not board peripherals, not the app slots. |
| I4 | **Nothing reachable over Wi-Fi can change the bootloader, partition table or eFuses.** |
| I5 | **Never two unverified images at once.** The app is only updated while recovery is known-good, and recovery only while the running app is known-good. |
| I6 | **Wi-Fi settings never cause a rollback or a lock-out.** Bad settings → access-point mode. |

### 3.2 Overview

The ESP32 2nd-stage bootloader cannot do Wi-Fi. "A bootloader that updates over Wi-Fi" is implemented as a
**recovery app** in the `factory` partition, plus two OTA app slots, plus a small **custom bootloader** that decides
when to start recovery.

```
ROM bootloader (fixed in silicon)
  └─ devlink bootloader = ESP-IDF 2nd-stage bootloader + small override (flashed once over USB)
       ├─ factory : RECOVERY image – minimal, always reachable, updated rarely
       ├─ ota_0   : TEST APP slot A
       └─ ota_1   : TEST APP slot B
```

- Normal update: the running app writes the *other* slot and reboots into it.
- New app broken (crash, hang, API never up) → bootloader **rolls back** to the previous slot.
- App broken later, or hostile (sleeps forever, kills Wi-Fi), or user wants it → bootloader starts **recovery** (§3.4).

### 3.3 Bootloader configuration (frozen at first USB flash – must be right in v1)

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
- RTC watchdog enabled (`CONFIG_BOOTLOADER_WDT_ENABLE=y`, default) and **kept running into the app**:
  apps are built with `CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE=y` (§3.5).
- `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y` (small retained-RTC area for the one-shot flags in §3.4, CRC protected).
- Image validation on every boot stays **on** (no "skip validation" / fast-boot options), so a corrupted slot is
  detected and the bootloader falls back to another slot or factory.
- Standard `CONFIG_BOOTLOADER_FACTORY_RESET` is **not** used: it erases `otadata` (loses which slot is good).
  The custom logic in §3.4 replaces it.
- Anti-rollback, secure boot, flash encryption: **off**.
- Flash mode **DIO** (works on every quad-flash module), 8 MB, 80 MHz. Bootloader RTC WDT 9 s; the image takes it
  over as its first step (§3.5).
- All of this lives in `tools/sdkconfig.defaults.devlink`, included by every image. Each build keeps its `sdkconfig`
  in its build directory, so build variants (e.g. test builds) can't leak into each other.

### 3.4 Boot decision (custom bootloader override, ~250 lines)

#### Boot log (`dlboot` partition, flash)

Borrowed from ESPHome's safe mode: boot attempts are counted **in flash**, so the count survives power loss and a crash
loop interrupted by power cycles is still caught.

- One **1-byte record per boot**, appended by the bootloader. Flash bits only go 1 → 0 without an erase, so later
  stages mark progress by clearing more bits in the same byte:

  | Bit (cleared = set) | Written by | Meaning |
  |---|---|---|
  | b0 `STARTED` | bootloader | a boot happened |
  | b1 `RECOVERY` | bootloader, recovery | this boot went to `factory` (recovery also sets it itself, which covers the loader falling back to `factory`) |
  | b2 `ALIVE_5S` | app / recovery | ran ≥ 5 s |
  | b3 `STABLE` | app / recovery | ran ≥ 120 s (app: also marked valid) |
  | b4 `CLEAN_RESTART` | app / recovery | planned restart via `devlink_restart()` (OTA, `/reboot`, config change) |

- 4 KB sector = 4096 boots per erase. The bootloader erases the sector only when it is full, after carrying the last
  8 records over (write them into the fresh sector). Wear is negligible.
- **Deep-sleep wake-ups are not logged** (reset reason checked first), so sleeping test apps don't trip the counters.
- Torn write (power loss while writing a record) leaves a byte with only some bits cleared; the rules below treat it
  as an unconfirmed boot, which errs towards recovery, never towards a brick. Any byte other than `0xFF` is a record;
  a torn write that left `0xFF` is simply reused by the next boot. Power loss between the rollover erase and the
  carry-over leaves an empty log: the guards only start counting again (never a wrong recovery).
- The running image finds its own record as the last non-`0xFF` byte and marks it (`ALIVE_5S` at 5 s, `STABLE` at
  120 s once valid, `CLEAN_RESTART` in `devlink_restart()`).
- **Host resets are planned restarts.** When the reset reason is a chip reset by the PC over USB-Serial/JTAG
  (`RESET_REASON_CORE_USB_UART` from esptool/`idf.py` DTR/RTS, or `RESET_REASON_CORE_USB_JTAG`), the bootloader sets
  `CLEAN_RESTART` in the interrupted boot's record before deciding. Repeated flashing during development therefore
  never trips the quick-reset escape or the crash-loop guard. Power-on and the EN button report a power-on reset,
  so they still count as quick resets (I2 unchanged); crashes and watchdog resets have their own reset reasons and
  still count towards the crash-loop guard.

#### Decision order, on every reset

1. **One-shot recovery request**: retained-RTC flag `REQ_RECOVERY`, set by app/recovery before `devlink_restart()`.
   → clear flag, boot `factory`. Used for "reboot to recovery" (button 10 s, API, USB console, recovery self-test §7.3).
   Does not touch `otadata`, so slot states are kept. A power cycle clears it (→ back to app).
   **Recovery loop** (checked before rules 2 and 3): the last **3** records are all `RECOVERY` boots that lack
   `STABLE` and `CLEAN_RESTART` → rules 2 and 3 do not send the board to `factory` again.
2. **Quick-reset escape** (physical, needs no app code): the last **3** records all lack `ALIVE_5S` (and
   `CLEAN_RESTART`), and no recovery loop → boot `factory`. Triggered by 3 power cycles **or 3 presses of the
   EN/reset button**, each within ~5 s of the previous boot. Works even if the app is hostile or foreign (deep sleep
   forever, Wi-Fi off, hung, non-devlink). Planned restarts don't count, so quick OTA/`/reboot` sequences are safe.
3. **Crash-loop guard**: the last **5** records all lack `STABLE` and `CLEAN_RESTART` → boot `factory`.
   Catches crashes, watchdog resets, brownouts and hangs-then-power-cycle, whatever the reset reason.
   - If there is a recovery loop (recovery is crash-looping too) → boot the last valid app slot instead (standard
     selection, but the OTA slots are tried before `factory`), boot reason `recovery_loop`. A broken recovery can't
     become a brick while a good app exists.
   - v0.4 applied this exception to rule 3 only. A recovery that crashes before 5 s never sets `ALIVE_5S`, so rule 2
     kept re-entering it and rule 3 was never reached (found in the milestone 1 host tests). The exception now
     covers both rules. This **strengthens** I1/I2; the only cost: with a broken recovery, 3 quick power cycles
     right after 3 recovery crashes boot the app instead of recovery, which is the only useful choice anyway.
4. **Recovery GPIO** (optional, board config, non-strapping pin only): held low at reset → boot `factory`.
5. Otherwise: standard ESP-IDF selection (otadata + rollback; falls back to the other slot or `factory` if the selected
   image fails validation; `factory` if otadata is blank or both copies are corrupt).

Thresholds (3 / 5 s, 5 / 120 s) are compile-time constants in the bootloader, so pick them before the first USB flash.

The bootloader passes the **boot reason** to the image in retained RTC
(`normal`, `rollback`, `requested`, `quick_reset`, `crash_loop`, `gpio`, `no_valid_app`, `recovery_loop`,
`deep_sleep`), reported by `/info`. Because the counters live in flash, they don't depend on retained RTC surviving a
brownout reset.

Retained-RTC block (`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE` = 16, `rtc_retain_mem_t.custom`, layout frozen):
`magic "DLLT"` (u32), `version` (u8), `req` (u8, `0xA5` = REQ_RECOVERY), `boot_reason` (u8), `target` (u8),
`start_offset` (u32, partition the bootloader started), `crc32` (u32) — own CRC, because IDF's CRC does not cover the
custom area. The bootloader first makes IDF's retain area valid (`bootloader_common_update_rtc_retain_mem`):
otherwise the IDF loader clears the whole area, custom block included, on the first boot after power-on.
If the loader then rejects the chosen image and falls back, the image sees `start_offset` ≠ running partition and
reports `no_valid_app`.

Implementation: `bootloader_components/devlink_boot` (record format, rules, RTC block: pure C, also linked into
recovery and app, host-tested in `test/host`) and `bootloader_components/main` (copy of the IDF v6.0.3
`bootloader_start.c`, partition selection replaced by `devlink_boot_select()`).

The bootloader is the one part that cannot be fixed without USB. It gets its own unit tests (boot-log parsing, torn
records, full sector, every decision branch) and the full fault-injection run in §9 before any board gets it.

### 3.5 App update safety rules

1. A new app boots in `PENDING_VERIFY`.
2. **Hardware watchdog during verify**: the RTC WDT from the bootloader stays running. devlink reconfigures it to 90 s
   as the first thing in `app_main`, and disables it only after the app is marked valid. A hang (deadlock, stuck
   driver, infinite loop with interrupts off) → hardware reset → rollback. The task WDT alone does not catch a deadlock
   where all tasks are blocked.
3. The app marks itself valid (`esp_ota_mark_app_valid_cancel_rollback`) once the **self-test** passes within 60 s:
   - the HTTP API is listening, **and**
   - the network is usable: STA connected, **or** the config AP is up and serving.
4. **Wi-Fi settings are not part of the decision (I6).** Missing/wrong credentials or no network → config AP opens, the
   self-test passes on the AP path, the app stays.
5. Rollback therefore only happens when the firmware itself is broken: reset before marking valid (crash, watchdog,
   power loss) or self-test not passed in 60 s (it then calls `esp_ota_mark_app_invalid_rollback_and_reboot`).
6. After rollback, the restored app reports `boot_reason=rollback` and the rejected version, so the PC app can show it.
7. The devlink boot steps run **before** anything else in `app_main`, in this order: RTC WDT take-over, boot-log
   timers (`ALIVE_5S` at 5 s, `STABLE` at 120 s and after marking valid), config load. Test modules start only after that.
8. All planned restarts go through `devlink_restart()`, which sets `CLEAN_RESTART` first, so OTA reboots and
   `/reboot` never count towards the crash-loop guard.

Implementation (milestone 3, `dl_app.c`):
- The self-test runs in `devlink_start(DEVLINK_MODE_APP)`: every 500 ms it checks that the HTTP server task answers
  (queued work, < 5 s old) and that the STA is connected or the config AP is on. Pass → `devlink_mark_valid()`
  (valid + RTC WDT off) — on **every** boot, not only after an OTA, because the RTC WDT runs from the bootloader on
  every boot.
- Not passed within `CONFIG_DEVLINK_SELFTEST_S` (60 s) while `PENDING_VERIFY` → `devlink_restart()`. The bootloader
  then finds the image still pending and rolls back itself, so the restored app reports `boot_reason=rollback`
  (`esp_ota_mark_app_invalid_rollback_and_reboot()` would mark it invalid first and the reason would be lost).
- Not passed while the app is already valid → no rollback possible; the RTC WDT stays on and resets the board after
  90 s, and the crash-loop guard takes a repeatedly failing app to recovery.
- Test builds: `bootlab` fault variants (`test/fw/bootlab_{panic,hang,noapi}.defaults`) for tests 5, 6, 7.

### 3.6 What can only be changed over USB

Bootloader, partition table, eFuses, flash mode/size. `tools/flash_all.py` is the only tool that writes them.
- Apps link with `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y` (default): writes to the bootloader/partition-table region abort.
- Apps and recovery build with `CONFIG_EFUSE_VIRTUAL=y` so a test module cannot burn eFuses permanently
  (wrong VDD_SPI or download-mode-disable eFuses are a true brick). Verify MAC/ID reads are unaffected.
- **IDF upgrades**: newer apps must stay compatible with the frozen bootloader. Any IDF bump runs the full §9 test set
  on a board with the *old* bootloader before release.

### 3.7 Partition tables

**8 MB (default)**

| Name | Type | SubType | Offset | Size | Owner |
|---|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 0x6000 (24 KB) | test app only |
| otadata | data | ota | 0xF000 | 0x2000 | IDF |
| phy_init | data | phy | 0x11000 | 0x1000 | IDF |
| dlcfg | data | 0x40 (custom) | 0x12000 | 0x2000 (2 × 4 KB) | devlink config (§5.3) |
| dlboot | data | 0x41 (custom) | 0x14000 | 0x1000 | boot log (§3.4) |
| factory | app | factory | 0x20000 | 0x180000 (1.5 MB) | recovery |
| ota_0 | app | ota_0 | 0x1A0000 | 0x300000 (3 MB) | app |
| ota_1 | app | ota_1 | 0x4A0000 | 0x300000 (3 MB) | app |
| coredump | data | coredump | 0x7A0000 | 0x10000 (64 KB) | IDF |
| storage | data | littlefs | 0x7B0000 | 0x50000 (320 KB) | test app |

**4 MB (optional)**: same first 0x20000; factory 1 MB, ota_0/ota_1 1.375 MB each, coredump 64 KB, no storage.

The partition table is frozen. Every app and recovery build checks at startup that the partitions it needs exist with the
expected offsets; a mismatch is logged and reported in `/status`.

### 3.8 First flash (USB, once per board)

`tools/flash_all.py`: erase flash → bootloader + partition table + recovery → `factory` + app → `ota_0` →
otadata set to `ota_0` (`otatool.py switch_ota_partition --slot 0`) → optional `dlcfg` image with Wi-Fi + label →
read back and verify all hashes → reset and wait for the device to report on serial.

---

## 4. Device identity

| Field | Source | Example |
|---|---|---|
| `id` | last 3 bytes of factory base MAC (eFuse), hex | `A1B2C3` |
| `mac` | full base MAC – **the PC app's primary key** | `F4:12:FA:A1:B2:C3` |
| `hostname` | `esp-<id>` (lowercase) | `esp-a1b2c3` |
| `label` | user-set friendly name, `dlcfg`, max 32 UTF-8 bytes | `PSU board rev B #4` |
| `board` | compile-time board/variant string | `s3-devkitc-1` |

Stable across firmware updates and config erase.

---

## 5. Firmware: devlink core (shared by app and recovery)

### 5.1 Components

```
bootloader_components/
  devlink_boot/       bootloader override (§3.4)
components/
  devlink/            core: dlcfg, boot log, wifi, provisioning (AP + Improv), discovery, http/ws server, ota, info, log tap, button, LED
  devlink_sysmon/     built-in channels: mcu temperature, heap, rssi, uptime  (app only)
  mod_<ic>/           one per IC under test, publishes stream channels (template: mod_example)
recovery/             IDF project → factory image
app/                  IDF project → test app template
pc/                   Python PC app
tools/                flash_all.py, partition CSVs, shared sdkconfig.defaults, release script, host_tests.py
test/host/            host unit tests of the pure-C parts (boot log + rules, RTC block, dlcfg format)
test/fw/              test-only firmware: bootlab test app, test console, crash-test build options
test/hil/             hardware-in-the-loop scripts (§9)
```

A shared `sdkconfig.defaults.devlink` holds everything both images must agree on (bootloader options, partition table,
flash settings, AP password, API key, project prefix). App and recovery both include it; CI fails if they differ.

### 5.2 Version and boot information

Reported by `/api/v1/info` and in discovery TXT records:
- App: `esp_app_desc_t` (project name, version from `git describe`, build date/time, IDF version, ELF SHA).
- Recovery: descriptor read from `factory`, plus its image SHA-256 and last self-test result (§7.3).
- Bootloader: ESP-IDF bootloader descriptor (where provided by the IDF version) + devlink bootloader version.
- Running partition, mode (`app`/`recovery`), state and version of each slot, **boot reason** (§3.4),
  last rejected/rolled-back version, the last 16 boot-log records (decoded), reset reasons (ring of 8, in `dlcfg`).
- Chip model + revision, flash size, PSRAM size, partition table check result, API version.

### 5.3 Config storage (`dlcfg`)

devlink settings are **not** stored in NVS. NVS is left to the test app, which may fill it, corrupt it, or be built
with an IDF whose NVS format the recovery doesn't understand.

`dlcfg` = two 4 KB sectors, each holding one record: `magic, format_version, sequence, crc32, payload`.
- Read: newest valid record by sequence; none valid → defaults (no credentials → AP mode).
- Write: always into the *other* sector, then it becomes newest. Power loss during a write leaves the old record valid.
  The body is written first and the 16-byte header last, so a torn write never looks valid.
- Record layout (frozen): header `magic "DCFG"` (u32), `format_version` (u16), `payload_len` (u16), `sequence` (u32,
  newest = larger, wrap-safe), `crc32` (u32, over the 12 header bytes before it, then the payload); payload =
  `dlcfg_t` (`components/devlink/include/dlcfg_core.h`, 272 bytes in format 1). A shorter payload (older writer)
  keeps defaults for the missing fields. `tools/devlink_tools.py` builds the same record for `flash_all.py`.
- Payload: Wi-Fi (current + previous credentials), label, config-window times, log level, reset-reason ring.
- Format changes are additive only (new fields at the end, older readers ignore them), so an old recovery can read
  a config written by a newer app.
- Wi-Fi driver runs with `WIFI_STORAGE_RAM` and does not need NVS.

### 5.4 Wi-Fi and provisioning

**Credentials**: one network in v1. 2.4 GHz only (ESP32-S3), WPA2/WPA3-Personal, hidden SSIDs allowed, no Enterprise.

**Boot behaviour (app)** – identical after a fresh flash, reboot and OTA:

| Situation | Behaviour |
|---|---|
| No credentials | AP only, stays up until configured |
| Credentials stored | STA connects immediately; **in parallel** (APSTA) config AP open for `cfg_window_boot` (default 20 s, 0 = off) |
| STA fails for 30 s (bad password, network gone) | Config AP opens and stays open until STA connects or new settings are saved; STA keeps retrying |
| Button long-press (3 s) | Config AP open for `cfg_window_button` (default 120 s) |
| A client is connected to the AP | AP window extended until the client leaves (+10 s) |

**APSTA channel rule**: the AP follows the STA's channel, and STA reconnect scans hop channels, which drops phones/PCs
connected to the AP. So: **while a client is connected to the AP, STA retries are paused**; with no AP client, STA
retries every 15 s with back-off to 60 s.

**Changing Wi-Fi settings safely (I6)**
- Changed **over the LAN** (device currently connected): new settings are saved as *pending*; device reconnects with them.
  If they don't connect within 30 s, it reverts to the previous settings and reports `wifi_change_failed`.
  You cannot lose a working board by typing the wrong password. The device answers `{"mode":"pending"}` first and
  switches 1 s later: dropping the STA link at once lost the reply (found on the bench, test 24).
- Changed **through the config AP** (current settings already failing): saved directly.

**Config AP**
- SSID `ESP32S3-CFG-<id>` (app) / `ESP32S3-REC-<id>` (recovery). WPA2, password from the shared config
  (default `configure`), identical in app and recovery.
- IP 192.168.4.1, DHCP, captive-portal DNS redirect for browsers; the JSON API works without it.
- Tiny web page: scan list, SSID/password, friendly name, Save. Same JSON API as §5.6. The page carries the build-time
  API key (not a secret on a bench network, §1).
- Captive DNS answers only clients on the AP subnet (192.168.4.0/24); unknown paths from AP clients redirect to `/`.

**USB provisioning – Improv serial** (app and recovery)
- Wi-Fi over USB uses the open **Improv Wi-Fi serial** protocol (v1), implemented in devlink (`dl_improv.c`, host-tested;
  no registry component), on the native USB-Serial-JTAG port, alongside normal log output. Frames are written to the
  USB-Serial/JTAG driver directly, not through stdout: stdout turns every 0x0A byte (a length, checksum or data byte)
  into "
" and corrupts the frame (found with a 10-character device name).
- Supported Improv commands: current state, device info (firmware name/version, chip, device name = label or id),
  scan networks, send Wi-Fi settings. After connecting, the device returns its URL (`http://<ip>/`).
- Works from the PC app (pyserial) and from any browser tool that speaks Improv (e.g. ESP Web Tools), so a board can be
  set up from a laptop without our app.
- Settings received over Improv are applied with the AP rules (saved directly, §5.4): a wired user is physically present.
- Improv has no field for the label or recovery, so plain console lines remain: `devlink label <text>`,
  `devlink recovery`, `devlink restart`, and `devlink info` / `devlink status` (answer `devlink info {json}`, same
  JSON as the API). Improv frames start with `IMPROV`, so both can share the port.

### 5.5 Discovery

1. **mDNS**: `esp-<id>.local`, service `_devlink._tcp` port 80. TXT: `id`, `mac`, `label`, `board`, `mode`, `fw`,
   `rec`, `api`, `boot` (boot reason).
2. **UDP broadcast fallback** on UDP 47800: PC broadcasts `DEVLINK?`, device answers with the same fields as JSON.
3. **Unconfigured / failing devices** are not on the LAN; the PC app scans for `ESP32S3-CFG-*` and `ESP32S3-REC-*`
   Wi-Fi networks (§8.1).

### 5.6 HTTP API (port 80, JSON, prefix `/api/v1`)

- Mutating endpoints need header `X-Api-Key`. The key is **fixed at build time** in the shared config (not changeable
  over the API) so it can never be lost or mismatched between app and recovery.
- Mutating endpoints also need `X-Device-Id: <mac>`; the device rejects a mismatch. Protects against DHCP handing the
  IP to another board between discovery and upload.
- All request bodies and headers are size-limited; strings are length-checked and JSON-escaped.

| Method | Path | App | Rec | Purpose |
|---|---|---|---|---|
| GET | `/info` | ✓ | ✓ | identity, versions, boot info (§5.2) |
| GET | `/status` | ✓ | ✓ | uptime, heap, RSSI, IP, temp, unconfirmed-boot count, drops, last error |
| GET / POST | `/config` | ✓ | ✓ | label, config-window times, log level |
| POST | `/wifi` | ✓ | ✓ | `{ssid, password}` (safe change rules §5.4) |
| GET | `/wifi/scan` | ✓ | ✓ | visible networks |
| POST | `/ota?target=app` | ✓ | ✓ | raw app `.bin` body (§7) |
| POST | `/ota?target=recovery` | ✓ | – | raw recovery `.bin` (§7.3) |
| GET | `/ota/progress` | ✓ | ✓ | bytes written / total, state |
| POST | `/reboot` | ✓ | ✓ | `{target: "app" \| "recovery" \| "slot0" \| "slot1"}` – slot only if that image is valid |
| POST | `/factory-reset` | ✓ | ✓ | erase `dlcfg` (credentials, label) → AP mode. Test-app NVS untouched (separate option) |
| GET / DELETE | `/coredump` | ✓ | ✓ | download / clear last core dump |
| GET | `/channels` | ✓ | ✓ | stream channels (recovery: `log` only) |

### 5.7 Streaming (WebSocket `/ws`)

One WebSocket per client; max 2 clients.

PC → device: `{"op":"sub","ch":["sys.temp","log"]}`, `{"op":"unsub",...}`, `{"op":"rate","ch":"sys.temp","hz":2}`

Device → PC:
```json
{"ch":"sys.temp","ts":12345678,"v":41.5}
{"ch":"adc.raw","ts":12345678,"v":[1023,1018,1031]}
{"ch":"log","ts":12345678,"lvl":"W","tag":"wifi","msg":"..."}
```

- `ts` = µs since boot. The PC app adds its own receive time.
- Channel metadata: name, unit, type (number/array/object), default and max rate.
- Slow client → frames dropped (counter in `/status`), never unbounded queues.
- `ESP_LOG*` output is tapped into a ring buffer → channel `log`, and still goes to USB serial.
  `devlink_debug(ch, fmt, ...)` publishes to custom debug channels.
- **All streaming pauses during an OTA** (memory and CPU go to the update).

Implementation (milestone 4, `dl_stream.c`, `dl_stream_core.c`):
- One WebSocket message may carry **several frames, newline-separated** (one JSON object per line); the PC splits
  lines. Ops get a reply (`{"op":"sub","ok":[...],"unknown":[...]}`, `{"op":"rate","ch":..,"hz":..}`,
  `{"op":"error","error":..}`); `{"op":"unsub"}` without `ch` ends all subscriptions; `{"op":"list"}` = `/channels`.
  A new connection gets `{"op":"hello","api":1}`; a third one gets `too_many_clients` and is closed.
- Rate per client and channel: `hz` 0 = every value (events, e.g. `log`); a request above `max_hz` is clamped.
  Samples up to 1/8 period early are accepted, so a producer at exactly the requested rate keeps it.
- Each client has a fixed ring (32 frames × 256 B). The single sender task only hands a batch to the HTTP server task
  when the socket is writable: a stalled client never blocks the API (or, in recovery, the health check). Its new
  frames are dropped and counted (`/status` `drops`); a client that cannot take data for 10 s is closed.
- Log tap: `esp_log_set_vprintf` hook, Log V1 (one vprintf call per line, pinned in the shared config); never blocks
  a logging task (lock busy → counted drop); not from ISRs. Logs written while the OTA lock is held are not streamed.
- In IDF v6 the URI handler is not called for the WebSocket handshake: clients are registered in the post-handshake
  callback (`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT`).
- Recovery has the `log` channel only (no sysmon, §6.1). Producers can check `devlink_channel_wanted_hz()` and skip
  measuring while nobody listens.

### 5.8 Module API

```c
devlink_channel_t *ch = devlink_channel_register(&(devlink_channel_cfg_t){
    .name = "ina219.bus_v", .unit = "V", .type = DL_NUM, .default_hz = 10, .max_hz = 200 });
devlink_publish_num(ch, value);
```
Built-in channels: `sys.temp` (die temperature, low absolute accuracy), `sys.heap`, `sys.rssi`, `log`.

**Rules for test modules** (documented, checked in code review – they can't all be enforced):
- Don't use GPIO19/20 (native USB) unless the board config says so; it removes the USB serial/auto-download path.
- Don't use the button/LED pins, strapping pins as outputs that load GPIO0, or `esp_deep_sleep_start` without a wake-up
  source. Use `devlink_sleep()`, which refuses to sleep without a timer wake-up.
- Don't write to `factory`, `dlcfg`, `dlboot`, `otadata`.

### 5.9 Button and status LED

- Button default GPIO0 (BOOT), runtime only. **GPIO0 low at reset enters ROM download mode**, so it can't be a
  bootloader input; anything on the board holding GPIO0 low makes every reset land in download mode.
  - 3 s hold → config AP. 10 s hold → reboot into recovery (one-shot flag).
  - A button that reads pressed at boot is treated as stuck and ignored until released.
  - Implementation: `CONFIG_DEVLINK_BUTTON_GPIO` (default 0, -1 = none), app only, sampled every 50 ms. The AP action
    fires once when the hold reaches 3 s, recovery when it reaches 10 s (no release needed). Logic in `dl_button.c`
    (host-tested). Recovery does not use the button (its AP is always on).
- Status LED (optional): config AP open, connecting, connected, OTA in progress, recovery – distinct patterns.

---

## 6. Recovery image requirements

Recovery is the safety net, so it is held to stricter rules than the app.

### 6.1 Content
devlink core only: `dlcfg`, Wi-Fi + provisioning (AP + USB), discovery (`mode=recovery`), API subset (§5.6), `log`
channel, core dump download. No test modules, no sysmon, no other channels. Target size < 1.2 MB (25 % headroom).
Milestone 2 build: 0.92 MB. Wi-Fi without NVS (`CONFIG_ESP_WIFI_NVS_ENABLED=n`, `WIFI_STORAGE_RAM`), PHY calibration
not stored. Until the release script exists, recovery is built with the HIL test console
(`CONFIG_DEVLINK_TEST_CONSOLE=y`); a release recovery must be built without it.

### 6.2 Minimal hardware assumptions (I3)
- **No PSRAM** enabled (works on modules with and without PSRAM, and on a board with a damaged PSRAM).
- Conservative flash settings that every supported module handles.
- **Touches no GPIO** except the button (input) and LED. Every other pin stays at its reset state, so recovery can't
  drive anything on a board under test.
- No dependency on NVS, app slots, `storage`, or app-owned data. Blank/corrupt `dlcfg` → defaults → AP mode.
- Works with `otadata` blank, either slot empty/corrupt, or both.

### 6.3 Always reachable
- Config AP `ESP32S3-REC-<id>` is **always on** in recovery (APSTA when credentials work, AP only otherwise).
- mDNS + UDP discovery + USB console.
- No idle timeouts that shut anything down.

### 6.4 Self-healing
- RTC WDT stays enabled for the whole recovery run (30 s), fed every 2 s by a health task only while the HTTP server
  task (proved by queued work) and the Wi-Fi task report heartbeats (< 15 s old). Task WDT on all tasks (10 s, both
  idle tasks, panic).
- Wi-Fi errors → restart the Wi-Fi stack; after 3 failed restarts → `devlink_restart()` (clean restart, not a crash).
- Marks `ALIVE_5S` / `STABLE` in the boot log like the app does.
- **Brownout handling**: boards under test often have weak supplies, and Wi-Fi TX peaks cause brownout loops.
  Recovery starts Wi-Fi at reduced TX power (e.g. 11 dBm), and if the last reset was a brownout it waits 2 s and drops
  to 8 dBm. Brownout counts are reported.

### 6.5 Writing an app from recovery
- Target slot: the slot that is **not** the last valid one (keep the good one as a fallback). If neither is valid, ota_0.
- After the image is complete and verified: set boot partition, `devlink_restart()`. The new app goes through
  the normal verify (§3.5).

### 6.6 Leaving recovery
- `/reboot target=app` (only if a valid app exists), successful OTA, or a power cycle.
- If recovery was entered by crash loop or quick-reset escape, `/info` says so, and the PC app shows it.

---

## 7. OTA protocol (app and recovery)

### 7.1 Device side
1. Check headers: API key, `X-Device-Id`, `Content-Length` present and ≤ target partition size.
2. Take the **single OTA lock** (second request → HTTP 409). Pause streaming.
3. Validate the first bytes before writing anything:
   - image magic `0xE9`, chip ID = ESP32-S3, min/max chip revision vs this chip;
   - app descriptor magic present (rejects merged/bootloader images and ELF files);
   - flash size in the header ≤ this device's flash;
   - **devlink marker**: a constant struct with magic + devlink API version that every devlink build contains (found
     by scanning the stream); missing → reject unless `force=1`;
   - project-name prefix check unless `force=1`.
4. `esp_ota_begin` with **sequential erase** (`OTA_WITH_SEQUENTIAL_WRITES`): no multi-second full-slot erase that stalls
   the HTTP server or trips the task watchdog.
5. Stream to flash in small chunks; **idle timeout 10 s** → `esp_ota_abort`, release lock, report error. The device is
   ready for a new attempt without a reboot.
6. `esp_ota_end` (SHA-256 check) → set boot partition → reply `200` with the new version → reboot after 500 ms.

Implementation notes (milestone 2, found on the bench):
- The upload runs in its **own task** (esp_http_server async request), not in the HTTP server task. The server task is
  single-threaded: with the upload inside it, a second upload simply waited and then succeeded (no 409, test 10), and
  in recovery a long upload starved the HTTP heartbeat, so the health task would let the RTC WDT reset the board
  mid-update. Concurrent uploads are answered 409 by a small reject task.
- Rejections after the header **drain the rest of the body** before replying (bounded by the largest slot and the
  10 s idle timeout): closing while the client still sends makes its TCP stack reset the connection and the error
  reply is lost. A `Content-Length` larger than the slot is answered `413` at once without draining; then only the
  status line is guaranteed to reach the client.
- esp_http_server only dispatches a request once body bytes arrive; a client that stalls right after the headers gets
  the server's own `408`.

### 7.2 PC side
- Re-checks the file before upload (same rules, shows project/version/date/chip).
- Uploads by MAC (rediscovers the current IP first), sends `X-Device-Id`.
- After reboot waits up to 120 s for the same MAC to reappear; checks: version = new, `boot_reason=normal`, slot valid.
  If the old version is back, shows **"rolled back"** with the rejected version and reset reasons.
- **One device at a time per OTA "batch step"**: multi-select runs sequentially (or N parallel, configurable), stops on
  the first failure.

### 7.3 Recovery update (rare, guarded – I5)
- Only from a running app that is **valid** and whose last update succeeded; the PC app refuses if the app is pending
  or was just rolled back. Never in the same session step as an app update.
- Written straight into `factory` (no A/B for recovery). Image verified (`esp_image_verify`) after writing.
- **Recovery self-test boot**: app sets `REQ_RECOVERY` + `SELFTEST` and reboots. Recovery starts, brings up Wi-Fi and HTTP,
  writes `recovery_selftest=ok` to retained RTC and immediately reboots back to the app. If recovery crashes or hangs,
  the RTC WDT resets it and, because the flag was one-shot, the bootloader boots the app again. The app reports the result.
- Failed self-test → PC app shows a red warning and offers to re-flash the previous recovery file (kept in its history).

Implementation (milestone 6): the self-test state lives in **dlcfg**, not in retained RTC. The bootloader writes a
fresh RTC block on every boot, so a `SELFTEST` flag and its result would not survive the two reboots, and changing the
frozen bootloader for it is not worth the risk. dlcfg fields (appended, §5.3): `rec_selftest` (none / pending / ok /
failed) and `rec_selftest_version`.
- App, `POST /ota?target=recovery`: refused (409) in recovery, while the app is `pending_verify`, or after a rollback;
  images whose project name lacks "recovery" need `force=1`. Header checks as §7.1; `factory` is erased sector by
  sector while the data arrives, then `esp_image_verify`. Then `rec_selftest=pending` + version, reply
  `{"selftest":"running"}`, one-shot reboot into recovery.
- Recovery with `pending`: AP up and HTTP answering within 30 s → `ok`, else `failed`; then `devlink_restart()` into
  the app. A crash or hang never records anything; the app then finds `pending` at boot (`devlink_boot_early`) and
  records `failed`. `/info` reports `recovery_selftest {state, version}`.

---

## 8. PC app (Python)

Stack: PySide6 (Qt), pyqtgraph, zeroconf (mDNS), httpx, websockets/QtWebSockets, pyserial (Improv serial client),
`netsh` wrapper (Wi-Fi scan / join on Windows). PyInstaller single exe.

Implementation (milestone 5, `pc/devlink_pc/`, venv `pc/.venv`, `pc/requirements.txt`, run `pc/.venv/Scripts/python pc/devlink.py`):
- A Qt-free library (`image`, `api`, `discovery`, `ota`, `improv`, `wifi_win`, `configure`) used by the GUI and by
  the HIL tests; the GUI runs every network call in a worker thread. `requests` instead of httpx.
- OTA uploads send `Content-Length` (the upload body object needs `tell()`, or requests falls back to chunked).
- The configure flow forces a Windows Wi-Fi scan (WlanScan) before joining/rejoining an AP; netsh output is parsed in
  English.
- The bench Wi-Fi password is never stored; the configure dialog asks for it each time.
- Monitor tab (milestone 6): Qt Charts (part of PySide6) instead of pyqtgraph, so no extra dependency. Channel table
  with subscribe + rate per channel and the latest value; plot of the last 30 s (arrays: one line per element); log
  console (level and text filter, pause, save); status line from `/status` every 2 s. The WebSocket client
  (`stream.MonitorSession`) is Qt-free and reconnects after a reboot by finding the board again by MAC (its IP may
  change), then replays the subscriptions and rates.
- Firmware tab: target "recovery" with its own file history, an extra confirmation and the self-test result; on a
  failed self-test it offers the previous recovery file.

### 8.1 Devices tab
- Table keyed by **MAC**: id, label, IP, hostname, mode (app / recovery / needs-config), boot reason, app ver,
  recovery ver + self-test state, RSSI, last seen. Rows with `rollback`, `crash_loop`, `quick_reset` boot reasons are
  highlighted.
- Sources: mDNS browse + UDP broadcast (every 5 s + manual refresh); manual "add by IP".
- **Needs-config devices**: Wi-Fi scan for `ESP32S3-CFG-*` / `ESP32S3-REC-*` (on scan button and every 15 s).
  **Configure** flow: remember current Wi-Fi profile → join the device AP with a temporary, non-auto-connect profile →
  `POST /wifi` + label within a few seconds (Windows may switch away from a network without internet) → restore the
  previous network → wait for the device to appear on the LAN → confirm. Retries the POST if the join was dropped.
  Fallback: **USB** – the app lists serial ports that answer Improv "device info" and configures them the same way.
- Actions: connect (Monitor), open web page, set friendly name, change Wi-Fi (safe-change rules), reboot / reboot to
  recovery / boot other slot, factory reset, download core dump.

### 8.2 Firmware tab
- Pick `.bin`; parse and show project, version, build date, IDF version, chip, min chip rev, devlink marker/API version.
  Block wrong chip / no devlink marker (override with a confirm + warning).
- Target devices (multi-select), target: app (default) or recovery (§7.3 rules, extra confirm).
- Per-device progress and result (§7.2). Keeps the last few app **and recovery** files.

### 8.3 Monitor tab
- Channel list from `/channels`, subscribe, per-channel rate; live plots, readouts, log console with filter/search;
  save log to text. Status bar: uptime, heap, RSSI, drops, boot reason, connection state; auto-reconnect after reboot.

### 8.4 Settings
API key, discovery options, default rates, data folder, bench Wi-Fi SSID/password used by the configure flow.

---

## 9. Acceptance tests (v1)

Grouped by invariant. All run on the oldest bootloader in use.

**Normal operation**
1. Fresh board: `flash_all.py` → no creds → `ESP32S3-CFG-<id>` → configure from PC app → on LAN with label in ≤ 30 s.
2. 50 OTAs in a row from the PC app, alternating slots, no failures.
3. Stream `sys.temp` 10 Hz + `log` for 1 h without disconnect.
4. mDNS blocked → found via UDP broadcast.

**Broken new app → rollback (I1)**
5. App that panics at start → rollback, PC app shows "rolled back".
6. App that hangs (deadlock, all tasks blocked) before self-test → RTC WDT → rollback.
7. App that boots but never starts the API → rollback after 60 s.
8. Power cut at 10 %, 50 %, 99 % of an OTA, and right after reboot into the new image → old app boots or new app
   verifies; never stuck.
9. Upload aborted (PC killed, AP dropped) → device recovers without reboot; next upload succeeds.
10. Two PCs upload at once → second gets 409, first succeeds.

**Hostile / broken valid app → recovery (I2)**
11. Valid app that crash-loops after 40 s → recovery after 5 loops; same result if the loop is interrupted by power cycles.
12. Valid app that deep-sleeps forever / turns Wi-Fi off / disables USB → **3 quick power cycles** or **3 reset-button
    presses** → recovery.
13. Foreign (non-devlink) image uploaded with `force=1` → reachable again via the quick-reset escape.
13a. App that deep-sleeps with a 10 s timer wake-up for 1 h → never enters recovery (wake-ups not logged).
13b. 200 OTAs + `/reboot`s in a row → never enters recovery (`CLEAN_RESTART`).
13c. Boot log: power cut while the record is written (100 tries); sector full and rolled over → decisions still correct.
14. Button 3 s → config AP; 10 s → recovery; power cycle in recovery → back to app.

**Recovery independence (I3)**
15. App fills NVS completely / writes garbage to NVS → recovery and app Wi-Fi config still work.
16. `dlcfg`: power cut during a config write (100 tries) → previous config always valid.
16a. Improv: configure a board over USB from the PC app and from ESP Web Tools in a browser.
17. Both app slots erased (esptool) → boots recovery, accepts an app.
18. Corrupt one byte in the active slot → bootloader falls back to the other slot / recovery.
19. Supply at the brownout edge → recovery still comes up (reduced TX power) and accepts an OTA.
20. Recovery built with crash at start (test build) → device falls back to the valid app (§3.4 rule 3).

**Updates & config safety (I4–I6)**
21. Wrong-chip / merged / ELF / no-marker / too-large images → rejected with a clear error, nothing written.
22. Wrong `X-Device-Id` → rejected.
23. Recovery update with a broken recovery → self-test fails, app keeps running, PC app warns, re-flash old recovery works.
24. Change Wi-Fi over LAN to a wrong password → device reverts within 30 s, still on the LAN.
25. Good app OTA to a device whose saved password is wrong → no rollback; config AP; configure from PC app → on LAN.
26. App writing to the bootloader region / burning an eFuse (test build) → aborted / virtual only.

---

## 10. Failure-mode table (brick review)

"Brick" = needs USB or a programmer. Target: **no row ends in a brick without physical damage or USB-only changes.**

| # | Scenario | Without mitigation | Mitigation (section) | Remaining risk |
|---|---|---|---|---|
| 1 | New app crashes at boot | – (IDF rollback) | §3.5 | none |
| 2 | New app hangs, no crash | stuck in PENDING_VERIFY until power cycle | RTC WDT kept through verify §3.5 | none |
| 3 | New app OK but Wi-Fi settings wrong | rollback loop / lost board | I6, AP path counts as pass §3.5 | none |
| 4 | Valid app crash-loops later (also across power cycles) | stuck forever | flash boot log, crash-loop guard §3.4 | none |
| 5 | Valid app hangs / sleeps / kills Wi-Fi / is foreign | **brick** | quick-reset escape (power or EN button) §3.4 | needs someone at the bench |
| 6 | "Reboot to recovery" via `set_boot_partition(factory)` | erases otadata, slot history lost | one-shot RTC flag §3.4 | none |
| 7 | Recovery itself broken | **brick** if app also broken | I5, recovery self-test §7.3, recovery loop → app §3.4 | both broken at once (prevented by I5) |
| 8 | Test app fills/corrupts NVS, or newer NVS format | recovery can't read config / erases it | `dlcfg` partition §5.3, `WIFI_STORAGE_RAM` | none |
| 9 | API key changed and forgotten | locked out | key fixed at build time §5.6 | none |
| 10 | Wi-Fi changed over LAN to wrong values | board leaves network | pending + revert §5.4 | none |
| 11 | AP password differs between app and recovery builds | can't join AP | shared config + CI check §5.1 | none |
| 12 | APSTA: STA scans drop the AP client | provisioning fails randomly | pause STA retries while AP has a client §5.4 | none |
| 13 | Power loss during OTA / otadata / dlcfg write | – | A/B everywhere §7, §5.3 | none |
| 14 | Upload dropped mid-way | OTA handle stuck until reboot | idle timeout + abort §7.1 | none |
| 15 | Full-slot erase stalls HTTP / trips WDT | failed or partial update | sequential erase §7.1 | none |
| 16 | Wrong file (other chip, merged bin, ELF, newer chip rev, bigger flash) | rollback at best, confusion | header checks §7.1 | none |
| 17 | Upload to wrong board after DHCP change | wrong board flashed | `X-Device-Id` §5.6 | none |
| 18 | Test module burns eFuses | **permanent brick** | `CONFIG_EFUSE_VIRTUAL` §3.6 | code that bypasses the eFuse API |
| 19 | Test module writes bootloader/partition table | **brick** | `DANGEROUS_WRITE_ABORTS` §3.6 | raw SPI register access |
| 20 | Test module writes factory/dlcfg/dlboot | recovery lost | module rules §5.8; recovery SHA reported at every app boot | deliberate raw writes |
| 21 | Test module uses GPIO19/20 or holds GPIO0 low | USB/download path lost | module rules §5.8 | needs BOOT button + USB |
| 22 | Weak supply → brownout loop | boot loop incl. recovery | reduced TX power + back-off §6.4 | supply too weak for any Wi-Fi |
| 23 | PSRAM missing/damaged, or app built for other PSRAM/flash type | app crash | rollback; recovery has no PSRAM §6.2 | none |
| 24 | IDF upgrade makes app incompatible with frozen bootloader | all boards affected | pinned IDF, upgrade test on old bootloader §3.6 | none |
| 25 | Partition table needs to change | can't over Wi-Fi | frozen layout, startup check §3.7 | USB re-flash |
| 26 | Bootloader bug | **brick** | small code, unit tests, full §9 fault run before rollout | USB re-flash |
| 27 | Network blocks mDNS / broadcast / client isolation | can't find boards | UDP fallback, add-by-IP; isolation = unsupported | bench network choice |
| 28 | 5 GHz-only or Enterprise Wi-Fi | can't join | documented; AP mode keeps board reachable | bench network choice |
| 29 | Long input / malformed request to recovery | crash → loop | size limits, fuzz test §5.6 | none |
| 30 | Windows drops the device AP (no internet) during configure | configure fails | fast POST, retry, non-auto profile §8.1 | Improv over USB fallback |
| 31 | Deep-sleeping test app trips the boot counters | unwanted recovery | wake-ups not logged §3.4 | none |
| 32 | Torn boot-log write / full sector | wrong decision | torn = unconfirmed; carry-over on erase; unit tests §3.4 | errs towards recovery |

---

## 11. Milestones

1. **Bootloader override (boot log + decision rules) + partition table + `dlcfg` + `flash_all.py`**, with tests 11–13c,
   16–18, 20 (bootloader is frozen after this, so it is done and tested first).
2. Recovery image: Wi-Fi/AP/Improv provisioning, discovery, API subset, OTA, self-healing (§6).
3. devlink in the app: boot guard, verify + RTC WDT, OTA, info/status, safe Wi-Fi change.
4. Streaming + `sys.*` channels + log tap; `mod_example`.
5. PC app: Devices (incl. configure flow) + Firmware tabs.
6. PC app: Monitor tab; Improv USB provisioning; recovery update + self-test.
7. Full acceptance run (§9), README.

---

## 12. Open questions

- Flash size of the boards in use (8 MB vs 4 MB layout)? Any octal-flash modules? *(Bench board 1: ESP32-S3 rev
  v0.1, 8 MB quad flash, 2 MB embedded PSRAM → 8 MB layout. Other boards still open.)*
- Do the boards have a spare non-strapping pin for a recovery button/jumper (optional §3.4 rule 4)?
- Bench network: same subnet as the PC, mDNS/broadcast allowed, no client isolation, 2.4 GHz WPA2/WPA3-Personal?
- Are the boot-log thresholds right for your bench: 3 resets within ~5 s → recovery; 5 boots that never reach 120 s → recovery?
- Do multiple people need to watch the same board at once (max WS clients)?

---

## 13. Planned for later (not in v1)

- **Remote commands**: modules register commands (`/commands`, `/cmd/<name>`, `cmd` op on the WebSocket).
- **CSV recording** in the Monitor tab.
- Multiple stored Wi-Fi networks, binary stream frames, TLS / signed images.

---

## 14. Prior art and reuse (checked 2026-10-27)

No maintained project matches the whole spec (ESP-IDF native + separate recovery image + generic test-app template +
Python desktop tool). Decision: build devlink as a thin layer on official ESP-IDF components, borrowing proven ideas.

| Project | Status | What we take |
|---|---|---|
| ESPHome `safe_mode` | very active, ESP-IDF default since 2026.1 | **adopted (§3.4)**: boot counting in flash (survives power loss); reset-button presses count; separate recovery image |
| Tasmota safeboot | active (15.x, 2026) | same factory-partition recovery architecture; known issue to avoid: devices stuck in safeboot with no way back |
| `espressif/network_provisioning` | official, registry | not used for v1 (needs phone app); reference for protocomm security levels |
| Improv Wi-Fi (serial) | open standard, ESP-IDF component on registry | **adopted (§5.4)** for USB Wi-Fi setup; works with ESP Web Tools too |
| ESP-IDF `app_update`, `esp_http_server` (WebSocket), `espressif/mdns` | official | used directly |
