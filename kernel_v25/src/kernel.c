#include "vga.h"
#include "keyboard.h"
#include "shell.h"
#include "net.h"
#include "cpu.h"
#include "intr.h"
#include "paging.h"
#include "task.h"
#include "hw.h"
#include "snd.h"
#include "usb.h"

uint32_t multiboot_info_ptr = 0;
uint32_t multiboot_flags = 0;

/* PCI scan results (filled by net_init) */
extern int      netdbg_pci_count;
extern uint16_t netdbg_pci_vend[8];
extern uint16_t netdbg_pci_dev[8];
extern int      net_fail_code;
extern int      net_env_vmware(void);

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
    btime_mark("K0 kernel_main enter");

    vga_init();
    kb_init();
    btime_mark("K1 vga+kbd init");

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
    btime_mark("K2 paging");
    intr_init();
    tasks_init();
    intr_enable();
    btime_mark("K3 intr+tasks+sti");
    hw_mem_boot(lo, hi);
    hw_calibrate_cpu();      /* 100 ms: TSC vs PIT -> hw_num(HW_CPU_MHZ) */
    btime_mark("K4 mem+cpu calibrate");

    /* Sound Blaster 16 (ISA, IRQ5).  Must come after intr_enable() --
     * snd_init() unmasks IRQ5 -- and before shell_run(), which never
     * returns.  A missing card is not fatal; the existing no-audio QEMU
     * regressions must keep booting unchanged.  To hear anything, add:
     *   -audiodev wav,id=snd0,path=snd.wav
     *   -device sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0
     * (QEMU >= 7.1 removed -soundhw; -device is the only form now.) */
    if (snd_init() == 0) {
        int be = snd_info(SND_INFO_BACKEND);
        if (be == 2)
            vga_write("snd: ES1370/1371 (PCI) ready\n");
        else
            vga_write("snd: Sound Blaster 16 ready (0x220 irq5 dma1)\n");
    } else {
        vga_write("snd: no audio device detected (audio disabled)\n");
    }
    btime_mark("K5 snd_init");

    /* v2.26 USB (UHCI + HID keyboard).  Missing controller is NOT fatal:
     * PS/2 remains the primary input path and all regressions must boot
     * identically without -usb.  Port reset costs ~200ms only when a
     * device actually attaches. */
    if (usb_start() && usb_ready())
        vga_write("usb: UHCI ready, HID keyboard on port\n");
    btime_mark("K6 usb_start");

    if (net_init() < 0) {
        vga_write("net: no usable NIC");
        if (net_fail_code == 1) vga_write(" (no 1022:2000 in PCI)");
        else if (net_fail_code == 2) vga_write(" (open bus, no chip answering)");
        else if (net_fail_code == 3) vga_write(" (MAC unreadable)");
        else if (net_fail_code == 4) vga_write(" (INIT/START failed)");
        vga_write("\nPCI devices on bus 0: ");
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
        if (net_env_vmware())
            vga_write("VMware guest: set the VM network adapter to Intel E1000\n"
                      "  (e1000 driver is on the roadmap; PCnet only works if\n"
                      "   the adapter is switched to 'AMD PCnet')\n");
        else
            vga_write("QEMU: -netdev user,id=n0 -device pcnet,netdev=n0\n");
    } else {
        vga_write("net: AMD PCnet-PCI up\n");
    }
    btime_mark("K7 net_init");

    shell_run();
}
