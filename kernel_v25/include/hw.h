#ifndef HW_H
#define HW_H

#include <stdint.h>

/* hw.h - hardware-properties query API (CatOS' HardwarePropertiesManager).
 *
 * Everything is fetched through two entry points:
 *   hw_num(what) -> int      numeric properties (MHz, KB, counts)
 *   hw_str(what, buf, max)   string properties (names, IDs, lists)
 * so ring3 programs use them as int 0x80 syscalls while the shell/top
 * call them directly.  Both return 0 / -1("unknown") / copied length.
 */

/* hw_num selectors */
enum {
    HW_CPU_MHZ = 1,     /* calibrated TSC frequency (0 = no TSC)   */
    HW_CPU_CORES,       /* CPUs managed by this kernel (always 1)  */
    HW_CPU_FAMILY, HW_CPU_MODEL, HW_CPU_STEPPING,
    HW_MEM_TOTAL_KB,    /* low memory reported by the bootloader   */
    HW_MEM_FREE_KB,     /* free 4 KiB frames from the allocator    */
    HW_MEM_USED_KB,     /* allocated frames                        */
    HW_MEM_KERNEL_KB,   /* kernel image size (text+data+bss)       */
    HW_DISK_MB,         /* first IDE disk capacity                 */
    HW_DISK_LBA48,
    HW_NIC_UP,
    HW_UPTIME_MS,
    HW_TASKS,           /* tasks currently alive                   */
    HW_TICKS,           /* timer ticks since boot                  */
};

/* hw_str selectors */
enum {
    HW_CPU_VENDOR_S = 1,
    HW_CPU_BRAND_S,    /* full brand string (e.g. "QEMU Virtual ...") */
    HW_CPU_FEATURES_S, /* notable CPUID feature flags                 */
    HW_DISK_MODEL_S, HW_DISK_SERIAL_S, HW_DISK_FW_S,
    HW_NIC_MODEL_S, HW_NIC_MAC_S, HW_NIC_IP_S,
    HW_PCI_LIST_S,     /* lspci-style multi-line device list          */
};

int  hw_num(int what);
int  hw_str(int what, char *buf, int max);

/* one-shot full dump (multi-line); returns bytes written */
int  hw_summary(char *buf, int max);

/* boot-time helpers */
void hw_calibrate_cpu(void);          /* after interrupts are on: measures
                                         the TSC against the PIT          */
void hw_mem_boot(uint32_t lower_kb, uint32_t upper_kb);

#endif
