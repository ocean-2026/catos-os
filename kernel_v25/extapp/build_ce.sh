#!/bin/sh
# build_ce.sh <app.c> [outdir] - compile one CatOS application into a .ce
#
#   sh build_ce.sh hello.c              -> bin/hello.ce
#   sh build_ce.sh ../node/node.c ../node -> ../node/node.ce
#
# outdir is relative to this script's directory (extapp/) and defaults to
# "bin".  It exists so an app whose source lives outside extapp/ can keep
# its .ce next to its source (node/ lives at kernel_v25/node/).
#
# Pipeline: gcc (freestanding i386) -> ld (PE, extapp/ce.ld) ->
# objcopy -O binary -> build/mkce.py wraps the CE01 header.
# The entry point ce_main() must sit at image offset 0 (ce.ld puts
# .text.ce_entry first); bss size and the __ce_api slot offset are read
# from objdump/nm.
set -e

# Toolchain: prefer PATH (gcc/ld/objdump/nm/python); fall back to the
# original absolute MSYS2 location if it exists on this machine.
if command -v gcc >/dev/null 2>&1; then
    GCC=gcc; LD=ld; OBJCOPY=objcopy; OBJDUMP=objdump; NM=nm
else
    BIN=/d/Tools/MSY32/msys64/mingw32/bin
    GCC="$BIN/gcc.exe"
    LD="$BIN/ld.exe"
    OBJCOPY="$BIN/objcopy.exe"
    OBJDUMP="$BIN/objdump.exe"
    NM="$BIN/nm.exe"
fi
PY=""
if [ -n "$PYTHON" ]; then PY="$PYTHON"; fi
if [ -z "$PY" ]; then
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
    done
fi
if [ -z "$PY" ]; then echo "ERROR: no python found"; exit 1; fi

SRC=$1
if [ -z "$SRC" ]; then echo "usage: sh build_ce.sh app.c [outdir]"; exit 1; fi
OUTREL=${2:-bin}
NAME=$(basename "$SRC" .c)
CEBASE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$CEBASE/.." && pwd)
mkdir -p "$CEBASE/build" "$CEBASE/$OUTREL"
cd "$CEBASE"   # compile/link/output paths are relative to extapp/

echo "== building $NAME.ce =="
# -I"$CEBASE"     : app-local headers (ce.ld-adjacent, API.txt examples)
# -I"$ROOT/include": the v2.5 user-mode ABI, ce_abi.h (57-slot table)
# NOTE: the -I paths are passed as SEPARATE, quoted arguments at the point
# of use rather than embedded in $CFLAGS.  $CFLAGS is expanded unquoted,
# so a path containing a space would be split into two arguments; and a
# quote character stored inside a variable is NOT re-processed as a
# quote during that expansion, so "-I\"$CEBASE\"" would hand gcc a
# literal '"' instead of protecting the space.
#
# -DCC_HOST selects the "compile against the real CE ABI" branch of the
# self-hosting compiler (kernel_v25/extapp/cc.c, content-identical to
# cc_self.c).  Without it that branch is skipped and the file falls back
# to its freestanding type shims, where catos_api_t is `typedef void`:
# every api->alloc / api->write call then fails to compile, and the
# `section(".text.ce_entry")` attribute on ce_main disappears, so the
# "ce_main not at image offset 0" check below would fail too.
# Only cc.c reacts to the macro -- every other CE app ignores it.
CFLAGS="-m32 -ffreestanding -fno-pic -fno-stack-protector -fno-stack-check \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-80387 \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -O2 -fno-omit-frame-pointer -std=c99 -DCC_HOST"
"$GCC" $CFLAGS -I"$CEBASE" -I"$ROOT/include" -c "$SRC" -o "$CEBASE/build/$NAME.o"
# runtime stub (__chkstk_ms etc.) -- see ce_stub.c
"$GCC" $CFLAGS -I"$CEBASE" -I"$ROOT/include" -c "$CEBASE/ce_stub.c" \
    -o "$CEBASE/build/ce_stub.o"
# v2.49 shared app layer: single __ce_api slot definition -- see ce_app.c
"$GCC" $CFLAGS -I"$CEBASE" -I"$ROOT/include" -c "$CEBASE/ce_app.c" \
    -o "$CEBASE/build/ce_app.o"

# ce_main must be at offset 0: force it via the linker script.
# (MinGW mangles C symbols with a leading underscore: _ce_main; GNU ld on
# Linux does not: ce_main)
if "$NM" "$CEBASE/build/$NAME.o" 2>/dev/null | grep -q ' ce_main$'; then
    ENTRY_SYM=ce_main
else
    ENTRY_SYM=_ce_main
fi
# --disable-reloc-section: a PE .reloc section is placed after .bss, and
# objcopy -O binary would pad the flat image up to it (851 KiB of zeros for
# cc.c's static buffers, far over CE_MAX_IMAGE).  The CE loader relocates
# nothing, so the section is pure weight.
"$LD" -m i386pe --disable-reloc-section -T "$CEBASE/ce.ld" -e "$ENTRY_SYM" \
    "$CEBASE/build/$NAME.o" "$CEBASE/build/ce_stub.o" "$CEBASE/build/ce_app.o" \
    -o "$CEBASE/build/$NAME.exe"

MAIN=$("$NM" "$CEBASE/build/$NAME.exe" | awk '/[TD] _*ce_main$/{print $1; exit}')
# ce_main must sit at image offset 0: with link base 0x2000000 (CE_LOAD_BASE)
# its VMA is 0x02000000, which objcopy maps to file offset 0.
if [ -z "$MAIN" ] || [ "$MAIN" != "02000000" ] && [ "$MAIN" != "2000000" ]; then
    echo "ERROR: ce_main not at image offset 0 (base 0x2000000, found '$MAIN')"; exit 1
fi

BSS=$("$OBJDUMP" -h "$CEBASE/build/$NAME.exe" | awk '/\.bss/{print $3; exit}')
API=$("$NM" "$CEBASE/build/$NAME.exe" | awk '/[DR] _*ce_api$/{print $1; exit}')
if [ -z "$BSS" ]; then BSS=0; fi
if [ -z "$API" ]; then echo "ERROR: no __ce_api symbol"; exit 1; fi

# objcopy -O binary pads the file out to the end of the last section, so a
# large .bss (cc.c has ~850 KiB of static buffers) would be written as
# zeros and blow the CE_MAX_IMAGE limit.  Strip .bss from a copy first; the
# kernel zeroes bss_size bytes after the image anyway, so this matches the
# loader contract exactly.
"$OBJCOPY" -R .bss "$CEBASE/build/$NAME.exe" "$CEBASE/build/$NAME.nobss.exe"
"$OBJCOPY" -O binary "$CEBASE/build/$NAME.nobss.exe" "$CEBASE/build/$NAME.bin"

# nm reports the linked VMA (0x02001094 with base 0x2000000); the header
# needs the file offset inside the raw image, so subtract the base.
# (16#.. is bash-only; use python so dash/MinGW-sh both work.)
API_HEX=$("$PY" -c "import sys; print('%x' % (int('$API',16) - 0x2000000))")

"$PY" "$CEBASE/build/mkce.py" "$CEBASE/build/$NAME.bin" \
    "$CEBASE/$OUTREL/$NAME.ce" "$BSS" "$API_HEX"
echo "OK -> $OUTREL/$NAME.ce"
