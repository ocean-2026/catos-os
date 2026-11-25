#!/bin/sh
# build_so.sh <lib.c> [outdir] [base_hex] [name=symbol,...]
#
# Build a CSO2 shared object as a PIC -shared ELF, then wrap it with mkso.py.
#   sh build_so.sh libmath.c bin 0x2700000 add=lib_add,mul=lib_mul
#
# The ELF is linked at vaddr 0 (position independent); the kernel loads the
# flat image at `base` and applies the extracted CSO2 relocations.  Reloc
# types present in the PIC output: R_386_GLOB_DAT(6), R_386_JMP_SLOT(7),
# R_386_RELATIVE(8), and (for absolute data pointers) R_386_32(1).

set -e

if command -v gcc >/dev/null 2>&1; then
    GCC=gcc; LD=ld; OBJCOPY=objcopy; OBJDUMP=objdump; NM=nm; READELF=readelf
else
    BIN=/d/Tools/MSY32/msys64/mingw32/bin
    GCC="$BIN/gcc.exe"; LD="$BIN/ld.exe"; OBJCOPY="$BIN/objcopy.exe"
    OBJDUMP="$BIN/objdump.exe"; NM="$BIN/nm.exe"; READELF="$BIN/readelf.exe"
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

CFLAGS="-m32 -ffreestanding -fPIC -fno-stack-protector -fno-stack-check \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-80387 \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -O2 -fno-omit-frame-pointer -std=c99"

echo "== building $NAME.so (PIC -shared) =="
"$GCC" $CFLAGS -I"$CEBASE" -I"$ROOT/include" -c "$SRC" -o "build/${NAME}_so.o"
"$GCC" $CFLAGS -I"$CEBASE" -I"$ROOT/include" -c so_stub.c -o build/so_stub.o
for o in "${NAME}_so" so_stub; do
    "$OBJCOPY" --remove-section=.comment --remove-section=.note.GNU-stack \
        "build/$o.o" 2>/dev/null || true
done

# Private linker script pinned at vaddr 0; only mapped sections are listed.
# The dynamic sections (.dynsym/.dynstr/.rel.dyn/.rel.plt/.got*) are kept by
# ld for readelf inspection but are NOT part of the flat image.
LD_SCRIPT="build/${NAME}.so.ld"
{
    echo "SECTIONS"
    echo "{"
    echo "    . = 0;"
    echo '    .text   : ALIGN(4) { *(.text*) }'
    echo '    .rodata : ALIGN(4) { *(.rodata*) }'
    echo '    .data   : ALIGN(4) { *(.data*) }'
    echo '    .ce_api : ALIGN(4) { *(.ce_api) }'
    echo '    .bss    : ALIGN(4) { *(.bss*) *(COMMON) }'
    echo "}"
} > "$LD_SCRIPT"

ENTRY=so_entry
"$LD" -m elf_i386 -shared -T "$LD_SCRIPT" -e "$ENTRY" \
    "build/${NAME}_so.o" build/so_stub.o -o "build/${NAME}.so.elf"

API=$("$NM" "build/${NAME}.so.elf" | awk '/[DRB] _*__ce_api$/{print $1; exit}')
if [ -z "$API" ]; then
    APIPARM=none
else
    APIPARM="0x$("$PY" -c "print('%x' % int('$API',16))")"
fi

BSS=$("$READELF" -S "build/${NAME}.so.elf" | awk '/\.bss/{print $3; exit}')
if [ -z "$BSS" ]; then BSS=0; else BSS="0x$BSS"; fi

"$OBJCOPY" -R .bss "build/${NAME}.so.elf" "build/${NAME}.nobss.so.elf"
"$OBJCOPY" -O binary "build/${NAME}.nobss.so.elf" "build/${NAME}.so.bin"

# Resolve each export by internal symbol name; mkso.py reads the offset from
# the ELF symtab.  nm -D is kept as a cross-check for missing symbols.
SPECS=""
if [ -n "$EXPORTS" ]; then
    for pair in $(echo "$EXPORTS" | tr ',' ' '); do
        EXPNAME=${pair%%=*}
        SYM=${pair#*=}
        if ! "$NM" "build/${NAME}.so.elf" | grep -qw "$SYM" 2>/dev/null; then
            echo "ERROR: export symbol $SYM not found in $NAME.so"; exit 1
        fi
        if [ -n "$SPECS" ]; then SPECS="$SPECS,"; fi
        SPECS="$SPECS$EXPNAME=$SYM"
    done
fi

"$PY" "$CEBASE/build/mkso.py" "build/${NAME}.so.elf" "build/${NAME}.so.bin" \
    "$CEBASE/$OUTREL/$NAME.so" "$BASE" "$BSS" "$APIPARM" "$SPECS"
echo "OK -> $OUTREL/$NAME.so"
