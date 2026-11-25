/*
 * ahci.c -- minimal SATA/AHCI storage driver (see include/ahci.h).
 *
 * Read path first: IDENTIFY DEVICE + READ DMA EXT (LBA48).  The command
 * engine polls PxCI/PxTFD; no interrupt ISR, no MSI/X, no port multiplier.
 */

#include "ahci.h"
#include "io.h"
#include "paging.h"
#include "vga.h"
#include "serial_log.h"
#include <stdint.h>

/* ---- PCI config-space access (same 0xCF8/0xCFC mode as net.c) -------- */
static uint32_t pci_rd(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | off;
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_wr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) |
         ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | off);
    outl(0xCFC, v);
}

/* ---- HBA / port register offsets ------------------------------------- */
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10

#define PORT_BASE(p)   (0x100u + (uint32_t)(p) * 0x80u)
#define P_CLB    0x00
#define P_CLBU   0x04
#define P_FB     0x08
#define P_FBU    0x0C
#define P_IS     0x10
#define P_IE     0x14
#define P_CMD    0x18
#define P_TFD    0x20
#define P_SIG    0x24
#define P_SSTS   0x28
#define P_SCTL   0x2A
#define P_SERR   0x30
#define P_SACT   0x34
#define P_CI     0x38

#define GHC_AE   (1u << 31)
#define GHC_IE   (1u << 1)
#define GHC_HRE  (1u << 0)
#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define TFD_BSY  (1u << 7)
#define TFD_DRQ  (1u << 6)
#define TFD_ERR  (1u << 0)

#define AHCI_MAX_PORTS 32

typedef struct {
    int      present;
    uint8_t  port;
    uint32_t sectors;
    int      lba48;
    uint32_t cmd_list_pa;
    uint32_t cmd_tbl_pa;
    uint32_t fis_rx_pa;
    char     model[41];
} ahci_port_t;

static ahci_port_t g_port[AHCI_MAX_PORTS];
static int         g_count;
static volatile uint32_t *g_abar;
static uint32_t    g_abar_phys;
static int         g_hba;
static uint32_t    g_identify_pa;   /* shared 512B IDENTIFY scratch frame */

static inline uint32_t pr_rd(uint8_t p, uint8_t off) {
    return g_abar[PORT_BASE(p) / 4 + off / 4];
}
static inline void pr_wr(uint8_t p, uint8_t off, uint32_t v) {
    g_abar[PORT_BASE(p) / 4 + off / 4] = v;
}

/* ---- console printers (local, keep this driver self-contained) ------- */
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

/* ---- command structures (physical, frame-allocated) ------------------- */
/* Command header (16 B) at cmd_list + slot*16. */
typedef struct {
    uint32_t dw0;    /* CFL[4:0], W[6], PMPORT[12:8] */
    uint32_t dw1;    /* PRDTL[31:16] */
    uint32_t dw2;    /* PRDBC */
    uint32_t dw3;    /* CMDCT[31:0] */
    uint32_t dw4;    /* CMDCTU */
    uint32_t dw5, dw6, dw7;
} ahci_cmdhdr_t;

/* Command table (128 B): CFIS 64, ACMD 16+16 reserved, PRDT from 0x50. */
typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[32];
    uint32_t prd_base_lo;
    uint32_t prd_base_hi;
    uint32_t prd_reserved;
    uint32_t prd_dbc;       /* DBC[21:0], IEND bit31 */
} ahci_cmdtbl_t;

/* Fill a Register H2D FIS (0x27) for an ATA command. */
static void fill_reg_fis(uint8_t *fis, uint8_t cmd, uint8_t feature,
                         uint32_t lba, uint16_t count) {
    for (int i = 0; i < 64; i++) fis[i] = 0;
    fis[0]  = 0x27;                       /* FIS type: Host to Device Reg */
    fis[1]  = 0x80;                       /* command FIS */
    fis[2]  = cmd;
    fis[3]  = feature;
    fis[4]  = (uint8_t)lba;               /* LBA low */
    fis[5]  = (uint8_t)(lba >> 8);
    fis[6]  = (uint8_t)(lba >> 16);
    fis[7]  = 0x40;                       /* LBA mode, master */
    fis[8]  = (uint8_t)(lba >> 24);       /* LBA48 exp low */
    fis[9]  = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[11] = (uint8_t)count;
    fis[12] = (uint8_t)(count >> 8);
    fis[13] = 0;                          /* ICC */
    fis[14] = 0x02;                       /* control: nIEN=1, SRST=0 */
}

