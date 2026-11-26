#!/usr/bin/env python3
"""mkpe_reloc.py -- builder for the PE base-relocation probe images.

P1-1 (D9): the CatOS PE loader used to ignore the Base Relocation directory
(DataDirectory[5]), so an image whose preferred ImageBase did not match the
actual load address kept its absolute pointers at the link-time base and
faulted on first use.

This script emits two hand-assembled PE32 i386 EXEs:

  --sample   PE-RELOC PROBE OK program.  Preferred ImageBase is 0x400000
             (outside the CatOS user arena), so the kernel slides it to the
             fallback image slot.  A .data slot holds an absolute pointer to
             the message, fixed up by one IMAGE_REL_BASED_HIGHLOW(3) entry in
             a .reloc block.  When the relocation is applied correctly the
             program prints the marker line; without it the dereference
             targets the unmapped preferred base.

  --badreloc a malformed twin: the relocation block claims more entries than
             the file/image holds.  The loader must return a negative error
             (never a ring0 fault).

Usage:
  python mkpe_reloc.py --sample OUT.exe
  python mkpe_reloc.py --badreloc OUT.exe
"""
import struct
import sys

PE_MACHINE_I386 = 0x14c
PE_OPT_MAGIC_32 = 0x10b
PE_CHAR_DLL = 0x2000
PE_CHAR_CODE = 0x60000020
PE_CHAR_INIT_RW = 0xC0000040
PE_CHAR_INIT_R = 0x40000040

VS_TABLE = 0x2080800
SYS_WRITE = 0
SYS_EXIT = 13

# Preferred (link-time) base: deliberately below USER_IMAGE_BASE (0x2000000)
# so the loader has to slide the image and apply a non-zero relocation delta.
PREF_BASE = 0x400000

SEC_ALIGN = 0x1000
FILE_ALIGN = 0x200

TEXT_RVA = 0x1000
DATA_RVA = 0x2000
RELOC_RVA = 0x3000
MSG_RVA = DATA_RVA + 0x20
SLOT_RVA = DATA_RVA

MARKER = b"PE-RELOC PROBE OK\n\x00"


def _code():
    """PIC stub: read the relocated pointer out of the .data slot, write the
    message, exit.  The slot address is reached EIP-relative (call/pop), so
    the code itself needs no fixup; only the pointer slot in .data does.
    """
    slot_off = SLOT_RVA - (TEXT_RVA + 5)   # pop-eip lands at entry+5
    code = b""
    code += b"\xe8\x00\x00\x00\x00"              # call next
    code += b"\x58"                              # pop eax
    code += b"\x05" + struct.pack("<i", slot_off)  # add eax, &slot
    code += b"\x8b\x00"                          # mov eax, [eax]
    code += b"\x50"                              # push buf
    code += b"\xff\x15" + struct.pack("<I", VS_TABLE + 4 * SYS_WRITE)
    code += b"\x83\xc4\x04"                      # add esp, 4
    code += b"\x6a\x00"                          # push 0
    code += b"\xff\x15" + struct.pack("<I", VS_TABLE + 4 * SYS_EXIT)
    code += b"\xeb\xfe"                          # jmp $
    return code


def _reloc_block(bad=False):
    """One .reloc block covering the .data page.

    Entries: type HIGHLOW(3), offset 0x000 -> the pointer slot.  `bad`
    inflates BlockSize past the directory's declared size (the loader must
    reject the overrun with a negative code instead of reading past the
    image).
    """
    entry = (3 << 12) | 0x000
    body = struct.pack("<HH", entry, 0)          # second entry: ABSOLUTE(0) pad
    good_size = 8 + len(body)
    if bad:
        # Claim 64 entries while the directory advertises only one: overrun.
        block_size = 8 + 2 * 64
    else:
        block_size = good_size
    blk = struct.pack("<II", DATA_RVA, block_size) + body
    if bad:
        blk += b"\x00" * (block_size - len(blk))
    return blk, (good_size if bad else block_size)


def build_pe(bad=False):
    code = _code()
    msg = MARKER
    # pointer slot holds the preferred-base absolute VA (link-time value);
    # the reloc entry adds the load delta at runtime.
    slot_value = PREF_BASE + MSG_RVA
    data = struct.pack("<I", slot_value)
    data += b"\x00" * (0x20 - len(data))
    data += msg

    reloc, reloc_dir_size = _reloc_block(bad=bad)

    dos = bytearray(64)
    struct.pack_into("<H", dos, 0, 0x5A4D)
    struct.pack_into("<I", dos, 0x3C, 0x40)

    coff_off = 0x40
    opt_off = coff_off + 4 + 20
    opt_size = 0xE0
    sect_off = opt_off + opt_size

    coff = struct.pack("<HHIIIHH", PE_MACHINE_I386, 3, 0, 0, 0, opt_size, 0x0102)

    opt = bytearray(opt_size)
    struct.pack_into("<H", opt, 0, PE_OPT_MAGIC_32)
    struct.pack_into("<I", opt, 16, TEXT_RVA)          # AddressOfEntryPoint
    struct.pack_into("<I", opt, 28, PREF_BASE)         # ImageBase
    struct.pack_into("<I", opt, 32, SEC_ALIGN)
    struct.pack_into("<I", opt, 36, FILE_ALIGN)
    struct.pack_into("<I", opt, 56, 0x4000)            # SizeOfImage
    struct.pack_into("<I", opt, 60, FILE_ALIGN)       # SizeOfHeaders
    struct.pack_into("<I", opt, 68, 3)                 # Subsystem: console
    struct.pack_into("<I", opt, 92, 16)                # NumberOfRvaAndSizes
    # DataDirectory[5] = Base Relocation Table
    struct.pack_into("<II", opt, 96 + 5 * 8, RELOC_RVA, reloc_dir_size)

    def sec(name, vsize, va, raw_size, raw_ptr, chars):
        return struct.pack("<8sIIIIIIHHI", name, vsize, va, raw_size, raw_ptr,
                           0, 0, 0, 0, chars)

    text_raw = FILE_ALIGN
    data_raw = text_raw + FILE_ALIGN
    reloc_raw = data_raw + FILE_ALIGN
    sections = b"".join([
        sec(b".text\x00\x00\x00", len(code), TEXT_RVA, FILE_ALIGN, text_raw,
            PE_CHAR_CODE),
        sec(b".data\x00\x00\x00", len(data), DATA_RVA, FILE_ALIGN, data_raw,
            PE_CHAR_INIT_RW),
        sec(b".reloc\x00\x00", len(reloc), RELOC_RVA, FILE_ALIGN, reloc_raw,
            PE_CHAR_INIT_R),
    ])

    total = reloc_raw + FILE_ALIGN
    blob = bytearray(total)
    blob[0:64] = dos
    blob[coff_off:coff_off + 4] = b"PE\x00\x00"
    blob[coff_off + 4:coff_off + 24] = coff
    blob[opt_off:opt_off + opt_size] = opt
    blob[sect_off:sect_off + len(sections)] = sections
    blob[text_raw:text_raw + len(code)] = code
    blob[data_raw:data_raw + len(data)] = data
    blob[reloc_raw:reloc_raw + len(reloc)] = reloc
    return bytes(blob)


def main(argv):
    if len(argv) == 3 and argv[1] == "--sample":
        open(argv[2], "wb").write(build_pe(bad=False))
        print("reloc probe PE -> %s" % argv[2])
        return 0
    if len(argv) == 3 and argv[1] == "--badreloc":
        open(argv[2], "wb").write(build_pe(bad=True))
        print("malformed-reloc PE -> %s" % argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
