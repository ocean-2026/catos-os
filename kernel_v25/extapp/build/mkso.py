#!/usr/bin/env python3
"""mkso.py - wrap a PIC -shared ELF into a CatOS CSO2 shared object.

usage:
  python mkso.py in.elf in.bin out.so base bss_size api_offset specs

  in.elf      linked -shared ELF (readelf -r / -D parsed for relocs/symbols)
  in.bin      objcopy -O binary flat image (file offset == link vaddr, base 0)
  base        load VMA (hex, page-aligned, inside USER_LIB_BASE..USER_LIB_END)
  bss_size    zeroed bytes following the image (hex, 0 if none)
  api_offset  offset of the 4-byte API pointer slot, or "none"
  specs       "expname=offset,..." -- exported name -> link-time offset (hex)

CSO2 file layout (all integers little-endian), mirrors include/dl.h:

  offset  size   field
  0       64     header (magic CSO2, v2 fields incl. rela_off/rela_cnt)
  ...     8*cnt  export entries: { name_off, sym_off }
  ...     N      name blob (NUL-terminated export names)
  ...     12*cnt rela table: { r_offset, r_info, r_addend }
  ...     image_size  raw flat image bytes

Reloc extraction (host has no pyelftools; text parsing via readelf):
  `readelf -r in.elf` lists .rel.dyn/.rel.plt entries.  Type names are mapped
  to the R_386 codes below.  A reloc's symbol name is matched against the
  export specs; its index becomes the rela sym_idx (high 16 bits of r_info).
  R_386_32(1) and R_386_RELATIVE(8) bake the addend from the pre-relocation
  slot value in the flat image; other types use addend 0.  A reloc targeting
  a symbol absent from the export table is a build error (fail fast).

  Fallback: if a dynamic symbol name carries a `@...` version suffix or the
  .dynsym entry looks truncated, `nm -D` is cross-checked by build_so.sh;
  here names are compared after stripping everything from the first `@`.
"""
import os
import struct
import subprocess
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
MAX_RELA = 128

TYPE_CODE = {
    "R_386_32": 1,
    "R_386_PC32": 2,
    "R_386_GLOB_DAT": 6,
    "R_386_JMP_SLOT": 7,
    "R_386_JUMP_SLOT": 7,
    "R_386_RELATIVE": 8,
}
# R_386_RELATIVE(8) carries its addend (pre-relocation slot value); the
# other symbolic fixups resolve to sym_val directly (addend 0).
REL_ADDEND_FROM_SLOT = (8,)


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


def parse_relocs(elf_path, img):
    """Return list of (offset, type_code, sym_value, sym_name).  Fail on
    unsupported reloc types."""
    try:
        out = subprocess.check_output(["readelf", "-r", elf_path],
                                      stderr=subprocess.STDOUT, text=True)
    except (OSError, subprocess.CalledProcessError) as e:
        die("readelf -r failed on %s: %s" % (elf_path, e))
    relocs = []
    for line in out.splitlines():
        toks = line.split()
        if len(toks) < 3 or not toks[0].startswith("0000"):
            continue
        try:
            offset = int(toks[0], 16)
            int(toks[1], 16)  # info, decoded from the type name instead
        except ValueError:
            continue
        tname = toks[2]
        if tname not in TYPE_CODE:
            die("unsupported reloc type %s at offset 0x%x" % (tname, offset))
        symval = 0
        symname = ""
        if len(toks) >= 5:
            try:
                symval = int(toks[3], 16)
            except ValueError:
                symval = 0
            symname = toks[4].split("@", 1)[0]
        if offset >= len(img):
            die("reloc offset 0x%x outside image %d" % (offset, len(img)))
        relocs.append((offset, TYPE_CODE[tname], symval, symname))
    return relocs


def parse_symtab(elf_path):
    """Full `.symtab` name -> link-time offset (definition address).  The
    dynamic table can report st_value 0 for preemptible functions, so the
    regular symtab is the source of truth for export offsets."""
    try:
        txt = subprocess.check_output(["nm", elf_path],
                                      stderr=subprocess.STDOUT, text=True)
    except (OSError, subprocess.CalledProcessError) as e:
        die("nm failed on %s: %s" % (elf_path, e))
    sym = {}
    for line in txt.splitlines():
        t = line.split()
        if len(t) < 3:
            continue
        try:
            sym[t[2]] = int(t[0], 16)
        except ValueError:
            continue
    return sym


