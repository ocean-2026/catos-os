#!/usr/bin/env python3
"""craft_bad_ce.py -- build deliberately malformed CE01 images for the
loader attack (brief section 5, vector 4).

Usage: craft_bad_ce.py <outdir>

Each output file exercises one boundary of the CE loader in
src/task.c:2058-2145:

  BAD1.CE  entry != 0                       -> must be -5
  BAD2.CE  image_size = CE_MAX_IMAGE + 1    -> must be -5
  BAD3.CE  bss_size   = CE_MAX_BSS + 1      -> must be -5
  BAD4.CE  api_offset = 0xFFFFFFFC          (the old `api+4` wraparound)
  BAD5.CE  api_offset = image_size - 4      (largest legal slot)
  BAD6.CE  image_size = 0xFFFFFFFF          (uint32 wrap in img+20)
  BAD7.CE  image_size = 0, api_offset = 0   (degenerate header)
  BAD8.CE  image_size = 0x40000 but the file is only 4 KiB
                                            (chain shorter than img)
  BAD9.CE  bad magic "CE02"
  BAD10.CE header fine, image = 0xCC x 4096 (executes `int3` at entry)

The point is that the loader must reject every one of them with an
error code and stay alive; a ring0 fault here would be a loader bug.
"""
import os
import struct
import sys

CE_MAX_IMAGE = 0x40000
CE_MAX_BSS = 0x40000


def hdr(magic, entry, bss, api, img):
    return magic + struct.pack("<IIII", entry, bss, api, img)


def write(outdir, name, data):
    p = os.path.join(outdir, name)
    with open(p, "wb") as f:
        f.write(data)
    print("%-10s %6d bytes" % (name, len(data)))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    body = bytes(range(256)) * 4          # 1 KiB of deterministic image

    write(out, "BAD1.CE", hdr(b"CE01", 1, 0, 4, len(body)) + body)
    write(out, "BAD2.CE", hdr(b"CE01", 0, 0, 4, CE_MAX_IMAGE + 1) + body)
    write(out, "BAD3.CE", hdr(b"CE01", 0, CE_MAX_BSS + 1, 4, len(body)) + body)
    write(out, "BAD4.CE", hdr(b"CE01", 0, 0, 0xFFFFFFFC, len(body)) + body)
    write(out, "BAD5.CE", hdr(b"CE01", 0, 0, len(body) - 4, len(body)) + body)
    write(out, "BAD6.CE", hdr(b"CE01", 0, 0, 4, 0xFFFFFFFF) + body)
    write(out, "BAD7.CE", hdr(b"CE01", 0, 0, 0, 0) + b"")
    write(out, "BAD8.CE", hdr(b"CE01", 0, 0, 4, CE_MAX_IMAGE) + body[:4096])
    write(out, "BAD9.CE", hdr(b"CE02", 0, 0, 4, len(body)) + body)
    write(out, "BADA.CE", hdr(b"CE01", 0, 0, 4, 4096) + b"\xcc" * 4096)
    return 0


if __name__ == "__main__":
    sys.exit(main())