/* Dump port error state to serial: PxCI / PxTFD (status+error byte) / PxIS
 * (interrupt/error bits) / PxSERR / PxSIG.  Read before clearing anything. */
static void ahci_dump_state(uint8_t p, const char *tag) {
    uint32_t ci   = pr_rd(p, P_CI);
    uint32_t tfd  = pr_rd(p, P_TFD);
    uint32_t is   = pr_rd(p, P_IS);
    uint32_t serr = pr_rd(p, P_SERR);
    uint32_t sig  = pr_rd(p, P_SIG);
    ser_write("[ahci] "); ser_write(tag);
    ser_hex(" CI=", ci); ser_hex(" TFD=", tfd);
    ser_hex(" IS=", is); ser_hex(" SERR=", serr);
    ser_hex(" SIG=", sig); ser_write("\n");
}

/* One attempt at slot 0.  Returns 0 ok / -1 error. */
static int ahci_exec_once(ahci_port_t *pt, uint8_t cmd, uint32_t lba,
                          uint16_t count, void *buf, int write) {
    uint8_t p = pt->port;
    ahci_cmdhdr_t *hdr = (ahci_cmdhdr_t *)pt->cmd_list_pa;   /* identity */
    ahci_cmdtbl_t *tbl = (ahci_cmdtbl_t *)pt->cmd_tbl_pa;
    uint32_t buf_pa = (uint32_t)buf;

    /* wait port idle */
    for (int t = 0; t < 1000000; t++) {
        uint32_t tfd = pr_rd(p, P_TFD);
        if (!(tfd & (TFD_BSY | TFD_DRQ))) break;
    }
    if (pr_rd(p, P_TFD) & TFD_ERR) { pr_wr(p, P_IS, 0xFFFFFFFFu); return -1; }

    fill_reg_fis(tbl->cfis, cmd, 0, lba, count);

    /* one PRDT entry describing the whole transfer */
    tbl->prd_base_lo = buf_pa & 0xFFFFFFFFu;
    tbl->prd_base_hi = 0;
    tbl->prd_reserved = 0;
    tbl->prd_dbc = (uint32_t)(count * AHCI_SECTOR_SIZE) | (1u << 31);

    hdr->dw0  = (5u & 0x1F) | (write ? (1u << 5) : 0);  /* CFL=5 dwords, W=bit5 */
    hdr->dw1  = 1u << 16;                                /* PRDTL=1 */
    hdr->dw2  = count * AHCI_SECTOR_SIZE;                /* PRDBC */
    hdr->dw3  = pt->cmd_tbl_pa;
    hdr->dw4  = 0;
    hdr->dw5 = hdr->dw6 = hdr->dw7 = 0;

    pr_wr(p, P_IS, 0xFFFFFFFFu);                         /* clear status */
    pr_wr(p, P_CI, 1u << 0);                             /* issue slot 0 */

    /* poll until command collected */
    for (int t = 0; t < 10000000; t++) {
        if (!(pr_rd(p, P_CI) & 1u)) break;
    }
    uint32_t ci  = pr_rd(p, P_CI);
    uint32_t tfd = pr_rd(p, P_TFD);
    if (ci & 1u) return -1;                   /* timeout */
    if (tfd & TFD_ERR) return -1;
    return 0;
}

/* Issue one command on slot 0; on error dump state, clear SERR/IS and retry
 * once.  Returns 0 ok / -1 error. */
