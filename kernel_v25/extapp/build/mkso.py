#!/usr/bin/env python3
"""mkso.py - wrap a raw flat binary into a CatOS Shared Object (CSO1).

usage:
  python mkso.py in.bin out.so base bss_size api_offset specs

  base        load VMA (hex, page-aligned, inside USER_LIB_BASE..USER_LIB_END)
  bss_size    zeroed bytes following the image (hex, 0 if none)
  api_offset  offset of the 4-byte API pointer slot, or "none"
  specs       "expname=VMA,..." -- exported name -> linked VMA (hex)

CSO1 file layout (all integers little-endian), mirrors include/dl.h:

  offset  size   field
  0       4      magic "CSO1"
  4       4      version (1)
  8       4      load base VA
  12      4      image_size (code+data bytes, > 0)
  16      4      bss_size (zeroed bytes following the image)
  20      4      api_offset (0xFFFFFFFF = none)
  24      4      export_off (file offset of the export table)
  28      4      export_cnt
  ...     8*cnt  export entries: { name_off, func_off }
  ...     N      name blob (NUL-terminated export names)
  ...     image_size  raw image bytes

The kernel reads the header/export table/name blob into a kernel metadata
frame at dlopen(); only the raw image is copied into the task address space.
Limits mirror dl.h so a broken .so is caught here instead of failing inside
the loader.
"""
import os
import struct
import sys

MAGIC = b"CSO2"
VERSION = 2
HEADER_SIZE = 64
NO_API = 0xFFFFFFFF

# Mirrors CSO_MAX_* in include/dl.h.
MAX_IMAGE = 0x10000
MAX_BSS = 0x10000
MAX_EXPORTS = 128
MAX_NAMES = 2048


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def c_int(s, what):
    s = s.strip()
    try:
        v = int(s, 16) if s.lower().startswith("0x") else int(s, 10)
    except ValueError:
        die("%s is not a number: %r" % (what, s))
    if v < 0 or v > 0xFFFFFFFF:
        die("%s out of uint32 range: %r" % (what, s))
    return v


def main():
    if len(sys.argv) != 7:
        print("usage: mkso.py in.bin out.so base bss_size api_offset specs")
        sys.exit(1)

    in_path, out_path = sys.argv[1], sys.argv[2]
    base = c_int(sys.argv[3], "base")
    bss = c_int(sys.argv[4], "bss_size")
    api_arg = sys.argv[5]
    specs_arg = sys.argv[6]

    if base & 0xFFF:
        die("base 0x%x is not page-aligned" % base)
    if bss > MAX_BSS:
        die("bss too big (0x%x, max 0x%x)" % (bss, MAX_BSS))

    with open(in_path, "rb") as f:
        image = f.read()
    img = len(image)
    if img == 0 or img > MAX_IMAGE:
        die("image size %d out of range (1..0x%x)" % (img, MAX_IMAGE))

    if api_arg == "none":
        api = NO_API
    else:
        api = c_int(api_arg, "api_offset")
        if api + 4 > img:
            die("api_offset 0x%x + 4 > image size %d" % (api, img))

    # Parse export specs: expname=VMA
    exports = []  # (name, func_off)
    names = []    # ordered unique names
    if specs_arg:
        for pair in specs_arg.split(","):
            pair = pair.strip()
            if not pair:
                continue
            if "=" not in pair:
                die("bad export spec %r (want name=VMA)" % pair)
            expname, sym = pair.split("=", 1)
            expname, sym = expname.strip(), sym.strip()
            vma = c_int(sym, "export VMA")
            func_off = vma - base
            if func_off < 0 or func_off >= img:
                die("export %s VMA 0x%x -> off 0x%x outside image 0..0x%x"
                    % (expname, vma, func_off, img))
            exports.append((expname, func_off))
            names.append(expname)

    if len(exports) > MAX_EXPORTS:
        die("too many exports (%d, max %d)" % (len(exports), MAX_EXPORTS))

    # Build the name blob; each name_off is relative to the blob start.
    blob = b""
    name_off = {}
    for nm in names:
        if nm not in name_off:
            name_off[nm] = len(blob)
            blob += nm.encode("ascii") + b"\x00"
    if len(blob) > MAX_NAMES:
        die("name blob too big (%d, max %d)" % (len(blob), MAX_NAMES))

    export_off = HEADER_SIZE
    table = b"".join(
        struct.pack("<II", name_off[nm], off) for nm, off in exports)
    name_off_field = export_off + len(table)

    # CSO2 v2 header: v1 fields keep offsets 0..31; the trailing eight v2
    # fields default to "no rela / no arrays / whole image RW" until the
    # 2C-2 relocator and 2C-3 RO text split populate them.
    header = MAGIC + struct.pack(
        "<IIIIIIIIIIIIIII",
        VERSION, base, img, bss, api, export_off, len(exports),
        0,            # rela_off
        0,            # rela_cnt
        0,            # text_end (RO span end; 0 = no RO split yet)
        img,          # data_end (.data end = whole image for v1-style lib)
        0,            # init_array_off
        0,            # init_array_cnt
        0,            # fini_array_off
        0,            # fini_array_cnt
    )
    assert len(header) == HEADER_SIZE

    out = header + table + blob + image
    with open(out_path, "wb") as f:
        f.write(out)
    print("%s: base=0x%x image=%d bss=0x%x api=%s exports=%d blob=%d file=%d"
          % (out_path, base, img, bss,
             "none" if api == NO_API else "0x%x" % api,
             len(exports), len(blob), len(out)))


if __name__ == "__main__":
    main()
