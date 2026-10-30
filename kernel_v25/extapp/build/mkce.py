#!/usr/bin/env python3
"""mkce.py - wrap a raw flat binary into a CatOS Executable (.ce).

usage: python mkce.py in.bin out.ce bss_size api_offset

bss_size and api_offset are hex (with or without 0x prefix), as printed
by objdump (-h .bss size) and nm (__ce_api address).  The .ce file is a
20-byte header ("CE01", entry=0, bss, api, image_size) followed by the
raw image; the kernel loads it at 16 MiB, zeroes the bss, writes the
system-call table pointer into the api slot and jumps to offset 0.
"""
import struct
import sys


def to_int(s: str) -> int:
    s = s.strip().lower()
    if s.startswith("0x"):
        return int(s, 16)
    return int(s, 16)  # objdump/nm print hex without the prefix


def main() -> None:
    if len(sys.argv) != 5:
        print("usage: mkce.py in.bin out.ce bss_size api_offset")
        sys.exit(1)
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    bss = to_int(sys.argv[3])
    api = to_int(sys.argv[4])
    if len(data) > 0x10000:
        print("error: image too big (%d bytes, max 65536)" % len(data))
        sys.exit(1)
    hdr = b"CE01" + struct.pack("<IIII", 0, bss, api, len(data))
    with open(sys.argv[2], "wb") as f:
        f.write(hdr + data)
    print("%s: image=%d bss=0x%x api_off=0x%x"
          % (sys.argv[2], len(data), bss, api))


if __name__ == "__main__":
    main()
