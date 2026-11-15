#!/usr/bin/env python3
"""gen_bad_pe.py -- adversarial PE/MZ fixtures for the v2.43 functional review.

Each fixture exercises one rejection boundary in spawn_load_pe() /
pe_to_ce_file() (kernel_v25/src/task.c).  All must be refused with a
negative error code and must not crash the guest:

  BADMZ.EXE   "MZ" + zeros, e_lfanew = 0           -> not an MZ/PE image
  BADA_LF.EXE "MZ", e_lfanew = 0xFFFFFFF0          -> e_lfanew out of file
  TRUNC_PE.EXE valid PE/COFF/opt headers, section table beyond EOF
  OVERSEC.EXE  section claims raw data past end of file

Usage: python3 gen_bad_pe.py OUT_DIR
"""
import os
import struct
import sys


def dos_header(lfanew):
    b = bytearray(64)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, lfanew)
    return b


def coff(machine=0x14C, nsec=1, opt_hdr=0xE0, chars=0x010F):
    return struct.pack("<HHIIIHH", machine, nsec, 0, 0, 0, opt_hdr, chars)


def opt_header(entry_rva=0, image_base=0x2000000, size_image=0x1000):
    b = bytearray(96)
    struct.pack_into("<H", b, 0, 0x10B)          # PE32 optional magic
    struct.pack_into("<I", b, 16, entry_rva)
    struct.pack_into("<I", b, 28, image_base)
    struct.pack_into("<I", b, 56, size_image)
    return bytes(b)


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)

    # 1. MZ but e_lfanew == 0
    open(os.path.join(out_dir, "BADMZ.EXE"), "wb").write(bytes(dos_header(0)))

    # 2. e_lfanew points far beyond the file
    open(os.path.join(out_dir, "BADA_LF.EXE"), "wb").write(
        bytes(dos_header(0xFFFFFFF0)))

    # 3. PE/COFF/opt present, but the section table is past EOF.
    #    DOS(64) + PE sig(4) + COFF(20) + opt(96) = 0xB8; section table
    #    would live at opt_off + SizeOfOptionalHeader = 0x138.
    trunc = bytearray(dos_header(0x40))
    trunc += b"PE\0\0"
    trunc += coff(nsec=3)
    trunc += opt_header()
    trunc = trunc[:0xC0]                         # file ends before sect table
    open(os.path.join(out_dir, "TRUNC_PE.EXE"), "wb").write(bytes(trunc))

    # 4. Section table present, but raw data range is outside the file.
    over = bytearray(dos_header(0x40))
    over += b"PE\0\0"
    over += coff(nsec=1)
    over += opt_header()
    sec = bytearray(40)
    sec[0:6] = b".text\0"
    struct.pack_into("<I", sec, 8, 0x100)        # VirtualSize
    struct.pack_into("<I", sec, 12, 0)           # VirtualAddress
    struct.pack_into("<I", sec, 16, 0x1000)      # SizeOfRawData
    struct.pack_into("<I", sec, 20, 0x3000)      # PointerToRawData
    over += bytes(sec)
    open(os.path.join(out_dir, "OVERSEC.EXE"), "wb").write(bytes(over))

    for n in ("BADMZ.EXE", "BADA_LF.EXE", "TRUNC_PE.EXE", "OVERSEC.EXE"):
        p = os.path.join(out_dir, n)
        print("OK %-14s %d bytes" % (n, os.path.getsize(p)))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
