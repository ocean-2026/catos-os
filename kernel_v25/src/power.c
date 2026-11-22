/*
 * power.c -- ACPI power-off for CatOS (v2.67).
 * See include/power.h for why APM INT 15h was removed.
 */
#include "power.h"
#include "io.h"
#include "paging.h"

static uint32_t rsdp_phys;
static uint32_t fadt_phys;
static uint32_t pm1a_cnt, pm1b_cnt;
static uint16_t smi_cmd;        /* FADT SMI_CMD port   */
static uint8_t  acpi_enable_v;  /* value meaning "enable ACPI" */
static int      have_acpi;
static const char *last_method = "none";

/* ---- physical-access guard --------------------------------------------
 * The kernel identity-maps installed RAM with 4 MiB PSE pages, so a
 * physical address is directly readable ONLY below paging_phys_top().
 * ACPI tables live in RAM, but a bogus/partial pointer must never be
 * dereferenced: that is exactly the "trust nothing" rule. */
static int phys_ok(uint32_t p, uint32_t len) {
    uint32_t top = paging_phys_top();
    if (!top) return 0;
    if (p == 0) return 0;
    if (p >= top) return 0;
    if (len > top || p > top - len) return 0;
    return 1;
}

static const uint8_t *phys_ptr(uint32_t p) { return (const uint8_t *)p; }

/* ---- RSDP discovery --------------------------------------------------- */
static uint32_t find_rsdp(void) {
    /* 1) the first 1 KiB of the EBDA, which is where a legacy BIOS puts it */
    uint32_t ebda = (uint32_t)(*(const uint16_t *)0x40E) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000 && phys_ok(ebda, 1024)) {
        for (uint32_t off = 0; off < 1024; off += 16) {
            const uint8_t *p = phys_ptr(ebda + off);
            if (p[0]=='R'&&p[1]=='S'&&p[2]=='D'&&p[3]==' '&&
                p[4]=='P'&&p[5]=='T'&&p[6]=='R'&&p[7]==' ') return ebda + off;
        }
    }
    /* 2) the ACPI search area 0xE0000..0xFFFFF */
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        if (!phys_ok(a, 8)) break;
        const uint8_t *p = phys_ptr(a);
        if (p[0]=='R'&&p[1]=='S'&&p[2]=='D'&&p[3]==' '&&
            p[4]=='P'&&p[5]=='T'&&p[6]=='R'&&p[7]==' ') return a;
    }
    return 0;
}

/* Walk RSDT (ACPI 1.0) or XSDT (2.0+) looking for a table with `sig`. */
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
        if (e[0]==sig[0]&&e[1]==sig[1]&&e[2]==sig[2]&&e[3]==sig[3])
            return ent;
    }
    return 0;
}

/* ---- SLP_TYPa from the \_S5 object -----------------------------------
 * Decoding a DSDT AML package is genuinely fiddly.  We refuse to run an
 * AML interpreter in the shutdown path; instead we scan the DSDT bytes
 * for the `_S5_` name and the first small integer that follows it, which
 * is exactly the form every real firmware uses:
 *
 *     Name(_S5_, Package() { 0x05, 0x05, 0x00, 0x00 })
 *                          ^ SLP_TYPa
 *
 * A miss falls back to the standardized 5.  A wrong value is safe: SLP_EN
 * is only acted on when the type matches, and we try 0..7 in order. */
static int s5_slp_typa(void) {
    uint32_t dsdt = 0;
    if (fadt_phys && phys_ok(fadt_phys, 44))
        dsdt = *(const uint32_t *)(phys_ptr(fadt_phys) + 40);
    if (!dsdt || !phys_ok(dsdt, 36)) return 5;
    uint32_t len = *(const uint32_t *)(phys_ptr(dsdt) + 4);
    if (len < 36 || len > 0x20000 || !phys_ok(dsdt, len)) return 5;
    const uint8_t *p = phys_ptr(dsdt);
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (p[i]=='_'&&p[i+1]=='S'&&p[i+2]=='5'&&p[i+3]=='_') {
            for (uint32_t j = i + 4; j + 8 < len; j++) {
                /* PackageOp 0x12, then a small-int opcode */
                if (p[j] == 0x12) {
                    for (uint32_t k = j + 1; k < len && k < j + 64; k++) {
                        if (p[k] == 0x0A && k + 1 < len)
                            return (int)p[k + 1];      /* BytePrefix 0x0A */
                        if (p[k] == 0x00 || p[k] == 0x01)  /* ZeroOp / OneOp */
                            return (p[k] == 0x00) ? 0 : 1;
                    }
                    break;
                }
            }
        }
    }
    return 5;
}

