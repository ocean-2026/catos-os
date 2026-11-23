SECTIONS
{
    . = 0x40000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
}
ENTRY(_start)
