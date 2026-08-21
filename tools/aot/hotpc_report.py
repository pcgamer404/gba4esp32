#!/usr/bin/env python3
"""Turn a HOTPC dump (hotpc.c over serial) into an annotated coverage report.

Usage: hotpc_report.py <dump.txt> <rom.gba> [--disasm N]

Maps ROM-region blocks (0x08xxxxxx) to file offsets and disassembles them
with capstone. IWRAM blocks (0x03xxxxxx) are listed with counts only -- that
code is copied there at runtime (m4a mixer); finding its ROM source image is
the translator's job, not this report's.
"""
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB


def main():
    dump_path, rom_path = sys.argv[1], sys.argv[2]
    disasm_n = int(sys.argv[sys.argv.index("--disasm") + 1]) if "--disasm" in sys.argv else 6

    rom = open(rom_path, "rb").read()
    blocks = []
    total = 0
    for ln in open(dump_path):
        parts = ln.split()
        if len(parts) == 4 and parts[0] == "HOTPC" and parts[1].startswith("0x"):
            blocks.append((int(parts[1], 16), parts[2], int(parts[3])))
        elif "total=" in ln:
            total = int(ln.split("total=")[1].split()[0])

    if not total:
        total = sum(b[2] for b in blocks)
    print(f"total samples: {total}, blocks reported: {len(blocks)}")

    md_t = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md_a = Cs(CS_ARCH_ARM, CS_MODE_ARM)

    cum = 0.0
    for rank, (addr, mode, cnt) in enumerate(blocks):
        pct = 100.0 * cnt / total
        cum += pct
        region = ("ROM" if addr >> 24 == 0x08 else
                  "IWRAM" if addr >> 24 == 0x03 else
                  "EWRAM" if addr >> 24 == 0x02 else "BIOS/other")
        print(f"\n#{rank:2d} {addr:#010x} {mode} {region:6s} {cnt:8d} "
              f"{pct:5.1f}%  (cum {cum:5.1f}%)")
        if region != "ROM" or rank >= disasm_n:
            continue
        off = addr - 0x08000000
        code = rom[off:off + 64]
        md = md_t if mode == "T" else md_a
        for insn in md.disasm(code, addr):
            print(f"      {insn.address:#010x}  {insn.mnemonic:8s} {insn.op_str}")


if __name__ == "__main__":
    main()
