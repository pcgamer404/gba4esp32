#!/usr/bin/env python3
"""Build, then flash with a DIO bootloader and a QIO app. Linux & Windows.

This board's ROM loader cannot read the bootloader in QIO (the flash's Quad
Enable bit is not set that early), but the IDF bootloader sets QE and can
then read the app in QIO. The build produces everything QIO; only the
bootloader image is patched back down to DIO before flashing.

NEVER use `pio run -t upload`: it flashes the unpatched QIO bootloader and
the board boot-loops until rescued.

Usage:
    python tools/flash_qio.py [--port COM5 | --port /dev/ttyACM0] [--board X]

The port is auto-detected when omitted (esptool probes). The board defaults
to FNK0104AB, the Freenove FNK0104 this project targets.
"""
import argparse
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, ".pio", "build", "esp32s3")


def run(cmd, env=None):
    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=REPO, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("PORT"),
                    help="serial port (COM5, /dev/ttyACM0); auto-detect if omitted")
    ap.add_argument("--board", default=os.environ.get("ESP_GBA_BOARD",
                                                      "FNK0104AB"),
                    help="board variant (default FNK0104AB)")
    args = ap.parse_args()

    env = dict(os.environ)
    env["ESP_GBA_BOARD"] = args.board

    run(["pio", "run", "-e", "esp32s3"], env=env)

    boot_dio = os.path.join(tempfile.gettempdir(), "esp-gba-boot-dio.bin")
    run([sys.executable, os.path.join(REPO, "tools", "patch_qio.py"),
         os.path.join(BUILD, "bootloader.bin"), boot_dio, "dio"])

    esptool = ["pio", "pkg", "exec", "-p", "tool-esptoolpy", "--",
               "esptool.py", "--chip", "esp32s3"]
    if args.port:
        esptool += ["--port", args.port]
    esptool += ["--baud", "921600", "write_flash", "--flash_mode", "keep",
                "0x0", boot_dio,
                "0x8000", os.path.join(BUILD, "partitions.bin"),
                "0x10000", os.path.join(BUILD, "firmware.bin")]
    run(esptool, env=env)


if __name__ == "__main__":
    main()
