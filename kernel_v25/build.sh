#!/bin/sh
# build.sh - build CatOS on Windows MinGW (no make required)
#
#   sh build.sh          -> build/kernel.bin + catos.iso
#   sh build.sh kernel   -> build/kernel.bin only
#   sh build.sh iso      -> kernel.bin + catos.iso
#   sh build.sh disk     -> apps + catos_disk.img (pre-populated /bin,
#                           the decoupled app source; kernel image is
#                           only the embedded fallback)
#
# ISO: built by build/patch_work_iso.py, which appends kernel.bin to the
# proven GRUB template build/catos_work.iso (El Torito no-emulation boot).
#
# Run the result with:
#   qemu-system-i386 -kernel build/kernel.bin -m 256
#   qemu-system-i386 -cdrom catos.iso -m 256 -hda catos_disk.img
set -e

CC=gcc
LD=ld
OBJCOPY=objcopy
NM=nm
# -fleading-underscore: the asm files (cpu.s/boot.s) export/import symbols
# with a leading '_' (MinGW convention); on GNU/Linux gcc does not add it
# by default, so we ask gcc to match the asm exactly.
CFLAGS="-m32 -ffreestanding -fno-pic -fno-stack-protector -fno-stack-check -fleading-underscore -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-80387 -Wall -O2 -std=c99 -Iinclude"

# find a working python (needed for the built-in ISO generator and for
# embedding the CE apps into the kernel image)
PY=""
if [ -n "$PYTHON" ] && command -v "$PYTHON" >/dev/null 2>&1; then
    PY="$PYTHON"
fi
if [ -z "$PY" ] && [ -x /d/Tools/Python312/python.exe ]; then
    PY=/d/Tools/Python312/python.exe
fi
if [ -z "$PY" ]; then
    for c in python3 py python; do
        if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
    done
fi

mkdir -p build

build_apps() {
echo "== building CE apps (extapp) =="
# 'cc' is the self-hosting C compiler.  It was NOT in this list until
# now, which is why extapp/bin/cc.ce had been a stale artifact since
# ae0ee2d: the source changed but the image was never rebuilt, so the
# shipped compiler bore no reproducible relation to the source tree.
# Single source of truth: build/apps.list is also read by
# build/gen_builtin.py, so an app added here is automatically embedded.
APPS=""
while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    APPS="$APPS $line"
done < build/apps.list
for app in $APPS; do
    sh extapp/build_ce.sh $app.c
done

# node.c lives outside extapp/ (kernel_v25/node/), and its .ce has always
# been kept next to the source, so build it into ../node (the second
# argument to build_ce.sh selects the output directory).
echo "== building CE apps (node) =="
sh extapp/build_ce.sh ../node/node.c ../node
}

build_disk() {
    if [ -n "$PY" ]; then
        "$PY" build/mkdisk.py -o catos_disk.img -m 64
    else
        echo "ERROR: no python found for mkdisk.py" >&2
        exit 1
    fi
    echo "OK -> catos_disk.img (boot with: -hda catos_disk.img)"
}

# early dispatch: the disk target packages extapp/bin into a FAT32 image
# and does NOT need the kernel compile below.
case "$1" in
    disk) build_apps; build_disk; exit 0 ;;
esac

echo "== generating built-in app blobs =="
if [ -n "$PY" ]; then
    "$PY" build/gen_builtin.py
else
    echo "ERROR: no python found for gen_builtin.py" >&2
    exit 1
fi

echo "== compiling boot.s =="
$CC -m32 -c boot/boot.s -o build/boot.o
echo "== compiling cpu.s =="
$CC -m32 -c src/cpu.s -o build/cpu.o

for src in src/kernel.c src/vga.c src/render.c src/render_font.c src/be_text.c src/be_gfx.c src/be_vbe.c src/be_vbe24.c src/stubs.c src/ime.c src/keyboard.c src/rtc.c src/shell.c src/fs.c src/ata.c src/fat.c src/ce.c src/net.c src/netstack.c src/builtin_apps.c src/intr.c src/paging.c src/vma.c src/task.c src/hw.c src/snd.c src/diag.c src/user.c; do
    obj="build/$(basename $src .c).o"
    echo "== compiling $src =="
    $CC $CFLAGS -c $src -o $obj
done

echo "== linking (PE/COFF) =="
# NOTE: ld warns "section below image base" because our segments sit at
# 0x100000 (the i386 kernel load address) while PE's default image base
# is 0x400000.  Harmless: we only objcopy the flat image below.
# Do NOT add --image-base 0x100000: it makes objcopy emit a 1 MB zero
# gap at the start of kernel.bin, which breaks the multiboot image.
$LD -m i386pe -T pe.ld -o build/kernel.pe \
    build/boot.o build/cpu.o build/kernel.o build/vga.o build/render.o build/render_font.o build/be_text.o build/be_gfx.o build/be_vbe.o build/be_vbe24.o build/stubs.o build/ime.o build/keyboard.o build/rtc.o build/shell.o build/fs.o build/ata.o build/fat.o build/ce.o build/net.o build/netstack.o build/builtin_apps.o build/intr.o build/paging.o build/vma.o build/task.o build/hw.o build/snd.o build/diag.o build/user.o

echo "== extracting flat binary =="
$OBJCOPY -O binary build/kernel.pe build/kernel.bin
# NOTE: the bss is zero-filled into the flat file on purpose.  Truncating
# it at _load_end (letting the loader's [load_end, bss_end) fill do the
# zeroing) breaks on GRUB 2.06's iso9660+a.out-kludge path: the first
# timer interrupt returns with a smashed frame (observed EIP=1) - the
# loader does not honor the kludge fill for flat binaries.  Cost: the ISO
# carries ~4.6 MiB of zeros; correctness wins.
echo "OK -> build/kernel.bin"

build_iso() {
    echo "== building catos.iso =="
    # patch_work_iso.py appends kernel.bin to the proven bootable GRUB
    # template build/catos_work.iso.
    if [ -n "$PY" ]; then
        echo "using patch_work_iso.py (proven GRUB template)"
        "$PY" build/patch_work_iso.py
    else
        echo "ERROR: no python found. Set PYTHON=/path/to/python.exe or install python." >&2
        exit 1
    fi
    echo "OK -> catos.iso"
}

case "$1" in
    kernel) : ;;
    iso)     build_iso ;;
    *)       build_iso ;;
esac
