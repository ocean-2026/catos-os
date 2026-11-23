/* kmain.c - P0 bring-up: banner, paging/pmm/idt/syscall init, ring3 demo, REPL */
#include "k64.h"

extern u8 __bss_end[];
extern u8 stack_top64[];

u64 kernel_rsp_save;

static u64 kernel_end_pa(void) { return (u64)__bss_end; }   /* identity VA == PA */

static void banner(struct multiboot *mb)
{
    serial_puts("\nCatOS64 P0 - long mode bring-up\n");
    serial_puts("  boot     : GRUB multiboot (ELF), 32-bit trampoline -> long mode\n");
    serial_puts("  paging   : PML4/PDPT/PD, identity 0-1 GiB (2 MiB pages)\n");
    serial_puts("             user window 0x40000000 (code) / 0x40200000 (stack)\n");
    serial_puts("  kernel   : ");
    serial_hex(KPHYS0);
    serial_puts(" .. ");
    serial_hex(kernel_end_pa());
    serial_puts("\n");
    if (mb) {
        serial_puts("  multiboot: flags=");
        serial_hex(mb->flags);
        serial_puts(" mem_upper(KiB)=");
        serial_dec(mb->mem_upper);
        serial_puts(" mmap_len=");
        serial_dec(mb->mmap_length);
        serial_puts("\n");
    }
}

void kmain64(struct multiboot *mb)
{
    serial_init();
    banner(mb);

    idt64_init();
    tss_set_rsp0((u64)stack_top64);
    pic_init();
    pit_init(1000);
    syscall64_init();
    serial_puts("  idt/pic  : 32 exception stubs + IRQ0 1 kHz + int 0x80 (DPL3)\n");
    serial_puts("  syscall  : IA32_LSTAR/SFMASK + EFER.SCE armed\n");
    __asm__ volatile ("sti");

    pmm_init(mb, kernel_end_pa());
    serial_puts("  pmm      : total frames=");
    serial_dec(pmm_total());
    serial_puts(" used=");
    serial_dec(pmm_used());
    serial_puts(" free=");
    serial_dec(pmm_total() - pmm_used());
    serial_puts("\n");

    user_window_init();
    elf64_demo_run();

    /* unreachable unless the demo returns without exit */
    repl_resume();
}

void repl_resume(void)
{
    serial_puts("\n[k64] back in kernel (ticks=");
    serial_dec(kuptime_ticks());
    serial_puts(")\ncatos64> ");
    for (;;) {
        char c = serial_getc();
        if (c) {
            if (c == '\r' || c == '\n') serial_puts("\ncatos64> ");
            else serial_putc(c);
        } else {
            __asm__ volatile ("hlt");
        }
    }
}

