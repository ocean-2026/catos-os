/* hw.c - hardware-properties queries (the CatOS analogue of Android's
 * HardwarePropertiesManager): CPU (CPUID + TSC/PIT calibration), memory
 * (multiboot + frame allocator), disk (cached ATA IDENTIFY strings),
 * NIC state and a lspci-style PCI inventory.
 */

#include "hw.h"
#include "io.h"
#include "ata.h"

extern uint32_t multiboot_info_ptr;
extern volatile uint32_t timer_ticks;
extern int  net_up(void);
extern uint8_t net_mac[6];
extern uint8_t net_our_ip[4];
extern void frame_stats(uint32_t *total, uint32_t *used);
extern int  task_count(void);
extern char _bss_end[];

/* ---- CPUID ---------------------------------------------------------- */

static char cpu_vendor[13];
static char cpu_brand[49];
static uint32_t cpu_sig;         /* eax of leaf 1 */
static uint32_t cpu_feats_edx, cpu_feats_ecx;
static int      cpu_has_tsc;
static uint32_t cpu_mhz;

static void cpuid_(uint32_t leaf, uint32_t *a, uint32_t *b,
                   uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf));
}

static void cpu_detect(void) {
    static int done;
    if (done) return;
    done = 1;

    uint32_t a, b, c, d;
    cpuid_(0, &a, &b, &c, &d);
    uint32_t maxleaf = a;
    *(uint32_t *)&cpu_vendor[0] = b;
    *(uint32_t *)&cpu_vendor[4] = d;
    *(uint32_t *)&cpu_vendor[8] = c;
    cpu_vendor[12] = 0;

    if (maxleaf >= 1) {
        cpuid_(1, &a, &b, &c, &d);
        cpu_sig = a;
        cpu_feats_edx = d;
        cpu_feats_ecx = c;
        cpu_has_tsc = (d >> 4) & 1;
    }

    /* extended leaves: brand string at 0x80000002..4 (the max BASIC leaf
     * never reaches 0x80000000 - always probe 0x80000000 separately) */
    (void)maxleaf;
    uint32_t ext = 0;
    cpuid_(0x80000000, &ext, &b, &c, &d);
    if (ext >= 0x80000004) {
        uint32_t *p = (uint32_t *)cpu_brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_(leaf, &p[0], &p[1], &p[2], &p[3]);
            p += 4;
        }
        cpu_brand[48] = 0;
        int i = 0;
        while (cpu_brand[i] == ' ') i++;
        if (i) {
            int j = 0;
            while (cpu_brand[i]) cpu_brand[j++] = cpu_brand[i++];
            cpu_brand[j] = 0;
        }
    }
}

static uint64_t rdtsc_(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void hw_calibrate_cpu(void) {
    cpu_detect();
    if (!cpu_has_tsc) return;
    /* measure the TSC over 10 timer ticks (100 ms at 100 Hz) */
    uint32_t t0 = timer_ticks;
    while (timer_ticks == t0) {}
    uint64_t c0 = rdtsc_();
    while ((uint32_t)(timer_ticks - t0) < 10) {}
    uint64_t c1 = rdtsc_();
    /* no libgcc 64-bit divide in the kernel: a 100 ms TSC delta is always
     * < 4 GiB-worth of ticks below 42 GHz, so 32-bit math is enough:
     * MHz = delta / 100000 (the window was 1/10 s) */
    uint32_t delta = (uint32_t)(c1 - c0);
    cpu_mhz = delta / 100000u;
}

static uint32_t mem_lower_kb = 640, mem_upper_kb = 63u << 10;
void hw_mem_boot(uint32_t lower_kb, uint32_t upper_kb) {
    if (lower_kb) mem_lower_kb = lower_kb;
    if (upper_kb) mem_upper_kb = upper_kb;
}

/* ---- PCI inventory ---------------------------------------------------- */

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t ven, dev_id;
    uint8_t  cls, sub;
    uint8_t  irq;
} pci_ent_t;

#define PCI_MAX_ENT 24
static pci_ent_t pci_ents[PCI_MAX_ENT];
static int pci_n = -1;                       /* -1 = not scanned yet */

