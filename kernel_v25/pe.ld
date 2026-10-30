/* pe.ld - PE/COFF linker script for CatOS on MinGW (Windows)
 * Places everything at physical address 0x1000000, then objcopy
 * converts to a flat binary that GRUB loads via the multiboot
 * header (which must be at byte 0 of the flat image).
 *
 * NOTE: mingw gcc emits .rdata/.eh_frame sections that ld would
 * otherwise place BEFORE .text (they would then land at the start
 * of the flat binary and push the multiboot header away).  They are
 * folded into .text here so the multiboot header stays at 0x1000000.
 */

ENTRY(_start)

/* PE/COFF prepends underscore to C symbols; alias them for asm */
kernel_main = _kernel_main;

SECTIONS
{
    . = 0x1000000;

    .text : {
        *(.multiboot)
        *(.text)
        *(.rodata*)
        *(.rdata*)
        *(.eh_frame*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    /* objcopy -O binary zero-fills the bss into the flat file and the
     * image ends after .reloc/.bss.  (A NOLOAD bss + loader-side fill was
     * tried and reverted: GRUB 2.06's iso9660 path does not honor the
     * a.out-kludge fill for flat binaries - see build.sh.) */
    _load_end = .;
    _bss_end = _load_end;
    __bss_end = _load_end;   /* C spelling (MinGW adds a leading underscore) */
}
