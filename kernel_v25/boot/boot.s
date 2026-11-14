/* boot.s - Multiboot header + kernel entry point
 * We are loaded by GRUB in 32-bit protected mode, paging disabled.
 * FLAGS bit 16 (AOUT) is required: the flat binary has no ELF headers,
 * so the loader needs the explicit load/entry addresses below.
 */
.set MAGIC,    0x1BADB002
.set FLAGS,    0x00000003 | 0x00010000  /* ALIGN=1 | MEMINFO=2 | AOUT=0x10000 */
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
.long 0x1000000      /* header_addr (== load_addr; header sits at file start) */
.long 0x1000000      /* load_addr (16 MiB) */
.long _load_end      /* load_end_addr */
.long _bss_end       /* bss_end_addr */
.long _start         /* entry_addr */

/* Stack: 16 KiB */
.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .text
.global _start
_start:
    /* defensive bss clear: a no-op today (_load_end == _bss_end, the
     * bss is zero-filled into the file) but correct if the layout ever
     * changes back to a NOLOAD bss */
    mov $_load_end, %edi
    mov $_bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx
    xor %eax, %eax
    cld
    rep stosl

    /* Stack: 16 KiB (in .bss) */
    mov $stack_top, %esp

    /* Pass multiboot info pointer (EAX) to kernel_main */
    push %ebx
    push %eax

    call _kernel_main

    /* If kernel returns, halt */
    cli
1:  hlt
    jmp 1b
