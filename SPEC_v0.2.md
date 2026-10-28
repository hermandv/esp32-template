# ESP32-S3 Bench Template – Specification (v0.2 draft)

A reusable firmware + PC tool template for bringing up and testing ESP32-S3 hardware boards over Wi-Fi.
Working name for the shared firmware layer: **devlink**.

---

## 1. Goals and non-goals

**Goals**
- Update firmware over Wi-Fi, with a recovery path that survives a broken test firmware.
- A small, reusable device layer (Wi-Fi, provisioning, discovery, OTA, info, streaming) that every test app links in.
- **v1 focus: reliable Wi-Fi upgrades with a basic app** (info, versions, MCU temperature, debug/log streaming).
- Test apps for individual ICs plug in as modules that register **stream channels** (data out).
- A Python PC app to find, configure, update and monitor boards on the local network.

**Non-goals (v1)**
- Secure boot, flash encryption, TLS, user accounts, cloud connectivity.
- OTA updates of the 2nd-stage bootloader itself (see §3.2).
- Production/field deployment hardening. This is a lab/bench tool.
- Remote commands to the device, CSV recording in the PC app (planned, see §11).

---

## 2. Target and toolchain

| Item | Choice |
|---|---|
| MCU | ESP32-S3 (any module), **8 MB flash minimum** (4 MB layout in §3.4) |
| Framework | ESP-IDF 5.x (pin the exact version in `README` / CI) |
| Language | C (C++ allowed in test modules) |
| PC app | Python 3.11+, Windows first, Linux should work |

---

## 3. Boot architecture

### 3.1 Overview

Note: the ESP32 2nd-stage bootloader cannot do Wi-Fi. "A bootloader that updates over Wi-Fi" is implemented as a
**recovery app** in the `factory` partition, plus OTA-capable app slots.

```
ROM bootloader (fixed in silicon)
  └─ 2nd-stage bootloader (ESP-IDF, flashed once over USB)
       ├─ factory : RECOVERY image  – minimal: Wi-Fi, provisioning, discovery, OTA, info. Never changes in normal use.
       ├─ ota_0   : TEST APP slot A
       └─ ota_1   : TEST APP slot B
```

- Normal update: the running test app receives the new image over Wi-Fi, writes the *other* ota slot, reboots into it.
- If the new app crashes or never gets its API running, the bootloader **rolls back** to the previous slot.
  Missing or wrong Wi-Fi settings are **not** a reason to roll back: the app goes to access-point mode instead (§3.3).
- If the app is unusable (boot loop, Wi-Fi broken, user request), the device boots **recovery**, which can always
  receive a new app.

### 3.2 2nd-stage bootloader

Bootloader settings are fixed at the first USB flash, so they must be right in v1:
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (required for §3.3).
- `CONFIG_BOOTLOADER_FACTORY_RESET=y` with a **dedicated, non-strapping GPIO** (optional per board, see §6.3).
  Data partitions to erase on that path: **none** (keep Wi-Fi credentials).
- The bootloader is *not* updated over Wi-Fi in v1. Its version is still reported (§5.2).

### 3.3 Update safety rules

1. New app boots in `PENDING_VERIFY` state.
2. The app marks itself valid (`esp_ota_mark_app_valid_cancel_rollback`) once its **self-test** passes within 60 s:
   - the HTTP API is listening, **and**
   - the network is usable: either STA connected, **or** the config AP is up and serving.
3. **Wi-Fi settings are not part of the rollback decision.** If credentials are missing, or the STA can't connect
   (wrong password, network gone), the app opens the config AP (§5.3), passes the self-test on the AP path and stays
   on the new firmware. The user fixes the settings through the AP; no rollback happens.
4. Rollback therefore only happens when the new firmware itself is broken: it crashes/resets before marking valid,
   or it fails to bring up the API/AP within 60 s (it then reboots itself → bootloader rolls back).
5. **Boot-loop guard**: a counter in RTC no-init memory is incremented early in `app_main` and cleared after 30 s of
   stable run. At ≥ 3 consecutive early resets the app sets the boot partition to `factory` and restarts.
6. **Image checks before accepting an OTA upload** (both app and recovery):
   - ESP image magic and chip ID = ESP32-S3.
   - `esp_app_desc_t.project_name` starts with the configured prefix (default `devlink`), unless `force=1`.
   - Image fits the target partition; SHA-256 verified by `esp_ota_end`.
7. Recovery partition can be updated from the running test app (not from itself) with an explicit
   `target=recovery` + `force=1`. Considered advanced/rare.

### 3.4 Partition tables

**8 MB (default)**

