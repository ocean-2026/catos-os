ENTRY(ce_main)
SECTIONS
{
    . = 0x2000000;   /* CE_LOAD_BASE: absolute references inside the
                        image must match the load address, otherwise
                        string/API-table accesses hit wrong memory */
    .text : ALIGN(4) { *(.text.ce_entry) *(.text*) }
    .rodata : ALIGN(4) { *(.rodata*) }
    .data : ALIGN(4) { *(.data*) }
    .ce_api : ALIGN(4) { *(.ce_api) }
    /* .bss stays out of the file: the CE loader zeroes bss_size bytes
     * after the image, so objcopy must not pad the binary with it. */
    .bss : ALIGN(4) { *(.bss*) *(COMMON) }
}
