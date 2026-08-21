#!/usr/bin/env python3
"""Trim the padding off GBA ROMs so they copy and pack faster.

Commercial carts are padded to a power of two: FireRed ships as 16MB with
only ~9.3MB of real content, and much of the padding is TRAILING 0xFF (or
0x00). Cutting the tail is loss-free -- the game never addresses it -- and
directly shrinks the SD read + flash write when the console packs the cart.

Interior holes (FireRed also has a 5.2MB hole in the middle) are NOT removed
here: byte offsets inside the ROM must stay valid. The console's sparse
packer already skips those at 64KB granularity when writing flash.

Usage:
  trim_rom.py game.gba [more.gba ...]      analyze + write game.trim.gba
  trim_rom.py --in-place game.gba          overwrite the original
  trim_rom.py --dry-run *.gba              report only

The output is aligned up to 64KB (the packer's page size) and never cut
below 1MB. A GBA header sanity check (fixed byte 0x96 at 0xB2) guards
against trimming something that is not a ROM.
"""
import os
import sys

PAGE = 65536          # console packer page size; keep alignment
MIN_SIZE = 1 << 20    # never trim below 1MB


def analyze(data: bytes):
    """Return (content_end, interior_blank_pages, total_pages)."""
    # Trailing padding: run of 0xFF or 0x00 from the end.
    end = len(data)
    pad = data[-1]
    if pad in (0xFF, 0x00):
        lo, hi = 0, len(data)
        # binary search is overkill; scan back in 64KB strides then refine
        while end > 0 and data[end - 1] == pad:
            # fast stride: whole 64KB block of padding?
            blk = max(0, end - PAGE)
            if data[blk:end].count(pad) == end - blk:
                end = blk
            else:
                end -= 1
    content_end = end

    blank_pages = 0
    total_pages = (len(data) + PAGE - 1) // PAGE
    ff_page = b"\xff" * PAGE
    zero_page = b"\x00" * PAGE
    for off in range(0, content_end, PAGE):
        page = data[off:off + PAGE]
        if page == ff_page or page == zero_page:
            blank_pages += 1
    return content_end, blank_pages, total_pages


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    in_place = "--in-place" in sys.argv
    dry = "--dry-run" in sys.argv
    if not args:
        print(__doc__)
        return 1

    for path in args:
        with open(path, "rb") as f:
            data = f.read()

        if len(data) < 0xC0 or data[0xB2] != 0x96:
            print(f"{path}: NOT a GBA ROM (no 0x96 header byte) -- skipped")
            continue

        content_end, blank_pages, total_pages = analyze(data)
        new_size = max(MIN_SIZE, (content_end + PAGE - 1) // PAGE * PAGE)

        print(f"{os.path.basename(path)}:")
        print(f"  file size      : {len(data)/1048576:7.2f} MB")
        print(f"  content ends at: {content_end/1048576:7.2f} MB")
        print(f"  trailing pad   : {(len(data)-new_size)/1048576:7.2f} MB (removable)")
        print(f"  interior blank : {blank_pages*PAGE/1048576:7.2f} MB "
              f"(console packs these out of flash automatically)")
        print(f"  trimmed size   : {new_size/1048576:7.2f} MB")

        if dry or new_size >= len(data):
            if new_size >= len(data):
                print("  -> nothing to trim")
            continue

        out = path if in_place else os.path.splitext(path)[0] + ".trim.gba"
        with open(out, "wb") as f:
            f.write(data[:new_size])
        print(f"  -> wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