| Name | Type | SubType | Offset | Size |
|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 0x6000 (24 KB) |
| otadata | data | ota | 0xF000 | 0x2000 |
| phy_init | data | phy | 0x11000 | 0x1000 |
| factory | app | factory | 0x20000 | 0x180000 (1.5 MB) |
| ota_0 | app | ota_0 | 0x1A0000 | 0x300000 (3 MB) |
| ota_1 | app | ota_1 | 0x4A0000 | 0x300000 (3 MB) |
| coredump | data | coredump | 0x7A0000 | 0x10000 (64 KB) |
| storage | data | littlefs | 0x7B0000 | 0x50000 (320 KB) |

**4 MB (optional)**: factory 1 MB, ota_0/ota_1 1.375 MB each, coredump 64 KB, no storage.

### 3.5 First flash (USB, once per board)

`tools/flash_all.py` (wraps `esptool` / `idf.py`):
bootloader + partition table + recovery → `factory` + test app → `ota_0`, then sets otadata to boot `ota_0`
(`otatool.py switch_ota_partition --slot 0`). Optionally writes Wi-Fi credentials and label to NVS in the same step.

---

## 4. Device identity

| Field | Source | Example |
|---|---|---|
| `id` | last 3 bytes of factory base MAC (eFuse), hex | `A1B2C3` |
| `mac` | full base MAC | `F4:12:FA:A1:B2:C3` |
| `hostname` | `esp-<id>` (lowercase) | `esp-a1b2c3` |
| `label` | user-set friendly name, NVS, max 32 chars | `PSU board rev B #4` |
| `board` | compile-time board/variant string | `s3-devkitc-1` |

The id is stable across firmware updates and NVS erase.

---

## 5. Firmware: devlink core

### 5.1 Components

```
components/
  devlink/            core: wifi, provisioning, discovery, http/ws server, ota, info, log tap, button, status LED
  devlink_sysmon/     built-in channels: mcu temperature, heap, rssi, uptime
  mod_<ic>/           one per IC under test, publishes stream channels (template: mod_example)
recovery/             IDF project → factory image (devlink core only, minimal streaming: log only)
app/                  IDF project → test app template (devlink + sysmon + modules)
pc/                   Python PC app
tools/                flash_all.py, partition CSVs, release script
```

### 5.2 Version information

Reported by `/api/v1/info` and in discovery TXT records:
- App: `esp_app_desc_t` (project name, version from `git describe`, build date/time, IDF version, ELF SHA).
- Recovery: app descriptor read from the `factory` partition.
- 2nd-stage bootloader: ESP-IDF bootloader descriptor (`esp_bootloader_desc_t`, where the IDF version provides it),
  else `"unknown"`.
- Running partition, boot mode (`app` / `recovery`), OTA state of each slot.
- Chip model + revision, flash size, PSRAM size, reset reason, API version.

### 5.3 Wi-Fi and provisioning

**Credentials** stored in NVS (one network in v1; up to 3 is a stretch goal).

**Boot behaviour** (identical after a fresh flash, a reboot and an OTA update)

| Situation | Behaviour |
|---|---|
| No credentials | AP mode only, stays up until configured |
| Credentials stored | Connect STA immediately. **In parallel** (APSTA), open the config AP for `cfg_window_boot` (default 20 s, 0 = off) |
| STA fails for 30 s (bad password, network missing) | Open config AP (APSTA) and keep it open until STA connects or new settings are saved; keep retrying STA in the background |
| Button long-press (3 s) | Open config AP for `cfg_window_button` (default 120 s) |
| A client is connected to the AP | AP window is extended until the client leaves (+10 s) |

Running APSTA means normal connection is never delayed by the config window. Note that the AP follows the STA's channel.

**Config AP**
- SSID `ESP32S3-CFG-<id>`, WPA2 with password from Kconfig (default `configure`), or open if configured so.
- IP 192.168.4.1, captive-portal DNS redirect.
- Tiny web page: scan list, SSID/password, **friendly name (label)**, Save & Reboot. Same JSON API as §5.5, so the PC
  app can configure the device through the AP without a browser.

**USB provisioning (recommended)**: over the native USB-CDC console, a line command
`devlink wifi <ssid> <password>` / `devlink label <text>`. Lets the PC app configure a board that's plugged in
without joining its AP.

### 5.4 Discovery

1. **mDNS**: hostname `esp-<id>.local`, service `_devlink._tcp` on port 80.
   TXT: `id`, `label`, `board`, `mode` (`app`/`recovery`), `fw`, `rec` (recovery version), `api`.
