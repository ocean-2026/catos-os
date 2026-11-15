#!/usr/bin/env python3
"""mkpe.py -- PE32 -> CatOS Executable (.ce) translator + sample PE builder.

CatOS has no dynamic linking, so only self-contained freestanding PE32
EXEs are translated.  Import/export directories are ignored on purpose.

Subcommands
------------
  python mkpe.py IN.exe OUT.ce     translate a PE32 EXE into a CE01 flat image
  python mkpe.py --sample OUT.exe  emit the minimal runnable 32-bit PE sample
  python mkpe.py --selfcheck       build the sample in memory, round-trip it
                                    through the translator, verify the CE header

Limits (CE_MAX_IMAGE / CE_MAX_BSS) are read from
kernel_v25/include/ce.h so this translator never drifts from the kernel
loader.  The kernel-side twin of this translator is pe_to_ce_file() in
kernel_v25/src/task.c; keep the two layouts in lockstep.

Supported subset (mirrors spawn_load_pe()):
  * IMAGE_FILE_MACHINE_I386 (0x14c), PE32 optional-header magic 0x10b
  * EXE, not DLL; non-high ImageBase (< 0x80000000)
  * image fits the per-task user arena [USER_IMAGE_BASE, USER_EXT_END)
  * entry point must land at flat image offset 0 (the CE loader always
    starts the image at offset 0)
"""
import os
import re
import struct
import sys

# ---- ABI constants mirrored from the kernel headers ----------------------
VS_TABLE = 0x2080800          # CATOS_VS_TABLE (ce_abi.h)
SYS_WRITE = 0
SYS_EXIT = 13

PE_MACHINE_I386 = 0x14c
PE_OPT_MAGIC_32 = 0x10b
PE_CHAR_DLL = 0x2000
PE_CHAR_WRITE = 0x80000000
PE_HIGH_BASE = 0x80000000
PE_MAX_SECTIONS = 16

USER_IMAGE_BASE = 0x2000000    # paging.h
USER_EXT_END = 0x2800000

CE_HEADER_SIZE = 20
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "kernel_v25"))


def load_limits():
    """Read CE_MAX_IMAGE / CE_MAX_BSS from kernel_v25/include/ce.h."""
    path = os.path.join(ROOT, "include", "ce.h")
    fallback = (0x40000, 0x40000)
    if not os.path.isfile(path):
        return fallback[0], fallback[1]
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    vals = {}
    for key in ("CE_MAX_IMAGE", "CE_MAX_BSS"):
        m = re.search(r"^\s*#define\s+%s\s+(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*"
                      % key, text, re.MULTILINE)
        if m:
            vals[key] = int(m.group(1), 16) if m.group(1).lower().startswith("0x") \
                else int(m.group(1))
    return (vals.get("CE_MAX_IMAGE", fallback[0]),
            vals.get("CE_MAX_BSS", fallback[1]))


class PEError(Exception):
    pass


# ---- minimal PE32 sample --------------------------------------------------

def _sample_code():
    """Position-independent stub: write(MSG), exit(0).

    Mirrors tools/mkelf.py: the message pointer is computed EIP-relatively
    (call/pop) so it survives any base slide; syscalls go through the fixed
    vsyscall table at VS_TABLE.
    """
    MSG_AT = 0x20
    msg = b"PE->CE roundtrip OK\n\x00"
    code = b""
    code += b"\xe8\x00\x00\x00\x00"              # call next
    code += b"\x58"                              # pop eax
    code += b"\x05" + struct.pack("<i", MSG_AT - 5)  # add eax, &msg
    code += b"\x50"                              # push eax
    code += b"\xff\x15" + struct.pack("<I", VS_TABLE + 4 * SYS_WRITE)
    code += b"\x83\xc4\x04"                      # add esp, 4
    code += b"\x6a\x00"                          # push 0
    code += b"\xff\x15" + struct.pack("<I", VS_TABLE + 4 * SYS_EXIT)
    code += b"\xeb\xfe"                          # jmp $ (unreachable)
    assert len(code) <= MSG_AT, len(code)
    code += b"\x90" * (MSG_AT - len(code))
    code += msg
    return code, MSG_AT


