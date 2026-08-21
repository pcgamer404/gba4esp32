#!/usr/bin/env python3
"""Rewrite the app image to QIO flash mode, keeping the bootloader on DIO.

Why this exists: this board's *ROM* loader cannot read the second-stage
bootloader in QIO (it loads one bogus segment then watchdog-resets -- see
README "Flash mode must be DIO"). Most likely the flash's Quad Enable bit is not
set when the ROM loader runs. The IDF bootloader does set it, so once it is
running the app can be read in QIO perfectly well.

The flash mode lives in byte 2 of each image header independently, so the two
images can differ: bootloader DIO for the ROM loader, app QIO for speed. That
matters because the emulated GBA ROM is esp_partition_mmap'd from flash, so
every emulated instruction fetch that misses the data cache is a flash read.
QIO clocks 4 bits per cycle against DIO's 2.

esptool will not patch this itself ("Image file is protected with a hash
checksum, so not changing the flash mode setting"), so recompute the appended
SHA256 after editing the header.

  patch_qio.py in.bin out.bin [mode]     mode: qio (default) | dio
"""
import hashlib
import sys

MODES = {"qio": 0, "qout": 1, "dio": 2, "dout": 3}
NAMES = {v: k.upper() for k, v in MODES.items()}

src = sys.argv[1]
dst = sys.argv[2]
mode = MODES[(sys.argv[3] if len(sys.argv) > 3 else "qio").lower()]

img = bytearray(open(src, "rb").read())
if img[0] != 0xE9:
    raise SystemExit(f"{src}: not an ESP image (magic {img[0]:#x})")

hash_appended = img[23] == 1
if hash_appended and hashlib.sha256(bytes(img[:-32])).digest() != bytes(img[-32:]):
    raise SystemExit(f"{src}: existing digest does not verify; refusing to patch")

print(f"{src}: spi_mode {img[2]} ({NAMES[img[2]]}) -> {mode} ({NAMES[mode]})")
img[2] = mode

if hash_appended:
    img[-32:] = hashlib.sha256(bytes(img[:-32])).digest()
    print("  recomputed appended SHA256")

open(dst, "wb").write(bytes(img))
print(f"wrote {dst} ({len(img)} bytes)")
