"""First flash of a board over USB (SPEC §3.8). The ONLY tool that writes the bootloader and partition table.

  erase flash -> bootloader + partition table + recovery -> factory + app -> ota_0 -> otadata selects ota_0
  -> optional dlcfg (Wi-Fi + label) -> verify every region -> reset and wait for the app to report on serial.

Run from a PowerShell with ESP-IDF active (. C:\\projects\\esp-idf\\export-v6.0.3.ps1):
  py -3.12 tools/flash_all.py [--port COM22 | --mac 7c:df:a1:f2:50:30] [--app test/fw/bootlab]
                              [--label "PSU board #4"] [--ssid X --password Y] [--no-build] [--yes]

Milestone 1: the app defaults to the bootlab test app (test/fw/bootlab).
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
import tempfile
from pathlib import Path

import devlink_tools as dl

PARTITIONS = dl.ROOT / "tools" / "partitions_8mb.csv"
FLASH_ARGS = ["--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB"]


def partition_offsets(csv_path: Path) -> dict[str, int]:
    rows = (r for r in csv.reader(l for l in csv_path.read_text().splitlines() if l and not l.startswith("#")))
    return {r[0].strip(): int(r[3].strip(), 0) for r in rows}


def app_bin(build_dir: Path) -> Path:
    bins = [p for p in build_dir.glob("*.bin") if p.name not in ("ota_data_initial.bin",)]
    if len(bins) != 1:
        sys.exit(f"expected one app .bin in {build_dir}, found {[b.name for b in bins]}")
    return bins[0]


def esptool(port: str, *args: str) -> None:
    cmd = [*dl.esptool_cmd(), "--chip", "esp32s3", "--port", port, *args]
    print("+ esptool", " ".join(args))
    subprocess.run(cmd, check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--mac")
    ap.add_argument("--app", default="test/fw/bootlab", help="IDF project for ota_0 (relative to the repo)")
    ap.add_argument("--label", default="")
    ap.add_argument("--ssid", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--yes", action="store_true", help="don't ask before erasing the whole flash")
    args = ap.parse_args()

    recovery_dir = dl.ROOT / "recovery"
    app_dir = dl.ROOT / args.app
    b_rec = dl.ROOT / "build" / "recovery"
    b_app = dl.ROOT / "build" / app_dir.name

    if not args.no_build:
        dl.idf_py(recovery_dir, b_rec, "build")
        dl.idf_py(app_dir, b_app, "build")

    port = dl.find_port(args.port, args.mac)
    off = partition_offsets(PARTITIONS)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        otadata = tmp / "otadata.bin"
        otadata.write_bytes(dl.otadata_image(0))
        regions = [
            (0x0, b_rec / "bootloader" / "bootloader.bin"),
            (0x8000, b_rec / "partition_table" / "partition-table.bin"),
            (off["otadata"], otadata),
            (off["factory"], app_bin(b_rec)),
            (off["ota_0"], app_bin(b_app)),
        ]
        if args.label or args.ssid:
            dlcfg = tmp / "dlcfg.bin"
            dlcfg.write_bytes(dl.dlcfg_image(dl.dlcfg_payload(args.ssid, args.password, args.label)))
            regions.append((off["dlcfg"], dlcfg))

        for addr, path in regions:
            print(f"  0x{addr:06x}  {path.relative_to(dl.ROOT) if path.is_relative_to(dl.ROOT) else path.name}")
        if not args.yes:
            if input(f"Erase ALL flash on {port} and write the above? [y/N] ").strip().lower() != "y":
                return 1

        files = [x for addr, path in regions for x in (hex(addr), str(path))]
        # Erase + write in one session, then verify; only the final hard reset runs our bootloader (1 boot record).
        esptool(port, "--after", "no-reset", "write-flash", "--erase-all", *FLASH_ARGS, *files)
        esptool(port, "--after", "hard-reset", "verify-flash", *FLASH_ARGS, *files)

    con = dl.Console(port)
    m = con.wait_for(r"DLT ready image=(\S+)", timeout=30)
    if not m:
        print("\nFAILED: board did not report on serial within 30 s")
        return 1
    info = con.info()
    con.close()
    print(f"\nOK: {m.group(1)} running, boot info {info}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
