# Installed development tools

Tools on the Windows 11 bench PC used by this project. Log every new install here (name, version, source, command).

| Item | Version | Source | Install | Location | Used for |
|---|---|---|---|---|---|
| ESP-IDF | v6.0.3 (2026-09-02) | https://github.com/espressif/esp-idf | `git clone -b v6.0.3 --depth 1 --recursive --shallow-submodules`, then `idf_tools.py install --targets=esp32s3` and `install-python-env` (2026-10-28) | `C:\projects\esp-idf\` (shared, see its `README.md`) | Bootloader, recovery and app builds (SPEC §2: one pinned release) |
| ESP-IDF Python env | Python 3.12.3, esptool 5.6.0 | IDF constraints file `espidf.constraints.v6.0.txt` | as above | `C:\projects\esp-idf\tools\python_env\idf6.0_py3.12_env` | `idf.py`, esptool, idf monitor |
| `espressif/mdns` (IDF component) | 1.14.0 (2026-10-02), pinned `==1.14.0` | https://components.espressif.com/components/espressif/mdns (Apache-2.0) | `components/devlink/idf_component.yml`; fetched by the IDF component manager at build time (2026-10-28, Herman approved) | `<project>/managed_components/` (git-ignored) | mDNS discovery (SPEC §5.5) |
| PC app Python venv: PySide6 6.11.2 (LGPL-3.0), zeroconf 0.151.5, requests 2.34.2, websockets 17.3, pyserial 3.5, pytest 9.1.1 | see `pc/requirements.txt` | https://pypi.org (PySide6: https://pypi.org/project/PySide6/) | `py -3.12 -m venv pc\.venv`, `pc\.venv\Scripts\python -m pip install -r pc\requirements.txt` (2026-10-28, Herman approved PySide6 6.11.2 in a venv) | `pc/.venv/` (git-ignored, ~680 MB) | PC app (SPEC §8) |

Already on the PC (from the Greenhouse setup): Git 2.55, Python 3.12.3 (`py -3.12`; `python` is not on PATH),
WinLibs g++ 16.2 (host tests), Cppcheck 2.22, LLVM/clang-format 23.1.3.

## Why v6.0.3

Latest bug-fix release of the newest stable line that already has bug-fix releases. v6.1 (2026-08-27) had no bug-fix release
yet, and the bootloader is frozen after the first USB flash (SPEC §3.3, §3.6), so maturity matters more than features.

## Check (2026-10-28)

`idf.py --version` → `ESP-IDF v6.0.3`. The `hello_world` example built for esp32s3 and ran on the bench board
(COM22, ESP32-S3 QFN56 rev v0.1, 8 MB quad flash, 2 MB embedded PSRAM, MAC 7c:df:a1:f2:50:30).