static uint32_t pci_cfg_rd(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                | ((uint32_t)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

static void pci_scan_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t vd = pci_cfg_rd(bus, dev, fn, 0);
            uint16_t ven = vd & 0xFFFF, did = vd >> 16;
            if (ven == 0xFFFF) { if (fn == 0) break; continue; }
            if (pci_n >= PCI_MAX_ENT) return;
            uint32_t cl = pci_cfg_rd(bus, dev, fn, 8);
            uint32_t irqw = pci_cfg_rd(bus, dev, fn, 0x3C);
            pci_ents[pci_n].bus = bus;
            pci_ents[pci_n].dev = dev;
            pci_ents[pci_n].fn = fn;
            pci_ents[pci_n].ven = ven;
            pci_ents[pci_n].dev_id = did;
            pci_ents[pci_n].cls = (uint8_t)(cl >> 24);
            pci_ents[pci_n].sub = (uint8_t)(cl >> 16);
            pci_ents[pci_n].irq = (uint8_t)irqw;
            pci_n++;
            /* one level of PCI-PCI bridges */
            if ((uint8_t)(cl >> 16) == 0x04 && (uint8_t)(cl >> 24) == 0x06) {
                uint8_t sec = (uint8_t)(pci_cfg_rd(bus, dev, fn, 0x18) >> 8);
                if (sec > bus && sec < 255) pci_scan_bus(sec);
            }
            uint8_t ht = (uint8_t)(pci_cfg_rd(bus, dev, fn, 0x0C) >> 16);
            if (fn == 0 && !(ht & 0x80)) break;   /* not multifunction */
        }
    }
}

static const char *pci_name(uint16_t ven, uint16_t did) {
    static const struct { uint16_t v, d; const char *n; } tab[] = {
        {0x8086, 0x1237, "Intel 440FX - 82441FX PMC"},
        {0x8086, 0x7000, "Intel PIIX3 ISA"},
        {0x8086, 0x7010, "Intel PIIX3 IDE"},
        {0x8086, 0x7113, "Intel PIIX4 ACPI"},
        {0x1234, 0x1111, "QEMU Virtual VGA"},
        {0x1022, 0x2000, "AMD PCnet-PCI II (Am79C970A)"},
        {0x8086, 0x100E, "Intel e1000"},
        {0x15AD, 0x0405, "VMware SVGA II"},
        {0x15AD, 0x0790, "VMware PCI bridge"},
        {0x1B36, 0x0001, "Red Hat QXL VGA"},
        {0x1AF4, 0x1000, "virtio-net legacy"},
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i].v == ven && tab[i].d == did) return tab[i].n;
    return 0;
}

static void pci_ensure(void) {
    if (pci_n >= 0) return;
    pci_n = 0;
    pci_scan_bus(0);
}

/* ---- helpers ----------------------------------------------------------- */

static int scpy(char *dst, int max, const char *src) {
    int i = 0;
    if (max <= 0) return 0;
    for (; src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = 0;
    return i;
}

static int sap(char *b, int max, int n, const char *s) {
    if (n >= max) return n;
    n += scpy(b + n, max - n, s);
    return n;
}

static int sap_int(char *b, int max, int n, int v) {
    char t[12];
    int i = 0, neg = 0;
    unsigned u = (unsigned)v;
    if (v < 0) { neg = 1; u = 0u - u; }
    if (u == 0) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) t[i++] = '-';
    if (n >= max) return n;
    while (i && n < max - 1) b[n++] = t[--i];
    b[n] = 0;
    return n;
}

/* ---- the API ----------------------------------------------------------- */

int hw_num(int what) {
    switch (what) {
    case HW_CPU_MHZ:    cpu_detect(); return (int)cpu_mhz;
    case HW_CPU_CORES:  return 1;      /* this kernel manages the BSP only */
    case HW_CPU_FAMILY: cpu_detect(); return (int)((cpu_sig >> 8) & 0xF);
    case HW_CPU_MODEL:  cpu_detect(); return (int)(((cpu_sig >> 4) & 0xF)
                                                   | ((cpu_sig >> 12) & 0xF0));
    case HW_CPU_STEPPING: cpu_detect(); return (int)(cpu_sig & 0xF);
    case HW_MEM_TOTAL_KB: {
        uint32_t hi = mem_upper_kb < (63u << 10) ? mem_upper_kb : (63u << 10);
        return (int)(1024 + hi + mem_lower_kb / 1024);
    }
    case HW_MEM_FREE_KB:
    case HW_MEM_USED_KB: {
        uint32_t tot = 0, used = 0;
        frame_stats(&tot, &used);
        uint32_t kb = (what == HW_MEM_FREE_KB ? tot - used : used) * 4u;
        return (int)kb;
    }
    case HW_MEM_KERNEL_KB:
        return (int)(((uint32_t)_bss_end - 0x1000000u) / 1024u);
    case HW_DISK_MB: {
        uint32_t sec = 0;
        if (ata_identify(&sec) != 0) return 0;
        return (int)(sec / 2048u);
    }
    case HW_DISK_LBA48: return ata_lba48_sup();
    case HW_NIC_UP:     return net_up();
    case HW_UPTIME_MS:  return (int)(timer_ticks * 10u);
    case HW_TASKS:      return task_count();
    case HW_TICKS:      return (int)timer_ticks;
    default: return -1;
    }
}

