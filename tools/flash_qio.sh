#!/usr/bin/env bash
# Build, then flash with a DIO bootloader and a QIO app.
#
# This board's ROM loader cannot read the bootloader in QIO (the flash's Quad
# Enable bit is not set that early), but the IDF bootloader sets QE and can then
# read the app in QIO. sdkconfig builds everything QIO; only the bootloader
# image is patched back down to DIO. `pio run -t upload` would flash the
# unpatched QIO bootloader and boot-loop, so use this instead.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/esp-gba-boot-dio.bin
pio run -e esp32s3
python3 tools/patch_qio.py .pio/build/esp32s3/bootloader.bin "$OUT" dio
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port "${PORT:-/dev/ttyACM0}" \
  --baud 921600 write_flash --flash_mode keep \
  0x0 "$OUT" \
  0x8000 .pio/build/esp32s3/partitions.bin \
  0x10000 .pio/build/esp32s3/firmware.bin