def build_sample_pe():
    """Hand-assemble the minimal runnable 32-bit PE32 EXE."""
    code, _msg_at = _sample_code()
    entry_rva = 0x1000
    image_base = USER_IMAGE_BASE
    file_align = 0x200
    sec_align = 0x1000
    raw_off = file_align
    raw_size = 0x200

    dos = bytearray(64)
    struct.pack_into("<H", dos, 0, 0x5A4D)       # e_magic "MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)      # e_lfanew

    coff_off = 0x40
    opt_off = coff_off + 4 + 20
    opt_size = 0xE0
    sect_off = opt_off + opt_size

    coff = struct.pack("<HHIIIHH",
                       PE_MACHINE_I386, 1, 0, 0, 0, opt_size, 0x0102)

    opt = bytearray(opt_size)
    struct.pack_into("<H", opt, 0, PE_OPT_MAGIC_32)
    struct.pack_into("<I", opt, 16, entry_rva)
    struct.pack_into("<I", opt, 28, image_base)
    struct.pack_into("<I", opt, 32, sec_align)
    struct.pack_into("<I", opt, 36, file_align)
    struct.pack_into("<I", opt, 56, 0x2000)      # SizeOfImage
    struct.pack_into("<I", opt, 60, file_align)  # SizeOfHeaders
    struct.pack_into("<I", opt, 68, 3)            # Subsystem: console

    sec = struct.pack("<8sIIIIIIHHI",
                      b".text\x00\x00\x00",
                      0x40, entry_rva, raw_size, raw_off,
                      0, 0, 0, 0, 0x60000020)

    blob = bytearray(max(sect_off + 40, raw_off + raw_size))
    blob[0:64] = dos
    blob[coff_off:coff_off + 4] = b"PE\x00\x00"
    blob[coff_off + 4:coff_off + 24] = coff
    blob[opt_off:opt_off + opt_size] = opt
    blob[sect_off:sect_off + 40] = sec
    blob[raw_off:raw_off + len(code)] = code
    return bytes(blob)


# ---- PE parser ------------------------------------------------------------

def parse_pe(data):
    if len(data) < 64:
        raise PEError("file too small for a DOS header")
    if data[0] != 0x4D or data[1] != 0x5A:
        raise PEError("not an MZ file (bad magic)")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew == 0 or e_lfanew > len(data) - 4:
        raise PEError("PE header offset out of file")
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise PEError("missing PE signature")

    coff_off = e_lfanew + 4
    machine, nsect, _ts, _psym, _nsym, opt_hdr_size, chars = \
        struct.unpack_from("<HHIIIHH", data, coff_off)
    if machine != PE_MACHINE_I386:
        raise PEError("not an i386 PE (machine 0x%x)" % machine)
    if chars & PE_CHAR_DLL:
        raise PEError("DLL images are not supported (need an EXE)")
    if nsect == 0 or nsect > PE_MAX_SECTIONS:
        raise PEError("bad section count %d" % nsect)
    if opt_hdr_size < 96 or opt_hdr_size > 240:
        raise PEError("bad optional header size %d" % opt_hdr_size)

    opt_off = coff_off + 20
    opt_magic = struct.unpack_from("<H", data, opt_off)[0]
    if opt_magic != PE_OPT_MAGIC_32:
        raise PEError("need PE32 (magic 0x10b), got 0x%x" % opt_magic)

    entry_rva = struct.unpack_from("<I", data, opt_off + 16)[0]
    image_base = struct.unpack_from("<I", data, opt_off + 28)[0]
    size_image = struct.unpack_from("<I", data, opt_off + 56)[0]
    if image_base >= PE_HIGH_BASE:
        raise PEError("high ImageBase 0x%x unsupported" % image_base)
    if size_image == 0 or image_base > USER_EXT_END - size_image:
        raise PEError("image does not fit the CatOS user arena")

    sect_off = opt_off + opt_hdr_size
    if sect_off + nsect * 40 > len(data):
        raise PEError("section table out of file")

    sections = []
    for i in range(nsect):
        o = sect_off + i * 40
        _name, vsize, va, raw_size, raw_ptr, _prel, _plinen, _nrel, _nline, \
            schar = struct.unpack_from("<8sIIIIIIHHI", data, o)
        mem = vsize if vsize > raw_size else raw_size
        if raw_size and raw_ptr + raw_size > len(data):
            raise PEError("section raw data out of file")
        sections.append(dict(va=va, vsize=vsize, mem=mem, raw=raw_size,
                             raw_ptr=raw_ptr, writable=bool(schar & PE_CHAR_WRITE)))

    return dict(entry_rva=entry_rva, image_base=image_base,
                size_image=size_image, sections=sections)


