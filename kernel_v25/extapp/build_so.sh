#!/bin/sh
# build_so.sh <lib.c> [outdir] [base_hex] [name=symbol,...]
#
# Build a CSO1 shared object:
#   sh build_so.sh libmath.c bin 0x2700000 add=lib_add,mul=lib_mul
#
# base defaults to 0x2700000.  Each library in a program must use a
# DIFFERENT page-aligned base in 0x2700000..0x2800000; the linker base is
# the load address (CSO1 has no relocations).
#
# The library may include ce_abi.h and declare
#   catos_api_t *__ce_api __attribute__((section(".ce_api")));
# to receive the vsyscall table pointer at load time.

set -e

if command -v gcc >/dev/null 2>&1; then
    GCC=gcc; LD=ld; OBJCOPY=objcopy; OBJDUMP=objdump; NM=nm
else
    BIN=/d/Tools/MSY32/msys64/mingw32/bin
    GCC="$BIN/gcc.exe"; LD="$BIN/ld.exe"; OBJCOPY="$BIN/objcopy.exe"
    OBJDUMP="$BIN/objdump.exe"; NM="$BIN/nm.exe"
fi
PY=""
if [ -n "$PYTHON" ]; then PY="$PYTHON"; fi
if [ -z "$PY" ]; then
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
    done
fi
[ -z "$PY" ] && { echo "ERROR: no python found"; exit 1; }

SRC=$1
[ -z "$SRC" ] && { echo "usage: sh build_so.sh lib.c [outdir] [base] [name=sym,...]"; exit 1; }
OUTREL=${2:-bin}
BASE=${3:-0x2700000}
EXPORTS=${4:-}
NAME=$(basename "$SRC" .c)
CEBASE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$CEBASE/.." && pwd)
mkdir -p "$CEBASE/build" "$CEBASE/$OUTREL"
cd "$CEBASE"

CFLAGS="-m32 -ffreestanding -fno-pic -fno-stack-protector -fno-stack-check \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-80387 \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -O2 -fno-omit-frame-pointer -std=c99"

echo "== building $NAME.so =="
"$GCC" $CFLAGS -I"$CEBASE" -I"$ROOT/include" -c "$SRC" -o "build/${NAME}_so.o"
"$GCC" $CFLAGS -I"$CEBASE" -I"$ROOT/include" -c so_stub.c -o build/so_stub.o

# Generate a private linker script pinned at the requested base.
LD_SCRIPT="build/${NAME}.so.ld"
{
    echo "SECTIONS"
    echo "{"
    echo "    . = $BASE;"
    echo '    .text   : ALIGN(4) { *(.text*) }'
    echo '    .rodata : ALIGN(4) { *(.rodata*) }'
    echo '    .data   : ALIGN(4) { *(.data*) }'
    echo '    .ce_api : ALIGN(4) { *(.ce_api) }'
    echo '    .bss    : ALIGN(4) { *(.bss*) *(COMMON) }'
    echo '    /DISCARD/ : { *(.reloc*) *(.edata*) *(.idata*) *(.rsrc*) }'
    echo "}"
} > "$LD_SCRIPT"

if "$NM" "build/${NAME}_so.o" 2>/dev/null | grep -q ' so_entry$'; then
    ENTRY=so_entry
else
    ENTRY=_so_entry
fi
"$LD" -m i386pe --disable-reloc-section -T "$LD_SCRIPT" -e "$ENTRY" \
    "build/${NAME}_so.o" build/so_stub.o -o "build/${NAME}.so.pe"

API=$("$NM" "build/${NAME}.so.pe" | awk '/[DR] _*__ce_api$/{print $1; exit}')
if [ -z "$API" ]; then
    APIPARM=none
else
    APIPARM=$("$PY" -c "print('%x' % (int('$API',16) - int('$BASE',16)))")
fi

"$OBJCOPY" -R .bss "build/${NAME}.so.pe" "build/${NAME}.nobss.so.pe"
"$OBJCOPY" -O binary "build/${NAME}.nobss.so.pe" "build/${NAME}.so.bin"

# Resolve each export's symbol name to a linked VMA, then let mkso.py turn
# it into an offset from base.  Try both the bare and the _-prefixed name
# (MinGW's i386pe target mangles C symbols differently across builds).
SPECS=""
if [ -n "$EXPORTS" ]; then
    for pair in $(echo "$EXPORTS" | tr ',' ' '); do
        EXPNAME=${pair%%=*}
        SYM=${pair#*=}
        VMA=$("$NM" "build/${NAME}.so.pe" | awk -v s="$SYM" '$3==s || $3=="_"s {print $1; exit}')
        if [ -z "$VMA" ]; then
            echo "ERROR: export symbol $SYM not found in $NAME.so"; exit 1
        fi
        if [ -n "$SPECS" ]; then SPECS="$SPECS,"; fi
        SPECS="$SPECS$EXPNAME=$VMA"
    done
fi

"$PY" "$CEBASE/build/mkso.py" "build/${NAME}.so.bin" \
    "$CEBASE/$OUTREL/$NAME.so" "$BASE" "$APIPARM" "$SPECS"
echo "OK -> $OUTREL/$NAME.so"
