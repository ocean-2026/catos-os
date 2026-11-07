#!/usr/bin/env python3
"""mkce.py - wrap a raw flat binary into a CatOS Executable (.ce).

usage: python mkce.py in.bin out.ce bss_size api_offset

bss_size and api_offset are hex (with or without 0x prefix), as printed
by objdump (-h .bss size) and nm (__ce_api address).  The .ce file is a
20-byte header ("CE01", entry=0, bss, api, image_size) followed by the
raw image; the kernel loads it at 32 MiB (CE_LOAD_BASE 0x02000000),
zeroes the bss, writes the system-call table pointer into the api slot
and jumps to offset 0.

The image/bss limits come from include/ce.h -- the kernel-side header is
the single source of truth.  The v2.5 kernel raised CE_MAX_IMAGE from
64 KiB to 256 KiB, but this script kept rejecting images over 64 KiB;
reading the constants from ce.h stops that drift from happening again.
Build-time checks mirror task_spawn_ce() so a broken .ce is caught here
instead of failing much later with err -5 inside QEMU.
"""
import os
import re
import struct
import sys

HEADER_SIZE = 20
UINT32_MAX = 0xFFFFFFFF
FALLBACK_MAX_IMAGE = 0x40000   # 256 KiB -- keep in sync with include/ce.h
FALLBACK_MAX_BSS = 0x40000     # 256 KiB -- keep in sync with include/ce.h


def die(msg):
    print("error: " + msg)
    sys.exit(1)


def c_int(s):
    """Parse a C integer literal (hex with 0x, or decimal), no silent octal."""
    s = s.strip()
    return int(s, 16) if s.lower().startswith("0x") else int(s, 10)


def load_limits():
    """Return (max_image, max_bss, source) read from the kernel ce.h."""
    here = os.path.dirname(os.path.abspath(__file__))
    # build/mkce.py -> ../include/ce.h ; extapp/build/mkce.py -> ../../include/ce.h
    for rel in (os.path.join("..", "include", "ce.h"),
                os.path.join("..", "..", "include", "ce.h")):
        path = os.path.normpath(os.path.join(here, rel))
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        vals = {}
        for key in ("CE_MAX_IMAGE", "CE_MAX_BSS"):
            m = re.search(r"^\s*#define\s+%s\s+(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*"
                          % key, text, re.MULTILINE)
            if m:
                try:
                    vals[key] = c_int(m.group(1))
                except ValueError:
                    pass
        if len(vals) == 2:
            return vals["CE_MAX_IMAGE"], vals["CE_MAX_BSS"], path
        # A ce.h we can read but cannot parse is a build error, not a reason
        # to silently fall back to a possibly stale built-in constant.
        die("could not parse CE_MAX_IMAGE / CE_MAX_BSS from %s" % path)
    return FALLBACK_MAX_IMAGE, FALLBACK_MAX_BSS, "built-in fallback"


def to_int(s: str, what: str) -> int:
    # objdump/nm print hex without the 0x prefix; int() accepts both forms.
    try:
        v = int(s.strip(), 16)
    except ValueError:
        die("%s is not a hex number: %r (expected objdump/nm output)" % (what, s))
    if v < 0 or v > UINT32_MAX:
        die("%s out of uint32 range: %r" % (what, s))
    return v


def main() -> None:
    if len(sys.argv) != 5:
        print("usage: mkce.py in.bin out.ce bss_size api_offset")
        sys.exit(1)
    max_image, max_bss, source = load_limits()
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    bss = to_int(sys.argv[3], "bss_size")
    api = to_int(sys.argv[4], "api_offset")
    img = len(data)

    if img > max_image:
        die("image too big (%d bytes, max %d / 0x%x from %s)"
            % (img, max_image, max_image, source))
    if bss > max_bss:
        die("bss too big (0x%x, max 0x%x from %s) - kernel would reject this CE"
            % (bss, max_bss, source))
    # Mirrors task_spawn_ce(): api + 4 > img.  The kernel does this in
    # uint32_t, so api >= 0xFFFFFFFC wraps to a small number and slips
    # through there (see docs/CatOS-内核问题清单.md, 第十节 遗留发现).
    # Rejecting the wrapped case here is intentional: stricter than the
    # loader is safe, letting a corrupt CE through is not.
    if api > UINT32_MAX - 4 or api + 4 > img:
        die("api_offset 0x%x + 4 > image size %d - kernel would reject this CE"
            % (api, img))

    hdr = b"CE01" + struct.pack("<IIII", 0, bss, api, img)
    assert len(hdr) == HEADER_SIZE
    with open(sys.argv[2], "wb") as f:
        f.write(hdr + data)
    print("%s: image=%d bss=0x%x api_off=0x%x"
          % (sys.argv[2], img, bss, api))


if __name__ == "__main__":
    main()