static int ahci_exec(ahci_port_t *pt, uint8_t cmd, uint32_t lba,
                     uint16_t count, void *buf, int write) {
    uint8_t p = pt->port;
    int rc = ahci_exec_once(pt, cmd, lba, count, buf, write);
    if (rc == 0) return 0;

    ahci_dump_state(p, "err1");
    /* clear SERR (write-1-to-clear) and IS, then a single retry */
    pr_wr(p, P_SERR, 0xFFFFFFFFu);
    pr_wr(p, P_IS,   0xFFFFFFFFu);
    for (int t = 0; t < 200000; t++)
        if (!(pr_rd(p, P_TFD) & TFD_BSY)) break;
    rc = ahci_exec_once(pt, cmd, lba, count, buf, write);
    if (rc != 0) ahci_dump_state(p, "err2");
    return rc;
}

static int ahci_identify(ahci_port_t *pt) {
    uint16_t *id = (uint16_t *)g_identify_pa;
    for (int i = 0; i < 256; i++) id[i] = 0;

    if (ahci_exec(pt, 0xEC, 0, 1, id, 0) != 0) return -1;

    uint32_t sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    pt->lba48 = (id[83] & 0x0400) ? 1 : 0;
    if (pt->lba48) {
        sectors = (uint32_t)id[100] | ((uint32_t)id[101] << 16) |
                  ((uint32_t)id[102] << 32) | ((uint32_t)id[103] << 48);
    }
    if (sectors == 0) return -1;
    pt->sectors = sectors;

    int o = 0;
    for (int w = 27; w <= 46 && o < 39; w++) {
        pt->model[o++] = (char)(id[w] >> 8);
        pt->model[o++] = (char)(id[w] & 0xFF);
    }
    pt->model[o] = 0;
    int l = 0; while (pt->model[l]) l++;
    while (l > 0 && pt->model[l-1] == ' ') pt->model[--l] = 0;
    return 0;
}

int ahci_scan(void) {
    if (g_hba) return g_count;

    /* locate the AHCI controller (class 01 / subclass 06 / prog 01) */
    uint8_t hba_bus = 0, hba_dev = 0, hba_fn = 0;
    int found = 0;
    for (int dev = 0; dev < 32 && !found; dev++) {
        for (int fn = 0; fn < 8 && !found; fn++) {
            uint32_t cls = pci_rd(0, dev, fn, 0x08) >> 8;
            uint8_t base = (uint8_t)(cls >> 16);
            uint8_t sub  = (uint8_t)(cls >> 8);
            uint8_t prog = (uint8_t)cls;
            if (base == 0x01 && sub == 0x06 && prog == 0x01) {
                hba_bus = 0; hba_dev = (uint8_t)dev; hba_fn = (uint8_t)fn;
                found = 1;
            }
        }
    }
    if (!found) { ser_write("[ahci] no AHCI controller\n"); return 0; }

    uint32_t bar5 = pci_rd(hba_bus, hba_dev, hba_fn, 0x24) & 0xFFFFFFF0u;
    if (!bar5) return 0;
    /* enable memory-space decoding */
    uint32_t cmd = pci_rd(hba_bus, hba_dev, hba_fn, 0x04);
    pci_wr(hba_bus, hba_dev, hba_fn, 0x04, cmd | 0x02u);

    g_abar_phys = bar5;
    g_abar = vbe_lfb_map(bar5, 0x1000);
    if (!g_abar) { ser_write("[ahci] ABAR map failed\n"); return 0; }

    uint32_t cap = g_abar[HBA_CAP / 4];
    uint32_t pi  = g_abar[HBA_PI / 4];
    uint32_t nports = (cap & 0x1F) + 1;
    ser_hex("[ahci] cap=", cap); ser_hex(" pi=", pi);
    ser_write(" nports="); ser_putc('0' + (char)(nports & 7)); ser_write("\n");

    /* HBA reset, then enable AHCI + interrupts. */
    g_abar[HBA_GHC / 4] = GHC_HRE;
    for (int t = 0; t < 1000000; t++)
        if (!(g_abar[HBA_GHC / 4] & GHC_HRE)) break;
    g_abar[HBA_GHC / 4] = GHC_AE | GHC_IE;

    g_identify_pa = frame_alloc();
    if (!g_identify_pa) return 0;

    g_count = 0;
    for (uint8_t p = 0; p < nports && g_count < AHCI_MAX_PORTS; p++) {
        if (!(pi & (1u << p))) continue;

        uint32_t cl = frame_alloc();
        uint32_t ct = frame_alloc();
        uint32_t fr = frame_alloc();
        if (!cl || !ct || !fr) break;

        ahci_port_t *pt = &g_port[g_count];
        pt->port = p;
        pt->cmd_list_pa = cl;
        pt->cmd_tbl_pa  = ct;
        pt->fis_rx_pa   = fr;

        pr_wr(p, P_CLB, cl & 0xFFFFFFFFu);
        pr_wr(p, P_CLBU, 0);
        pr_wr(p, P_FB,  fr & 0xFFFFFFFFu);
        pr_wr(p, P_FBU, 0);
        pr_wr(p, P_IS,  0xFFFFFFFFu);
        pr_wr(p, P_IE,  0x0000001Fu);

        uint32_t cmdreg = pr_rd(p, P_CMD);
        pr_wr(p, P_CMD, (cmdreg & ~(CMD_ST | CMD_FRE)) | CMD_FRE);
        pr_wr(p, P_CMD, pr_rd(p, P_CMD) | CMD_ST);

        /* wait PHY up */
        int up = 0;
        for (int t = 0; t < 2000000; t++) {
            if ((pr_rd(p, P_SSTS) & 0x0F) == 3u) { up = 1; break; }
        }
        ser_write("[ahci] port "); ser_putc('0' + p);
        ser_hex(" ssts=", pr_rd(p, P_SSTS));
        ser_write(" up="); ser_putc(up ? '1' : '0'); ser_write("\n");
        if (!up) continue;

        int rc = ahci_identify(pt);
        ser_write("[ahci] identify rc="); ser_putc(rc ? '1' : '0'); ser_write("\n");
        if (rc != 0) continue;

        pt->present = 1;
        g_count++;
        ser_write("[ahci] port "); ser_putc('0' + p);
        ser_hex(" sectors=", pt->sectors); ser_write("\n");
    }

    g_hba = 1;
    return g_count;
}

