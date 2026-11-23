; demo_inc.s - embed the built demo ELF64 into the kernel image
BITS 64

SECTION .rodata
global demo_elf
global demo_elf_size
demo_elf:
	incbin "build/hello.elf"
demo_end:
demo_elf_size:
	dd demo_end - demo_elf
