"""Build and run the host unit tests (pure-C firmware logic) with the PC's gcc.

Usage: py -3.12 tools/host_tests.py [name ...]     (names: bootlog, dlcfg; default all)
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "host_tests"
BOOT = ROOT / "bootloader_components" / "devlink_boot"
DEVLINK = ROOT / "components" / "devlink"

# test name -> (test source, firmware sources, include dirs)
TESTS = {
    "bootlog": (
        "test/host/test_bootlog.c",
        [BOOT / "dl_bootlog.c", BOOT / "dl_rtc.c"],
        [BOOT / "include"],
    ),
    "dlcfg": (
        "test/host/test_dlcfg.c",
        [DEVLINK / "dlcfg_core.c", BOOT / "dl_rtc.c"],
        [DEVLINK / "include", BOOT / "include"],
    ),
    "json": (
        "test/host/test_json.c",
        [DEVLINK / "dl_json.c"],
        [DEVLINK / "include"],
    ),
    "ota_check": (
        "test/host/test_ota_check.c",
        [DEVLINK / "dl_ota_check.c"],
        [DEVLINK / "include", BOOT / "include"],
    ),
    "improv": (
        "test/host/test_improv.c",
        [DEVLINK / "dl_improv.c"],
        [DEVLINK / "include"],
    ),
    "wifi_policy": (
        "test/host/test_wifi_policy.c",
        [DEVLINK / "dl_wifi_policy.c"],
        [DEVLINK / "include"],
    ),
}

CFLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wconversion", "-Wno-sign-conversion"]


def run(name: str) -> bool:
    test_src, srcs, incs = TESTS[name]
    test_path = ROOT / test_src
    if not test_path.exists():
        print(f"== {name}: skipped ({test_src} not found)")
        return True
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / f"test_{name}.exe"
    cmd = ["gcc", *CFLAGS, *[f"-I{i}" for i in incs], f"-I{ROOT / 'test' / 'host'}",
           str(test_path), *map(str, srcs), "-o", str(exe)]
    print(f"== {name}: build")
    if subprocess.run(cmd).returncode != 0:
        return False
    print(f"== {name}: run")
    return subprocess.run([str(exe)]).returncode == 0


def main() -> int:
    names = sys.argv[1:] or list(TESTS)
    failed = [n for n in names if not run(n)]
    print("\nALL PASSED" if not failed else f"\nFAILED: {', '.join(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