2. **UDP broadcast fallback** (mDNS is often blocked on corporate/guest Wi-Fi): device listens on UDP 47800;
   PC broadcasts `DEVLINK?`; device replies with the same fields as JSON.
3. **Unconfigured devices** (or devices whose Wi-Fi settings fail) are not on the LAN. The PC app finds them by
   scanning for `ESP32S3-CFG-*` Wi-Fi networks and configures them (§7.1).

### 5.5 HTTP API (port 80, JSON, prefix `/api/v1`)

Mutating endpoints need header `X-Api-Key` (shared bench key, Kconfig default, changeable via API).
Read-only endpoints are open.

| Method | Path | Purpose |
|---|---|---|
| GET | `/info` | identity + versions (§4, §5.2) |
| GET | `/status` | uptime, free heap/min heap, RSSI, IP, temp, reset reason, boot-loop count |
| GET / POST | `/config` | **label (friendly name)**, config-window times, API key (write-only), log level |
| POST | `/wifi` | `{ssid, password}` → saved, reboot or reconnect |
| GET | `/wifi/scan` | visible networks |
| POST | `/ota?target=app\|recovery&force=0\|1` | raw `.bin` body, streamed to flash; returns result, then reboots |
| GET | `/ota/progress` | bytes written / total (for UIs that can't track upload progress) |
| POST | `/reboot` | `{target: "app"\|"recovery"}` |
| POST | `/factory-reset` | erase NVS (credentials, label), reboot into AP mode |
| GET / DELETE | `/coredump` | download / clear last core dump |
| GET | `/channels` | list of stream channels + metadata |

### 5.6 Streaming (WebSocket `/ws`)

One WebSocket per client; max 2 clients.

PC → device
```json
{"op":"sub","ch":["sys.temp","log"]}
{"op":"unsub","ch":["sys.temp"]}
{"op":"rate","ch":"sys.temp","hz":2}
```

Device → PC
```json
{"ch":"sys.temp","ts":12345678,"v":41.5}
{"ch":"adc.raw","ts":12345678,"v":[1023,1018,1031]}
{"ch":"log","ts":12345678,"lvl":"W","tag":"wifi","msg":"..."}
```

- `ts` = microseconds since boot (`esp_timer_get_time`). The PC app records its own receive time too.
- Each channel has a name, unit, value type (number, array, object), default rate and max rate.
- Frames are dropped (not queued forever) if the client is slow, with a drop counter in `/status`.
- **Debug data**: `ESP_LOG*` output is tapped (`esp_log_set_vprintf`) into a ring buffer and published on channel
  `log`, while still going to USB serial. A user `devlink_debug(ch, fmt, ...)` call publishes to custom debug channels.
- v2 option: binary frames for high-rate channels.

### 5.7 Module API (how a test module adds stream data)

```c
// declare a stream channel
devlink_channel_t *ch = devlink_channel_register(&(devlink_channel_cfg_t){
    .name = "ina219.bus_v", .unit = "V", .type = DL_NUM, .default_hz = 10, .max_hz = 200 });
devlink_publish_num(ch, value);
```

Built-in channels: `sys.temp` (internal sensor, °C – die temperature, low absolute accuracy), `sys.heap`, `sys.rssi`, `log`.

### 5.8 Button and status LED

- Button: default GPIO0 (BOOT). Works at runtime only; **holding GPIO0 at reset enters ROM download mode**, so it cannot be
  used for the bootloader recovery path.
  - 3 s hold → open config AP. 10 s hold → reboot into recovery.
- Status LED (optional, board config; addressable RGB or plain GPIO):
  config AP open, connecting, connected, OTA in progress, recovery mode – each a distinct pattern.

### 5.9 Recovery image

Contains: Wi-Fi, provisioning (AP + USB), discovery (`mode=recovery`), `/info`, `/status`, `/wifi`, `/ota` (app slots only),
`/reboot`, `/config` (label), `/coredump`, `log` channel. Nothing else. Target size < 1.2 MB.
After a successful app OTA it sets the new slot as boot partition and reboots.

---

## 6. Board configuration

### 6.1 Kconfig / `sdkconfig.defaults`
Board name, button GPIO + active level, LED type + GPIO, recovery GPIO (or none), AP password, API key, project-name prefix,
config-window defaults.

### 6.2 Versioning
`PROJECT_VER` from `git describe --tags --dirty`. Recovery and app versioned separately (`rec-v1.0.0`, `app-v1.2.0`).

### 6.3 Recovery GPIO
Only if the board has a spare, non-strapping pin with a button/jumper. Strapping pins on ESP32-S3 (GPIO0, 3, 45, 46)
must not be used.

---

## 7. PC app (Python)

Stack: PySide6 (Qt), pyqtgraph (plots), zeroconf (mDNS), httpx (HTTP), websockets or QtWebSockets, pyserial (USB provisioning), pywifi or `netsh` (Wi-Fi scan / join on Windows).
Distribute as a PyInstaller single exe for Windows.

### 7.1 Devices tab
- Table: id, label, IP, hostname, mode (app / recovery / config-AP), app ver, recovery ver, RSSI, last seen.
- Sources: mDNS browse + UDP broadcast (auto refresh every 5 s, manual refresh button).
- **Unconfigured devices**: when the scan button is pressed (and every 15 s while the tab is open), the app scans
  Wi-Fi networks for `ESP32S3-CFG-<id>` (Windows `netsh wlan show networks`) and lists them as "needs config".
  **Configure** flow: the PC joins that AP (temporary Wi-Fi profile with the known AP password) → `POST /wifi`
  with the bench SSID/password + label → PC returns to its previous network → waits for the device to appear on the
  LAN and confirms. Falls back to USB serial provisioning if the PC has no Wi-Fi adapter.
- Actions: connect (opens Monitor), open web page, **set friendly name**, change Wi-Fi settings,
  reboot / reboot to recovery, factory reset, download core dump.
- Manual "add by IP" for networks where discovery is blocked.

### 7.2 Firmware tab
- Pick `.bin`; app parses the ESP app descriptor and shows project name, version, build date, IDF version, chip.
  Warns on wrong chip / wrong project prefix.
- Select one or **multiple** target devices, choose target slot (app / recovery – recovery behind a confirm).
- Progress per device, then waits for the device to reappear and checks the reported version matches.
- Remembers the last few firmware files.

### 7.3 Monitor tab
- Channel list from `/channels`; tick to subscribe; per-channel rate.
- Live plots (rolling window), numeric readouts, log console with level/tag filter and search.
- Save the log console to a text file.
- Status bar: uptime, heap, RSSI, drops, connection state; auto-reconnect after reboot.

### 7.4 Settings
API key, discovery options, default rates, data folder.

---

## 8. Acceptance tests (v1)

1. Fresh board: `flash_all.py` → boots app; with no creds shows `ESP32S3-CFG-<id>`; configure via phone → appears in PC app within 10 s.
2. OTA app 20 times in a row from PC app, alternating slots, no failures.
3. OTA an app that crashes at start → device rolls back automatically, PC app shows the previous version.
4. OTA an app that boots but never brings up Wi-Fi → rollback after 60 s.
5. Boot-loop on an already-valid app (simulate) → device ends up in recovery and is OTA-able.
6. Button 3 s → config AP visible; button 10 s → recovery mode.
7. Upload a non-S3 / wrong-project image → rejected with a clear error.
8. Stream `sys.temp` at 10 Hz + `log` for 1 h without disconnect.
9. Power-cut during OTA → device still boots the old app.
10. mDNS blocked → device still found via UDP broadcast.
11. OTA a good app to a device whose saved Wi-Fi password is wrong → **no rollback**; device opens the config AP,
    PC app lists it as "needs config", configure it from the PC app → device appears on the LAN with the new version.
12. Unconfigured board: PC app finds its AP, sets Wi-Fi + friendly name, device appears on the LAN with that name.

---

## 9. Milestones

1. Partition table, bootloader config, `flash_all.py`, recovery image with OTA + `/info`.
2. devlink core in app: Wi-Fi/provisioning, discovery, info/status, OTA with rollback + boot-loop guard.
3. Streaming + `sys.*` channels + log tap; `mod_example`.
4. PC app: Devices tab (incl. AP scan + configure) + Firmware tab.
5. PC app: Monitor tab; USB provisioning.
6. Acceptance tests (§8), README.

---

## 10. Open questions

- Flash size of the boards in use (8 MB vs 4 MB layout)?
- Is a shared API key enough, or should OTA be fully open on the bench network?
- Bench network: same subnet as the PC, or behind something that blocks mDNS/broadcast?
- Any boards without a usable runtime button (then config AP relies on boot window / USB only)?
- Do multiple people need to watch the same board at once (affects max WS clients)?

---

## 11. Planned for later (not in v1)

- **Remote commands**: modules register commands (`/commands`, `/cmd/<name>`, `cmd` op on the WebSocket), e.g. I2C scan,
  register read/write, run self-test; PC app generates a command panel from the schema.
- **CSV recording** in the Monitor tab (pc_time, device_ts, channel, value).
- Multiple stored Wi-Fi networks, binary stream frames, TLS / signed images.