# ---- PE -> CE01 translator -------------------------------------------------

def pe_to_ce(data):
    pe = parse_pe(data)
    max_image, max_bss = load_limits()

    sections = sorted(pe["sections"], key=lambda s: s["va"])
    flat = bytearray()
    off = []
    bss = 0
    for s in sections:
        off.append(len(flat))
        if s["raw"]:
            flat += data[s["raw_ptr"]:s["raw_ptr"] + s["raw"]]
        pad = s["mem"] - s["raw"]
        if pad > 0:
            flat += b"\x00" * pad
            bss += pad
        # 4-byte alignment between sections
        if len(flat) % 4:
            flat += b"\x00" * (4 - len(flat) % 4)

    # entry must be at flat offset 0: CE loader starts the image at 0
    entry_off = None
    for s, o in zip(sections, off):
        if pe["entry_rva"] >= s["va"] and pe["entry_rva"] < s["va"] + s["mem"]:
            entry_off = o + (pe["entry_rva"] - s["va"])
            break
    if entry_off is None:
        raise PEError("entry point outside any section")
    if entry_off != 0:
        raise PEError("entry at flat offset 0x%x; CE images must start at 0 "
                      "(rebuild with the entry section first)" % entry_off)

    # reserve the 4-byte API slot at the image end; the kernel fills it with
    # the vsyscall table pointer on a CE load
    api_off = len(flat)
    flat += b"\x00" * 4
    image_size = len(flat)

    if image_size > max_image:
        raise PEError("image too large (%d bytes, max %d)" % (image_size, max_image))
    if bss > max_bss:
        raise PEError("bss too large (0x%x, max 0x%x)" % (bss, max_bss))

    hdr = b"CE01" + struct.pack("<IIII", 0, bss, api_off, image_size)
    return hdr + bytes(flat)


def selfcheck():
    sample = build_sample_pe()
    ce = pe_to_ce(sample)
    if ce[:4] != b"CE01":
        raise PEError("bad CE magic")
    entry, bss, api_off, image_size = struct.unpack_from("<IIII", ce, 4)
    if entry != 0:
        raise PEError("entry offset not 0")
    if api_off + 4 > image_size:
        raise PEError("api slot outside image")
    if image_size > load_limits()[0]:
        raise PEError("image over CE_MAX_IMAGE")
    # round-trip: re-parse the embedded PE from the sample
    parse_pe(sample)
    print("selfcheck OK: sample PE=%d bytes -> CE=%d bytes (image=%d bss=0x%x "
          "api=0x%x)" % (len(sample), len(ce), image_size, bss, api_off))
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "--sample":
        if len(argv) != 3:
            print("usage: mkpe.py --sample OUT.exe")
            return 2
        open(argv[2], "wb").write(build_sample_pe())
        print("sample PE32 -> %s" % argv[2])
        return 0
    if len(argv) >= 2 and argv[1] == "--selfcheck":
        return selfcheck()
    if len(argv) == 3:
        data = open(argv[1], "rb").read()
        ce = pe_to_ce(data)
        open(argv[2], "wb").write(ce)
        _e, bss, api_off, image_size = struct.unpack_from("<IIII", ce, 4)
        print("%s: image=%d bss=0x%x api_off=0x%x"
              % (argv[2], image_size, bss, api_off))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
