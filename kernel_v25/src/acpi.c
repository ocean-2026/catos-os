/*
 * acpi.c -- minimal ACPI table discovery (see include/acpi.h).
 *
 * Read-only fixed-table walk.  No AML interpretation, no runtime ACPI
 * objects.  All physical reads are guarded by paging_phys_top() so a
 * malformed firmware table degrades to "ACPI absent" instead of a #PF.
 */

#include "acpi.h"
#include "paging.h"
#include "vga.h"
#include "serial_log.h"

static int      have_acpi;
static uint32_t rsdp_phys, fadt_phys, madt_phys;
static uint32_t madt_lapic_addr;

static acpi_cpu_t     cpus[ACPI_MAX_CPU];
static int            n_cpu;
static acpi_ioapic_t  ioapics[ACPI_MAX_IOAPIC];
static int            n_ioapic;
static acpi_iso_t     isos[ACPI_MAX_ISO];
static int            n_iso;

static uint16_t reset_port;
static uint8_t  reset_value;
static int      have_reset;

/* ---- physical-access guard ------------------------------------------- */
static int phys_ok(uint32_t p, uint32_t len) {
    uint32_t top = paging_map_top();
    if (!top || p == 0) return 0;
    if (p >= top) return 0;
    if (len > top || p > top - len) return 0;
    return 1;
}

static const uint8_t *phys_ptr(uint32_t p) { return (const uint8_t *)p; }

static int sig_eq(const uint8_t *p, const char *s) {
    return p[0]==s[0] && p[1]==s[1] && p[2]==s[2] && p[3]==s[3];
}

/* ---- RSDP discovery --------------------------------------------------- */
static uint32_t find_rsdp(void) {
    /* 1) first KiB of the EBDA (legacy BIOS pointer at 0x40E) */
    uint32_t ebda = (uint32_t)(*(const uint16_t *)0x40E) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000 && phys_ok(ebda, 1024)) {
        for (uint32_t off = 0; off < 1024; off += 16) {
            const uint8_t *p = phys_ptr(ebda + off);
            if (p[0]=='R'&&p[1]=='S'&&p[2]=='D'&&p[3]==' '&&
                p[4]=='P'&&p[5]=='T'&&p[6]=='R'&&p[7]==' ') return ebda + off;
        }
    }
    /* 2) ACPI extended BIOS area 0xE0000..0xFFFFF */
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        if (!phys_ok(a, 8)) break;
        const uint8_t *p = phys_ptr(a);
        if (p[0]=='R'&&p[1]=='S'&&p[2]=='D'&&p[3]==' '&&
            p[4]=='P'&&p[5]=='T'&&p[6]=='R'&&p[7]==' ') return a;
    }
    return 0;
}

/* Walk RSDT (1.0) or XSDT (2.0+) for a table with `sig`. */
static uint32_t find_table(const char *sig) {
    if (!rsdp_phys || !phys_ok(rsdp_phys, 36)) return 0;
    const uint8_t *r = phys_ptr(rsdp_phys);
    uint8_t rev = r[15];
    uint32_t rsdt = 0;
    if (rev >= 2) {
        uint32_t x = *(const uint32_t *)(r + 24);
        if (phys_ok(x, 8)) rsdt = x;
    }
    if (!rsdt) {
        uint32_t x = *(const uint32_t *)(r + 16);
        if (phys_ok(x, 8)) rsdt = x;
    }
    if (!rsdt) return 0;

    const uint8_t *t = phys_ptr(rsdt);
    uint32_t len = *(const uint32_t *)(t + 4);
    if (len < 8 || len > 0x10000 || !phys_ok(rsdt, len)) return 0;
    int is_x = (rev >= 2 && rsdt == *(const uint32_t *)(r + 24));
    uint32_t n = (len - 36) / (is_x ? 8 : 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ent;
        if (is_x) ent = *(const uint32_t *)(t + 36 + i * 8);
        else      ent = *(const uint32_t *)(t + 36 + i * 4);
        if (!phys_ok(ent, 8)) continue;
        const uint8_t *e = phys_ptr(ent);
        if (sig_eq(e, sig)) return ent;
    }
    return 0;
}

/* ---- MADT parse -------------------------------------------------------- */
static void parse_madt(uint32_t madt) {
    if (!phys_ok(madt, 44)) return;
    const uint8_t *m = phys_ptr(madt);
    uint32_t len = *(const uint32_t *)(m + 4);
    if (len < 44 || len > 0x20000 || !phys_ok(madt, len)) return;

    madt_lapic_addr = *(const uint32_t *)(m + 36);

    uint32_t off = 44;
    while (off + 2 < len) {
        uint8_t type = m[off];
        uint8_t elen = m[off + 1];
        if (elen < 2 || off + elen > len) break;

        if (type == 0 && n_cpu < ACPI_MAX_CPU) {
            /* Processor Local APIC: proc_id u8, apic_id u8, flags u32 */
            if (elen >= 8) {
                cpus[n_cpu].proc_id = m[off + 2];
                cpus[n_cpu].apic_id = m[off + 3];
                cpus[n_cpu].flags   = *(const uint32_t *)(m + off + 4);
                n_cpu++;
            }
        } else if (type == 1 && n_ioapic < ACPI_MAX_IOAPIC) {
            /* IO APIC: id u8, rsvd u8, addr u32, irq_base u32 */
            if (elen >= 12) {
                ioapics[n_ioapic].ioapic_id = m[off + 2];
                ioapics[n_ioapic].addr      = *(const uint32_t *)(m + off + 4);
                ioapics[n_ioapic].irq_base  = *(const uint32_t *)(m + off + 8);
                n_ioapic++;
            }
        } else if (type == 2 && n_iso < ACPI_MAX_ISO) {
            /* Interrupt Source Override: bus u8, source u8, global u32, flags u16 */
            if (elen >= 10) {
                isos[n_iso].bus        = m[off + 2];
                isos[n_iso].source_irq = m[off + 3];
                isos[n_iso].global_irq = *(const uint32_t *)(m + off + 4);
                isos[n_iso].flags      = *(const uint16_t *)(m + off + 8);
                n_iso++;
            }
        }
        off += elen;
    }
}

