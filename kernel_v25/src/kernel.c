#include "vga.h"
#include "keyboard.h"
#include "shell.h"
#include "net.h"
#include "cpu.h"
#include "intr.h"
#include "paging.h"
#include "task.h"
#include "hw.h"

uint32_t multiboot_info_ptr = 0;
uint32_t multiboot_flags = 0;

/* PCI scan results (filled by net_init) */
extern int      netdbg_pci_count;
extern uint16_t netdbg_pci_vend[8];
extern uint16_t netdbg_pci_dev[8];

static void print_hex4(uint16_t v) {
    static const char *hex = "0123456789abcdef";
    vga_putc(hex[(v >> 12) & 0xF]);
    vga_putc(hex[(v >> 8) & 0xF]);
    vga_putc(hex[(v >> 4) & 0xF]);
    vga_putc(hex[v & 0xF]);
}

void kernel_main(uint32_t mb_magic, uint32_t mb_info) {
    (void)mb_magic;
    multiboot_info_ptr = mb_info;

    vga_init();
    kb_init();

    /* five-pillar boot order:
     *   1. paging (identity kernel map + frame allocator)
     *   2. GDT/IDT/PIC/PIT (interrupts still masked)
     *   3. task table (shell = task 0), THEN enable interrupts
     *   4. drivers + shell (shell_run never returns)              */
    typedef struct {
        uint32_t flags;
        uint32_t mem_lower, mem_upper;
    } mb_mem_t;
    mb_mem_t *mi = (mb_mem_t *)mb_info;
    uint32_t lo = (mi && (mi->flags & 1)) ? mi->mem_lower : 640;
    uint32_t hi = (mi && (mi->flags & 1)) ? mi->mem_upper : (63u << 10);

    paging_init(lo, hi);
    intr_init();
    tasks_init();
    intr_enable();
    hw_mem_boot(lo, hi);
    hw_calibrate_cpu();      /* 100 ms: TSC vs PIT -> hw_num(HW_CPU_MHZ) */

    if (net_init() < 0) {
        vga_write("net: AMD PCnet-PCI (1022:2000) NOT found\n");
        vga_write("PCI devices on bus 0: ");
        if (netdbg_pci_count == 0) {
            vga_write("none\n");
        } else {
            for (int i = 0; i < netdbg_pci_count; i++) {
                print_hex4(netdbg_pci_vend[i]);
                vga_putc(':');
                print_hex4(netdbg_pci_dev[i]);
                if (i < netdbg_pci_count - 1) vga_putc(' ');
            }
            vga_write("\n");
        }
        vga_write("add to QEMU:  -netdev user,id=n0  -device pcnet,netdev=n0\n");
    } else {
        vga_write("net: AMD PCnet-PCI up (10.0.2.15)\n");
    }

    shell_run();
}