def main():
    if len(sys.argv) != 8:
        print("usage: mkso.py in.elf in.bin out.so base bss_size api_offset specs")
        sys.exit(1)

    elf_path, bin_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    base = c_int(sys.argv[4], "base")
    bss = c_int(sys.argv[5], "bss_size")
    api_arg = sys.argv[6]
    specs_arg = sys.argv[7]

    if base & 0xFFF:
        die("base 0x%x is not page-aligned" % base)
    if bss > MAX_BSS:
        die("bss too big (0x%x, max 0x%x)" % (bss, MAX_BSS))

    with open(bin_path, "rb") as f:
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

    symtab = parse_symtab(elf_path)

    # Parse export specs: expname=internal_symbol (ELF linked at vaddr 0).
    exports = []  # (dlsym_name, internal_name, sym_off)
    names = []
    if specs_arg:
        for pair in specs_arg.split(","):
            pair = pair.strip()
            if not pair:
                continue
            if "=" not in pair:
                die("bad export spec %r (want expname=internal)" % pair)
            expname, internal = pair.split("=", 1)
            expname, internal = expname.strip(), internal.strip()
            if internal not in symtab:
                die("export symbol %r not in symtab" % internal)
            off = symtab[internal]
            if off >= img:
                die("export %s offset 0x%x outside image 0..0x%x"
                    % (expname, off, img))
            exports.append((expname, internal, off))
            names.append(expname)

    if len(exports) > MAX_EXPORTS:
        die("too many exports (%d, max %d)" % (len(exports), MAX_EXPORTS))

    # The __ce_api slot is referenced by a GLOB_DAT reloc but must not be a
    # dlsym-visible export.  Append a hidden internal entry for it so the
    # reloc can resolve to the api offset.
    internal_to_idx = {internal: i for i, (_e, internal, _o) in enumerate(exports)}
    if api != NO_API:
        exports.append(("", "__ce_api", api))
        internal_to_idx["__ce_api"] = len(exports) - 1

    # Extract relocs from the ELF; resolve symbol -> export index by name.
    relocs = parse_relocs(elf_path, image)
    if len(relocs) > MAX_RELA:
        die("too many relocs (%d, max %d)" % (len(relocs), MAX_RELA))

    rela = []
    for offset, code, symval, symname in relocs:
        sym_idx = 0
        if symname:
            if symname not in internal_to_idx:
                die("reloc targets non-exported symbol %r (value 0x%x)"
                    % (symname, symval))
            sym_idx = internal_to_idx[symname] + 1   # 1-based; 0 = null
        addend = 0
        if code in REL_ADDEND_FROM_SLOT:
            addend = struct.unpack_from("<i", image, offset)[0]
        rela.append((offset, code, sym_idx, addend))

    # Build the name blob; each name_off is relative to the blob start.
    # Hidden internal entries get a standalone NUL name.
    blob = b""
    idx_name_off = []
    for expname, _internal, _off in exports:
        idx_name_off.append(len(blob))
        blob += (expname.encode("ascii") if expname else b"") + b"\x00"
    if len(blob) > MAX_NAMES:
        die("name blob too big (%d, max %d)" % (len(blob), MAX_NAMES))

    export_off = HEADER_SIZE
    table = b"".join(
        struct.pack("<II", idx_name_off[i], off)
        for i, (_e, _internal, off) in enumerate(exports))
    blob_off = export_off + len(table)
    rela_off = blob_off + len(blob)
    rela_bytes = b"".join(
        struct.pack("<IIi", off, code | (idx << 16), addend)
        for off, code, idx, addend in rela)

    header = MAGIC + struct.pack(
        "<IIIIIIIIIIIIIII",
        VERSION, base, img, bss, api, export_off, len(exports),
        rela_off, len(rela),
        0,            # text_end (RO span end; filled by 2C-3 split)
        img,          # data_end (.data end = whole image for now)
        0, 0,         # init_array_off / cnt
        0, 0,         # fini_array_off / cnt
    )
    assert len(header) == HEADER_SIZE

    out = header + table + blob + rela_bytes + image
    with open(out_path, "wb") as f:
        f.write(out)
    print("%s: base=0x%x image=%d bss=0x%x api=%s exports=%d blob=%d rela=%d file=%d"
          % (out_path, base, img, bss,
             "none" if api == NO_API else "0x%x" % api,
             len(exports), len(blob), len(rela), len(out)))


if __name__ == "__main__":
    main()
