#!/usr/bin/env python3
"""mkelf.py -- build a minimal, real ELF32 i386 executable for CatOS.

The host toolchain is MinGW-only (no elf_i386 emitter), so the ELF test
binary is hand-assembled here.  The program is tiny on purpose:

    push  &msg                 ; cdecl arg for slot 0 (write)
    call  [0x2080800 + 4*0]    ; SYS_WRITE
    add   esp, 4
    push  0                    ; exit code
    call  [0x2080800 + 4*13]   ; SYS_EXIT
    jmp   $

0x2080800 is CATOS_VS_TABLE (the vsyscall API table), fixed by the ABI.

The image is ET_DYN (PIE) with one RW PT_LOAD covering code+data; the
kernel slides it by 0..15 pages above USER_EXT_BASE (ASLR).

usage: python tools/mkelf.py extapp/bin/hello.elf
"""
import struct, sys

VS_TABLE   = 0x2080800
SYS_WRITE  = 0
SYS_EXIT   = 13
SEG_OFF    = 0x1000          # file offset of the PT_LOAD data
ALIGN      = 0x1000

def enc_calls():
    code = b""
    # push msg (vaddr 0x30 inside the segment, i.e. base-relative 0x30)
    code += struct.pack("<BI", 0x68, 0x30)
    # call dword [VS_TABLE + 4*SYS_WRITE]
    code += b"\xff\x15" + struct.pack("<I", VS_TABLE + 4 * SYS_WRITE)
    code += b"\x83\xc4\x04"                      # add esp,4
    code += b"\x6a\x00"                          # push 0
    # call dword [VS_TABLE + 4*SYS_EXIT]
    code += b"\xff\x15" + struct.pack("<I", VS_TABLE + 4 * SYS_EXIT)
    code += b"\xeb\xfe"                          # jmp $ (unreachable)
    msg = b"hello from ELF\n\x00"
    assert len(code) <= 0x30
    code += b"\x90" * (0x30 - len(code)) + msg
    return code

def main(out):
    data = enc_calls()
    ehsize, phentsize, phnum = 52, 32, 1
    phoff = ehsize

    seg = bytearray(data)
    seg_len = len(seg)

    # ELF header
    eh  = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8   # ident: 32-bit LE, SysV
    eh += struct.pack("<HHIIIIIHHHHHH",
                      3,            # e_type   = ET_DYN (PIE)
                      3,            # e_machine= EM_386
                      1,            # e_version
                      SEG_OFF,      # e_entry  = segment-relative code start
                      phoff,
                      0,            # e_shoff  (none)
                      0,            # e_flags
                      ehsize, phentsize, phnum,
                      0, 0, 0)      # shentsize/shnum/shstrndx
    assert len(eh) == ehsize, len(eh)

    # program header: one RW PT_LOAD, file bytes == memory bytes
    ph  = struct.pack("<IIIIIIII",
                      1,            # p_type = PT_LOAD
                      SEG_OFF,      # p_offset
                      0,            # p_vaddr (PIE: slid by kernel)
                      0,            # p_paddr
                      seg_len,      # p_filesz
                      seg_len,      # p_memsz
                      6,            # p_flags = R|W
                      ALIGN)        # p_align
    assert len(ph) == phentsize

    img = bytearray(eh + ph)
    img += b"\x00" * (SEG_OFF - len(img))
    img += seg
    with open(out, "wb") as f:
        f.write(img)
    print("wrote %s (%d bytes: 1 PT_LOAD, entry=0x%x, %d code bytes)"
          % (out, len(img), SEG_OFF, len(data)))

    # self-check: reparse like the kernel loader would
    assert img[:4] == b"\x7fELF" and img[4] == 1 and img[5] == 1
    e = struct.unpack_from("<HHIIIIIHHHHHH", img, 16)
    assert e[0] == 3 and e[1] == 3
    p = struct.unpack_from("<IIIIIIII", img, e[4])
    assert p[0] == 1 and p[5] >= p[4] and p[7] == ALIGN
    assert (p[1] % ALIGN) == (p[2] % ALIGN)      # congruence for lazy mapping
    print("self-check OK")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "hello.elf")
