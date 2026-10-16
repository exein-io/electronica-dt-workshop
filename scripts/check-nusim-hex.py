#!/usr/bin/env python3
"""Fail if an Intel hex writes the nuSIM / TF-M ITS region 0xF8000-0xFFFFF.

Flashing such an image destroys the nuSIM credentials (see README).
Usage: scripts/check-nusim-hex.py build/merged.hex [...]
"""
import sys

LO, HI = 0xF8000, 0x100000


def bad_records(path):
    base = bad = 0
    for line in open(path):
        raw = line.strip()[1:]
        if not raw:
            continue
        rt = int(raw[6:8], 16)
        if rt == 4:                       # extended linear address
            base = int(raw[8:12], 16) << 16
        elif rt == 2:                     # extended segment address
            base = int(raw[8:12], 16) << 4
        elif rt == 0:                     # data
            a = base + int(raw[2:6], 16)
            bad += a < HI and a + int(raw[0:2], 16) > LO
    return bad


if __name__ == "__main__":
    failed = False
    for path in sys.argv[1:]:
        n = bad_records(path)
        print(f"{path}: " + (f"DANGER, {n} record(s) in nuSIM region" if n else "SAFE"))
        failed |= n > 0
    sys.exit(failed)
