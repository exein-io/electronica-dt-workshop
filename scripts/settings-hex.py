#!/usr/bin/env python3
"""Write the runtime-lte settings page (token, custom id, platform host) as an Intel hex.

Same page the web flasher writes at 0x80000 (start of the unused MCUboot
secondary slot): "EXEINCFG" + token, custom id and host as NUL-terminated
strings, empty = keep the firmware's value. Flash it next to the app with
nrfutil; the board registers with the token on its next boot (if it has no
identity yet) and then clears it.

  scripts/settings-hex.py --token <bootstrap token> > settings.hex
  nrfutil device program --firmware settings.hex --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE
"""
import argparse
import re
import sys

PAGE = 0x80000


def field(name, value, max_len):
    if len(value) > max_len or not re.fullmatch(r"[\x21-\x7e]*", value):
        sys.exit(f"{name} must be up to {max_len} printable characters without spaces")
    return value.encode() + b"\0"


def ihex(addr, data):
    def rec(kind, offset, payload):
        b = bytes([len(payload), offset >> 8, offset & 0xFF, kind]) + payload
        return ":" + (b + bytes([-sum(b) & 0xFF])).hex().upper()

    lines = [rec(4, 0, (addr >> 16).to_bytes(2, "big"))]
    lines += [rec(0, (addr & 0xFFFF) + i, data[i:i + 16]) for i in range(0, len(data), 16)]
    return "\n".join(lines + [rec(1, 0, b"")]) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--token", default="")
    ap.add_argument("--custom-id", default="")
    ap.add_argument("--host", default="", help="default: the firmware's built-in platform.exein.io")
    a = ap.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9.-]*", a.host):
        sys.exit("host must be a plain hostname or IPv4 address (no scheme or port)")
    page = (b"EXEINCFG" + field("token", a.token, 128) + field("custom id", a.custom_id, 64)
            + field("host", a.host, 64))
    page += b"\xff" * (-len(page) % 4)  # flash writes are 4-byte words
    sys.stdout.write(ihex(PAGE, page))


if __name__ == "__main__":
    main()