int ahci_count(void) { return g_count; }

uint32_t ahci_sectors(int port) {
    if (port < 0 || port >= g_count || !g_port[port].present) return 0;
    return g_port[port].sectors;
}

const char *ahci_model_str(int port) {
    if (port < 0 || port >= g_count) return "";
    return g_port[port].model;
}

static ahci_port_t *ahci_bundle(int port) {
    if (port < 0 || port >= g_count || !g_port[port].present) return 0;
    return &g_port[port];
}

int ahci_read(int port, uint32_t lba, uint8_t count, void *buf) {
    ahci_port_t *pt = ahci_bundle(port);
    if (!pt || count == 0) return -1;
    return ahci_exec(pt, 0x25, lba, count, buf, 0);   /* READ DMA EXT */
}

int ahci_write(int port, uint32_t lba, uint8_t count, const void *buf) {
    ahci_port_t *pt = ahci_bundle(port);
    if (!pt || count == 0) return -1;
    return ahci_exec(pt, 0x35, lba, count, (void *)buf, 1); /* WRITE DMA EXT */
}

void ahci_dump(void) {
    if (!g_hba && ahci_scan() == 0) { vga_write("ahci: no AHCI controller\n"); return; }
    vga_write("ahci: ABAR="); wh(g_abar_phys);
    vga_write(" drives="); wdec((uint32_t)g_count); vga_write("\n");
    for (int i = 0; i < g_count; i++) {
        ahci_port_t *pt = &g_port[i];
        vga_write("  port "); wdec(pt->port);
        vga_write("  model="); vga_write(pt->model[0] ? pt->model : "(unknown)");
        vga_write("  sectors="); wdec(pt->sectors);
        vga_write("  lba48="); vga_write(pt->lba48 ? "yes" : "no");
        vga_write("\n");
    }
}
