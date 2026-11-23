#!/bin/sh
# build.sh - CatOS64 P0 (kernel64) build
# toolchain: nasm, x86_64 gcc (freestanding), ld -m elf_x86_64
set -e
cd "$(dirname "$0")"
OUT=build
mkdir -p $OUT

# 1. demo user program (ELF64, linked at 0x40000000)
nasm -f elf64 -o $OUT/hello.o demo/hello.s
ld -m elf_x86_64 -T demo/hello.ld -o $OUT/hello.elf $OUT/hello.o

# 2. boot assembly (+ embedded demo ELF)
nasm -f elf64 -o $OUT/boot32.o boot/boot32.s
nasm -f elf64 -o $OUT/isr64.o  boot/isr64.s
nasm -f elf64 -o $OUT/demo_inc.o boot/demo_inc.s

# 3. C kernel
CFLAGS="-m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -O2 -Wall -Wextra -Iinclude"
for s in src/serial.c src/pmm64.c src/idt64.c src/sys64.c src/elf64.c src/kmain.c; do
    gcc $CFLAGS -c $s -o $OUT/$(basename $s .c).o
done

# 4. link
ld -m elf_x86_64 -T boot/k64.ld -o $OUT/catos64.elf \
    $OUT/boot32.o $OUT/isr64.o $OUT/demo_inc.o \
    $OUT/serial.o $OUT/pmm64.o $OUT/idt64.o $OUT/sys64.o $OUT/elf64.o $OUT/kmain.o

echo "OK -> $OUT/catos64.elf"

