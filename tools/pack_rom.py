#!/usr/bin/env python3
"""Pre-pack GBA ROMs on the PC into the console's sparse-pack cache format.

The console stores carts in flash with every all-0xFF 64KB page aliased to a
single shared erased page and every all-0x00 page aliased to a single shared
zero page (see romCopyFromSd in menu.cpp). On first pick it must scan the
whole ROM from SD to find those pages -- for a 16MB cart that is 16MB of SD
reads before the flash write even starts.

This tool does the scan on the PC and writes the exact cache files the
console looks for, next to the ROM:

    <name>.gba.pak   distinct-content 64KB pages, back to back, in page order
    <name>.gba.map   8-byte magic "GBAMAP2\\0", then uint16 per page:
                     0 = 0xFF alias, 0xFFFF = 0x00 alias, else pak slot number

Copy ROM + .pak + .map into the card's /roms and /packed directories:

    /sd/roms/<name>.gba
    /sd/packed/<name>.gba.pak
    /sd/packed/<name>.gba.map

and the console skips the scan entirely.

The zero-alias matters beyond speed: Emerald US has zero interior 0xFF
padding and 228 distinct pages against the flash partition's 223 slots.
Its 29 all-zero pages collapse to one shared slot (199 + 2 shared), which is
the difference between fitting and silently losing the last six pages --
the ones its title screen graphics live in.

The map header is mandatory: headerless (v1) caches are ignored by the
console because a v1 cache written before the capacity wall was understood
can describe a truncated pack as complete.

Usage: pack_rom.py game.gba [more.gba ...]   (writes into ./packed/)
"""
import os
import struct
import sys

PAGE = 65536
BLANK = b"\xff" * PAGE
ZERO = b"\x00" * PAGE
MAP_ZERO = 0xFFFF
MAP_MAGIC = b"GBAMAP2\x00"


def pack(path: str, outdir: str):
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 0xC0 or data[0xB2] != 0x96:
        print(f"{path}: NOT a GBA ROM -- skipped")
        return

    n_pages = (len(data) + PAGE - 1) // PAGE
    name = os.path.basename(path)
    os.makedirs(outdir, exist_ok=True)
    pak_path = os.path.join(outdir, name + ".pak")
    map_path = os.path.join(outdir, name + ".map")

    slots = []
    page_map = []
    zeros = 0
    next_slot = 1  # slot 0 is the shared erased page on the device
    for i in range(n_pages):
        page = data[i * PAGE:(i + 1) * PAGE]
        if len(page) < PAGE:
            page = page + b"\xff" * (PAGE - len(page))
        if page == BLANK:
            page_map.append(0)
        elif page == ZERO:
            page_map.append(MAP_ZERO)
            zeros += 1
        else:
            page_map.append(next_slot)
            slots.append(page)
            next_slot += 1

    with open(pak_path, "wb") as f:
        for page in slots:
            f.write(page)
    with open(map_path, "wb") as f:
        f.write(MAP_MAGIC)
        f.write(struct.pack("<%dH" % n_pages, *page_map))

    print(f"{name}: {n_pages} pages, {len(slots)} packed "
          f"({len(slots)*PAGE/1048576:.2f} MB pak, "
          f"{n_pages - len(slots) - zeros} blank + {zeros} zero aliased)")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    for path in args:
        pack(path, os.path.join(os.path.dirname(os.path.abspath(path)), "packed"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
