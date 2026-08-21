#!/usr/bin/env python3
"""Fetch box art for every ROM in ~/roms from libretro-thumbnails and emit
the menu's icon format: 48x48 RGB565 big-endian raw, named
<rom name without extension>.raw.

Output goes to ~/roms/art/ -- copy that folder to the SD card's /art next
time the card is in the PC (or once a serial file-push exists).

Usage: scrape_art.py [roms_dir]
"""
import io
import pathlib
import sys
import urllib.parse
import urllib.request

from PIL import Image

ICON_W = ICON_H = 48
REPOS = {
    ".gba": ["Nintendo_-_Game_Boy_Advance"],
    ".gbc": ["Nintendo_-_Game_Boy_Color", "Nintendo_-_Game_Boy"],
    ".gb":  ["Nintendo_-_Game_Boy", "Nintendo_-_Game_Boy_Color"],
}
BASE = "https://raw.githubusercontent.com/libretro-thumbnails/{}/master/Named_Boxarts/"
# Japanese GB Pokemon use romaji names in no-intro
JP_ALIAS = {
    "Red": "Aka", "Green": "Midori", "Blue": "Ao", "Yellow": "Pikachu",
    "Gold": "Kin", "Silver": "Gin", "Crystal": "Crystal Version",
}

def libretro_name(stem: str) -> str:
    # libretro replaces &*/:`<>?\| with underscores in thumbnail filenames
    out = []
    for ch in stem:
        out.append("_" if ch in "&*/:`<>?\\|" else ch)
    return "".join(out)

def to_raw(img: Image.Image) -> bytes:
    img = img.convert("RGB").resize((ICON_W, ICON_H), Image.LANCZOS)
    buf = bytearray()
    for r, g, b in img.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        buf += bytes(((v >> 8) & 0xFF, v & 0xFF))  # big-endian for the panel
    return bytes(buf)

def main():
    roms = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                        pathlib.Path.home() / "roms")
    out = roms / "art"
    out.mkdir(exist_ok=True)
    for rom in sorted(list(roms.glob("*.gba")) + list(roms.glob("*.gb")) +
                      list(roms.glob("*.gbc"))):
        stem = rom.stem
        ext = rom.suffix.lower()
        dst = out / f"{stem}.raw"
        if dst.exists():
            print(f"  = {stem} (already have it)")
            continue
        # candidate thumbnail names: exact, then common renames of the
        # user's shorthand ("Pokemon Emerald JP") to no-intro names
        cands = [stem]
        base = stem.replace(" JP", "").replace("Pokemon ", "")
        jp = JP_ALIAS.get(base, base)
        cands.append(f"Pocket Monsters - {base} (Japan)")
        cands.append(f"Pocket Monsters {jp} (Japan)")
        cands.append(f"Pocket Monsters - {jp} (Japan)")
        cands.append(f"Pokemon - {base} Version (USA, Europe)")
        cands.append(f"Pokemon - {base} Version (USA)")
        cands.append(f"Pokemon - {base} Version (USA, Europe) (SGB Enhanced)")
        cands.append(f"Pokemon - {base} Version (USA, Europe) (GB Compatible)")
        cands.append(f"Pokemon - {base} Version (USA, Europe) (SGB Enhanced) (GB Compatible)")
        if base == "Yellow":
            cands.append("Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe) (CGB+SGB Enhanced)")
            cands.append("Pocket Monsters - Pikachu (Japan) (SGB Enhanced)")
        png = None
        for repo in REPOS.get(ext, REPOS[".gba"]):
            for cand in cands:
                url = BASE.format(repo) + urllib.parse.quote(libretro_name(cand) + ".png")
                try:
                    with urllib.request.urlopen(url, timeout=20) as r:
                        png = r.read()
                    print(f"  + {stem} <- {repo}/{cand}")
                    break
                except Exception:
                    continue
            if png:
                break
        if png is None:
            print(f"  x {stem}: no thumbnail found")
            continue
        dst.write_bytes(to_raw(Image.open(io.BytesIO(png))))

if __name__ == "__main__":
    main()