/* ---- ACPI setup -------------------------------------------------------- */
static int acpi_probe(void) {
    if (have_acpi) return 1;
    rsdp_phys = find_rsdp();
    if (!rsdp_phys) return 0;
    fadt_phys = find_table("FACP");
    if (!fadt_phys) return 0;
    if (!phys_ok(fadt_phys, 0x50)) return 0;

    const uint8_t *f = phys_ptr(fadt_phys);
    uint32_t len = *(const uint32_t *)(f + 4);
    if (len < 0x50) return 0;                 /* need PM1a/PM1b CNT fields */
    pm1a_cnt = *(const uint32_t *)(f + 0x48);
    pm1b_cnt = *(const uint32_t *)(f + 0x4C);
    /* v2.76: FADT SMI_CMD (0x30) + ACPI_ENABLE (0x34).  ACPI 2.0 firmware
     * boots in LEGACY mode (PM1_CNT.SCI_EN clear) and IGNORES PM1_CNT
     * writes until the OS asks for ACPI mode; that is why a VMware guest
     * "shutdown" fell through to the reboot fallback while QEMU (which
     * ships ACPI already enabled) worked. */
    smi_cmd = (uint16_t)(*(const uint16_t *)(f + 0x30));
    if (len > 0x34) acpi_enable_v = f[0x34];
    /* GAS structures: only a pure I/O-space block is usable with outw */
    if (!pm1a_cnt || pm1a_cnt > 0xFFFF) return 0;

    have_acpi = 1;
    return 1;
}

int power_has_acpi(void) { return acpi_probe(); }

/* ---- the actual power-off --------------------------------------------- */
static void io_delay(void) {
    /* port 0x80 is the classic POST diagnostic latch: a dummy write costs
     * roughly 1 us and needs no timer. */
    for (int i = 0; i < 2000; i++) outb(0x80, 0);
}

/* Ask the firmware for ACPI mode; returns 1 when SCI_EN is set. */
static int acpi_enable_mode(void) {
    uint16_t pm = (uint16_t)(pm1a_cnt);
    uint16_t v = inw(pm);
    if (v & 0x0001) return 1;                  /* SCI_EN already set */
    if (smi_cmd && smi_cmd != 0xFFFF && acpi_enable_v) {
        outb(smi_cmd, acpi_enable_v);
        for (int i = 0; i < 100000; i++) {
            if (inw(pm) & 0x0001) return 1;
            io_delay();
        }
    }
    return 0;
}

int power_shutdown(void) {
    if (!acpi_probe()) return -1;

    acpi_enable_mode();
    int typa = s5_slp_typa();
    /* v2.76: write ONE value -- the firmware's \_S5 SLP_TYP -- instead of
     * sweeping 0..7.  Writing SLP_EN with an unrelated SLP_TYP is not a
     * harmless no-op on every chipset: some decode a wrong type as a
     * platform reset, which is exactly the "shutdown restarts the machine"
     * report.  Only if the correct value provably does nothing do we try
     * the standardized 5, then 7 (a few boards use 7). */
    int tries[3] = { typa, 5, 7 };
    for (int i = 0; i < 3; i++) {
        int t = tries[i];
        if (i > 0 && t == tries[0]) continue;
        uint16_t v = (uint16_t)(((t & 7) << 10) | (1u << 13));   /* SLP_EN */
        outw((uint16_t)pm1a_cnt, v);
        if (pm1b_cnt && pm1b_cnt <= 0xFFFF) outw((uint16_t)pm1b_cnt, v);
        for (int k = 0; k < 64; k++) io_delay();               /* >= 30 us */
    }
    last_method = "acpi-pm1";
    return 0;
}

/* Used when ACPI is absent: the fixed "power off" ports older emulators
 * still honour.  Returns 0 when at least one was written. */
int power_shutdown_ports(void) {
    outw(0x604, 0x2000);      /* QEMU/Bochs ACPI-less PM1a */
    outw(0xB004, 0x2000);     /* Bochs older              */
    outw(0x4004, 0x3400);     /* VirtualBox               */
    io_delay();
    last_method = "legacy-port";
    return 0;
}

const char *power_last_method(void) { return last_method; }