/* ---- FADT reset block -------------------------------------------------- */
static void parse_fadt(uint32_t fadt) {
    if (!phys_ok(fadt, 0x40)) return;
    const uint8_t *f = phys_ptr(fadt);
    /* FADT +0x38: RESET_REG GAS (space u8, width u8, offset u8, size u8,
     * address u64); +0x40: RESET_VALUE u8.  Only a pure I/O block is usable
     * with outb; memory-space reset needs an MMIO write this kernel does not
     * wire up yet. */
    uint8_t space = f[0x38];
    if (space == 1) {
        uint32_t addr = *(const uint32_t *)(f + 0x3C);
        if (addr <= 0xFFFF) {
            reset_port  = (uint16_t)addr;
            reset_value = f[0x40];
            have_reset = 1;
        }
    }
}

static void ser_dec(uint32_t v);

int acpi_init(void) {
    if (have_acpi) return 1;
    rsdp_phys = find_rsdp();
    if (!rsdp_phys) return 0;
    fadt_phys = find_table("FACP");
    if (!fadt_phys) return 0;
    madt_phys = find_table("APIC");
    if (madt_phys) parse_madt(madt_phys);
    parse_fadt(fadt_phys);
    have_acpi = 1;
    ser_hex("[acpi] rsdp=", rsdp_phys);
    ser_hex("fadt=", fadt_phys);
    ser_hex("madt=", madt_phys);
    ser_write("cpus="); ser_dec((uint32_t)n_cpu); ser_write("\n");
    return 1;
}

int acpi_present(void) { return have_acpi; }
int acpi_cpu_count(void) { return n_cpu; }
const acpi_cpu_t *acpi_cpu(int i) {
    return (i >= 0 && i < n_cpu) ? &cpus[i] : 0;
}
int acpi_ioapic_count(void) { return n_ioapic; }
const acpi_ioapic_t *acpi_ioapic(int i) {
    return (i >= 0 && i < n_ioapic) ? &ioapics[i] : 0;
}
int acpi_iso_count(void) { return n_iso; }
const acpi_iso_t *acpi_iso(int i) {
    return (i >= 0 && i < n_iso) ? &isos[i] : 0;
}
uint32_t acpi_madt_lapic_addr(void) { return madt_lapic_addr; }
uint32_t acpi_rsdp_phys(void) { return rsdp_phys; }
uint32_t acpi_fadt_phys(void) { return fadt_phys; }

int acpi_reset_block(uint16_t *port, uint8_t *value) {
    if (!have_reset) return 0;
    if (port) *port = reset_port;
    if (value) *value = reset_value;
    return 1;
}

static void wh(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char b[11]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 8; i++) b[2+i] = h[(v >> (28 - i*4)) & 0xF];
    b[10] = 0; vga_write(b);
}

static void wdec(uint32_t v) {
    char b[12]; int i = 11; b[i--] = 0;
    if (!v) { vga_write("0"); return; }
    while (v && i >= 0) { b[i--] = '0' + (v % 10); v /= 10; }
    vga_write(b + i + 1);
}

static void ser_dec(uint32_t v) {
    char b[12]; int i = 11; b[i--] = 0;
    if (!v) { ser_putc('0'); return; }
    while (v && i >= 0) { b[i--] = '0' + (v % 10); v /= 10; }
    for (char *p = b + i + 1; *p; p++) ser_putc(*p);
}

void acpi_dump(void) {
    if (!acpi_init()) { vga_write("acpi: no ACPI tables found\n"); return; }
    vga_write("acpi: RSDP="); wh(rsdp_phys);
    vga_write(" FADT="); wh(fadt_phys);
    vga_write(" MADT="); wh(madt_phys); vga_write("\n");

    vga_write("  local APIC addr="); wh(madt_lapic_addr); vga_write("\n");

    vga_write("  CPUs: ");
    for (int i = 0; i < n_cpu; i++) {
        vga_write(i ? ", " : "");
        vga_write("id="); wdec(cpus[i].apic_id);
        vga_write(" en="); wdec(cpus[i].flags & 1);
    }
    if (!n_cpu) vga_write("(none in MADT)");
    vga_write("\n");

    for (int i = 0; i < n_ioapic; i++) {
        vga_write("  IOAPIC "); wdec(ioapics[i].ioapic_id);
        vga_write(" addr="); wh(ioapics[i].addr);
        vga_write(" base="); wdec(ioapics[i].irq_base); vga_write("\n");
    }
    for (int i = 0; i < n_iso; i++) {
        vga_write("  IRQ override "); wdec(isos[i].source_irq);
        vga_write(" -> "); wdec(isos[i].global_irq); vga_write("\n");
    }
    if (have_reset) {
        vga_write("  reset: port="); wdec(reset_port);
        vga_write(" value="); wdec(reset_value); vga_write("\n");
    } else {
        vga_write("  reset: no I/O reset block in FADT\n");
    }
}
