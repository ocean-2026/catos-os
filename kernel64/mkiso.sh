#!/bin/sh
# mkiso.sh - wrap catos64.elf into a GRUB rescue ISO and boot-test it
set -e
cd "$(dirname "$0")"
mkdir -p build/iso/boot/grub
cp build/catos64.elf build/iso/boot/catos64.elf
cat > build/iso/boot/grub/grub.cfg <<'CFG'
set timeout=0
set default=0
menuentry "CatOS64 P0" {
    multiboot /boot/catos64.elf
    boot
}
CFG
grub-mkrescue -o build/catos64.iso build/iso >/dev/null 2>&1
ls -l build/catos64.iso