int hw_str(int what, char *buf, int max) {
    if (!buf || max <= 0) return -1;
    switch (what) {
    case HW_CPU_VENDOR_S:
        cpu_detect();
        return scpy(buf, max, cpu_vendor[0] ? cpu_vendor : "unknown");
    case HW_CPU_BRAND_S:
        cpu_detect();
        return scpy(buf, max, cpu_brand[0] ? cpu_brand : "unknown");
    case HW_CPU_FEATURES_S: {
        cpu_detect();
        static const struct { uint32_t bit; const char *nm; } f[] = {
            {1 << 0,  "fpu"},   {1 << 4,  "tsc"},   {1 << 5,  "msr"},
            {1 << 9,  "apic"},  {1 << 15, "cmov"},  {1 << 23, "mmx"},
            {1 << 24, "fxsr"},  {1 << 25, "sse"},   {1 << 26, "sse2"},
        };
        int n = 0;
        buf[0] = 0;
        for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
            if (cpu_feats_edx & f[i].bit) {
                n = sap(buf, max, n, f[i].nm);
                n = sap(buf, max, n, " ");
            }
        }
        if (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
        (void)cpu_feats_ecx;
        return n;
    }
    case HW_DISK_MODEL_S:  return scpy(buf, max, ata_model_str());
    case HW_DISK_SERIAL_S: return scpy(buf, max, ata_serial_str());
    case HW_DISK_FW_S:     return scpy(buf, max, ata_fw_str());
    case HW_NIC_MODEL_S:
        return scpy(buf, max, net_up()
                    ? "AMD PCnet-PCI II (Am79C970A)" : "none");
    case HW_NIC_MAC_S: {
        if (!net_up()) return scpy(buf, max, "-");
        uint8_t *m = net_mac;
        static const char *h = "0123456789abcdef";
        int n = 0;
        for (int i = 0; i < 6; i++) {
            if (n < max - 2) buf[n++] = h[m[i] >> 4];
            if (n < max - 1) buf[n++] = h[m[i] & 0xF];
            if (i < 5 && n < max - 1) buf[n++] = ':';
        }
        buf[n] = 0;
        return n;
    }
    case HW_NIC_IP_S: {
        if (!net_up()) return scpy(buf, max, "-");
        int n = 0;
        for (int i = 0; i < 4; i++) {
            int v = net_our_ip[i];
            char t[4]; int ti = 0;
            if (v == 0) t[ti++] = '0';
            while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; }
            while (ti && n < max - 2) buf[n++] = t[--ti];
            if (i < 3 && n < max - 1) buf[n++] = '.';
        }
        buf[n] = 0;
        return n;
    }
    case HW_PCI_LIST_S: {
        pci_ensure();
        int n = 0;
        static const char *cls[] = {
            "unclass", "storage", "network", "display", "multimed",
            "memory", "bridge", "comm", "system", "input", "dock",
            "cpu", "serial", "wireless", "i2o", "satcomm",
        };
        for (int i = 0; i < pci_n; i++) {
            pci_ent_t *e = &pci_ents[i];
            char line[96];
            int w = 0;
            const char *p;
            /* "00:03.0 1022:2000 network AMD PCnet-PCI II" */
            line[w++] = '0' + e->bus / 10; line[w++] = '0' + e->bus % 10;
            line[w++] = ':';
            line[w++] = '0' + e->dev / 10; line[w++] = '0' + e->dev % 10;
            line[w++] = '.'; line[w++] = '0' + e->fn; line[w++] = ' ';
            for (int k = 12; k >= 0 && w < 88; k -= 4)
                line[w++] = "0123456789abcdef"[(e->ven >> k) & 0xF];
            line[w++] = ':';
            for (int k = 12; k >= 0 && w < 88; k -= 4)
                line[w++] = "0123456789abcdef"[(e->dev_id >> k) & 0xF];
            line[w++] = ' ';
            p = cls[e->cls < 16 ? e->cls : 0];
            while (*p && w < 88) line[w++] = *p++;
            line[w++] = ' ';
            p = pci_name(e->ven, e->dev_id);
            if (p) while (*p && w < 88) line[w++] = *p++;
            line[w] = 0;
            if (n + w + 1 >= max) break;
            n = sap(buf, max, n, line);
            n = sap(buf, max, n, "\n");
        }
        return n;
    }
    default: return -1;
    }
}

