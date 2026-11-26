#!/usr/bin/env python3
"""mkrel8.py - append one synthetic R_386_RELATIVE rela to an existing CSO2.

The CSO2 writer (mkso.py) only emits relocs it can read out of the host ELF's
.rel.dyn/.rel.plt.  A RELATIVE reloc has no dynamic symbol at all, so the host
linker drops it from those tables and the packaged image keeps whatever value
was baked in at link time.  rel8.c exists exactly to prove the loader really
applies RELATIVE entries, so we hand-inject one after packaging.

Usage:
    mkrel8.py <in.so> <out.so> 0x<slot_offset>

`slot_offset` is the link-time VA of rel8_slot inside the library image; the
RELATIVE entry rewrites that slot to base + addend at load time.

CSO2 layout (mirrors include/dl.h and mkso.py):
  0   64   header: magic + 15 u32 fields
  ...      export table  8 * export_cnt
  ...      name blob
  ...      rela table   12 * rela_cnt
  ...      raw image     img bytes
"""
import struct
import sys

MAGIC = b"CSO2"
HEADER_SIZE = 64
RELATIVE = 8
MAX_RELA = 128


def die(msg):
    sys.stderr.write("mkrel8: %s\n" % msg)
    sys.exit(1)


def main():
    if len(sys.argv) != 4:
        die("usage: mkrel8.py <in.so> <out.so> 0x<slot_off>")
    src, dst = sys.argv[1], sys.argv[2]
    try:
        slot = int(sys.argv[3], 16)
    except ValueError:
        die("bad slot offset %r" % sys.argv[3])

    with open(src, "rb") as f:
        data = f.read()
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        die("%s is not a CSO2 container" % src)

    f = struct.unpack_from("<15I", data, 4)
    (_version, _base, img_size, _bss, _api, _export_off, _export_cnt,
     rela_off, rela_cnt, _text_end, _data_end, _ia_off, _ia_cnt,
     _fa_off, _fa_cnt) = f

    if rela_cnt >= MAX_RELA:
        die("rela table already full (%d)" % rela_cnt)
    if not (0 <= slot < img_size):
        die("slot 0x%x outside image (0..0x%x)" % (slot, img_size))

    # The rela table must stay contiguous at rela_off; the image follows it
    # directly, so inserting here shifts the image by one entry.
    insert_at = rela_off + 12 * rela_cnt
    entry = struct.pack("<IIi", slot, RELATIVE, 0)
    out = data[:insert_at] + entry + data[insert_at:]

    hdr = list(struct.unpack_from("<4s15I", out, 0))
    hdr[9] = rela_cnt + 1          # index 9 == rela_cnt
    packed = struct.pack("<4s15I", *hdr)
    out = packed + out[HEADER_SIZE:]

    with open(dst, "wb") as w:
        w.write(out)
    print("mkrel8: %s -> %s slot=0x%x rela=%d->%d file=%d"
          % (src, dst, slot, rela_cnt, rela_cnt + 1, len(out)))


if __name__ == "__main__":
    main()