int hw_summary(char *buf, int max) {
    if (!buf || max <= 0) return -1;
    char b[128];
    int n = 0;
    n = sap(buf, max, n, "== CatOS hardware properties ==\n");

    hw_str(HW_CPU_BRAND_S, b, sizeof(b));
    n = sap(buf, max, n, "cpu.brand : ");
    n = sap(buf, max, n, b);
    n = sap(buf, max, n, "\n");
    hw_str(HW_CPU_VENDOR_S, b, sizeof(b));
    n = sap(buf, max, n, "cpu.vendor: ");
    n = sap(buf, max, n, b);
    n = sap(buf, max, n, "  sig fam ");
    n = sap_int(buf, max, n, hw_num(HW_CPU_FAMILY));
    n = sap(buf, max, n, " mod ");
    n = sap_int(buf, max, n, hw_num(HW_CPU_MODEL));
    n = sap(buf, max, n, " step ");
    n = sap_int(buf, max, n, hw_num(HW_CPU_STEPPING));
    n = sap(buf, max, n, "\n");

    hw_str(HW_CPU_FEATURES_S, b, sizeof(b));
    n = sap(buf, max, n, "cpu.flags : ");
    n = sap(buf, max, n, b);
    n = sap(buf, max, n, "\ncpu.mhz   : ");
    n = sap_int(buf, max, n, hw_num(HW_CPU_MHZ));
    n = sap(buf, max, n, "  cores ");
    n = sap_int(buf, max, n, hw_num(HW_CPU_CORES));
    n = sap(buf, max, n, "\n");

    n = sap(buf, max, n, "mem.total : ");
    n = sap_int(buf, max, n, hw_num(HW_MEM_TOTAL_KB));
    n = sap(buf, max, n, " KB  kernel ");
    n = sap_int(buf, max, n, hw_num(HW_MEM_KERNEL_KB));
    n = sap(buf, max, n, " KB\nmem.frames: used ");
    n = sap_int(buf, max, n, hw_num(HW_MEM_USED_KB) / 4);
    n = sap(buf, max, n, " / free ");
    n = sap_int(buf, max, n, hw_num(HW_MEM_FREE_KB) / 4);
    n = sap(buf, max, n, " (4K pages)\n");

    hw_str(HW_DISK_MODEL_S, b, sizeof(b));
    n = sap(buf, max, n, "disk.model: ");
    n = sap(buf, max, n, b);
    n = sap(buf, max, n, "  ");
    n = sap_int(buf, max, n, hw_num(HW_DISK_MB));
    n = sap(buf, max, n, " MB  lba48 ");
    n = sap_int(buf, max, n, hw_num(HW_DISK_LBA48));
    hw_str(HW_DISK_SERIAL_S, b, sizeof(b));
    n = sap(buf, max, n, "\ndisk.s/n  : ");
    n = sap(buf, max, n, b);
    hw_str(HW_DISK_FW_S, b, sizeof(b));
    n = sap(buf, max, n, "  fw ");
    n = sap(buf, max, n, b);
    n = sap(buf, max, n, "\n");

    hw_str(HW_NIC_MODEL_S, b, sizeof(b));
    n = sap(buf, max, n, "nic.model : ");
    n = sap(buf, max, n, b);
    hw_str(HW_NIC_MAC_S, b, sizeof(b));
    n = sap(buf, max, n, "  mac ");
    n = sap(buf, max, n, b);
    hw_str(HW_NIC_IP_S, b, sizeof(b));
    n = sap(buf, max, n, "  ip ");
    n = sap(buf, max, n, b);
    n = sap(buf, max, n, "\n");

    n = sap(buf, max, n, "pci devices:\n");
    n += hw_str(HW_PCI_LIST_S, buf + n, max - n);
    return n;
}
