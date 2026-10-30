#include "ata.h"
#include "task.h"
#include "io.h"

/* PIO ATA driver with channel/device discovery.
 *
 * ata_identify() probes four IDE slots (primary/secondary bus x
 * master/slave) and remembers the first one that answers IDENTIFY
 * DEVICE with a non-zero capacity; all later reads/writes go to that
 * slot.  This makes the same kernel work under QEMU, VMware, Bochs
 * and real hardware without any configuration.
 */

#define CMD_READ       0x20
#define CMD_WRITE      0x30
#define CMD_FLUSH      0xE7
#define CMD_IDENTIFY   0xEC

/* selected slot (filled by ata_identify) */
static uint16_t g_base = 0x1F0;   /* data register */
static uint16_t g_ctrl = 0x3F6;   /* alt status / device control */
static uint8_t  g_dev  = 0xE0;    /* master, LBA mode (0xE0 master / 0xF0 slave) */

/* cached IDENTIFY strings for the hardware-info API (hw.c) */
static char g_model[41], g_serial[21], g_fw[9];
static int  g_lba48;

static void ata_select(uint32_t lba) {
    outb(g_base + 6, g_dev | ((lba >> 24) & 0x0F));   /* LBA28 hi nibble */
}

/* Wait until the drive is not busy. */
static void ata_wait_ready(void) {
    for (int t = 0; t < 10000000; t++) {
        if (!(inb(g_base + 7) & 0x80)) break;          /* clear BSY */
        task_maybe_yield();                /* let other tasks run */
    }
}

/* Wait for DRQ (data transfer ready); -1 on error/timeout, 0 on ready. */
static int ata_wait_drq(void) {
    for (int t = 0; t < 10000000; t++) {
        uint8_t st = inb(g_base + 7);
        if (st & 0x80) { task_maybe_yield(); continue; }  /* still busy */
        if (st & 0x01) return -1;                    /* error */
        if (st & 0x08) return 0;                     /* DRQ: data ready */
    }
    return -1;
}

/* Try IDENTIFY on one slot.  Returns 0 + *sectors on success. */
static int ata_try_identify(uint16_t base, uint16_t ctrl,
                            uint8_t dev, uint32_t *sectors) {
    uint8_t st;

    /* wait for the bus to be idle before touching the slot */
    for (int t = 0; t < 10000000; t++) {
        st = inb(base + 7);
        if (!(st & 0x80)) break;
    }

    outb(base + 6, dev);                    /* select device */
    for (int i = 0; i < 4; i++) inb(ctrl);  /* 400ns settle */
    outb(base + 2, 0);                      /* sector count = 0 */
    outb(base + 3, 0);                      /* LBA low  = 0 */
    outb(base + 4, 0);                      /* LBA mid  = 0 */
    outb(base + 5, 0);                      /* LBA high = 0 */
    outb(base + 7, CMD_IDENTIFY);

    int got = 0;
    for (int t = 0; t < 10000000; t++) {
        st = inb(base + 7);
        if (st & 0x80) continue;            /* busy */
        if (st & 0x01) break;               /* error: no device here */
        if (st & 0x08) { got = 1; break; }  /* DRQ: data ready */
    }
    if (!got) return -1;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(base);

    /* POST-IDENTIFY: read status so the controller leaves the
     * command-complete state; the next command may be rejected otherwise. */
    (void)inb(base + 7);

    uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (lba28 == 0) {                                /* fall back to LBA48 */
        uint64_t lba48 = (uint64_t)id[100]
                       | ((uint64_t)id[101] << 16)
                       | ((uint64_t)id[102] << 32)
                       | ((uint64_t)id[103] << 48);
        /* the driver issues 28-bit LBA commands only, so the honest
         * reportable maximum is 0x0FFFFFFF sectors (128 GiB) - the old
         * 0xFFFFFFFF clamp advertised space it could not address */
        lba28 = (lba48 > 0x0FFFFFFFu) ? 0x0FFFFFFFu : (uint32_t)lba48;
    }
    if (lba28 == 0) return -1;
    if (sectors) *sectors = lba28;

    /* cache the descriptive strings for the hardware-info API:
     * every string word is byte-swapped (high byte printed first). */
    {
        static uint16_t *idcache;
        idcache = id;                    /* keep the parse close to the data */
        int o = 0;
        for (int w = 27; w <= 46 && o < 39; w++) {
            g_model[o++] = (char)(id[w] >> 8);
            g_model[o++] = (char)(id[w] & 0xFF);
        }
        g_model[o] = 0;
        o = 0;
        for (int w = 10; w <= 19 && o < 19; w++) {
            g_serial[o++] = (char)(id[w] >> 8);
            g_serial[o++] = (char)(id[w] & 0xFF);
        }
        g_serial[o] = 0;
        o = 0;
        for (int w = 23; w <= 26 && o < 7; w++) {
            g_fw[o++] = (char)(id[w] >> 8);
            g_fw[o++] = (char)(id[w] & 0xFF);
        }
        g_fw[o] = 0;
        g_lba48 = (id[83] & 0x0400) ? 1 : 0;   /* word 83 bit 10 */
        (void)idcache;
        /* trim trailing spaces on model/serial */
        for (int which = 0; which < 2; which++) {
            char *s = which ? g_serial : g_model;
            int l = 0; while (s[l]) l++;
            while (l > 0 && s[l - 1] == ' ') s[--l] = 0;
        }
    }
    return 0;
}

const char *ata_model_str(void)   { return g_model; }
const char *ata_serial_str(void)  { return g_serial; }
const char *ata_fw_str(void)      { return g_fw; }
int         ata_lba48_sup(void)   { return g_lba48; }

/* Probe all four IDE slots; remember the first working one. */
int ata_identify(uint32_t *sectors) {
    /* identify_dev selects without LBA bit (0xA0/0xB0, traditional for
     * IDENTIFY); lba_dev is what subsequent reads/writes must use. */
    static const struct slot { uint16_t base, ctrl; uint8_t identify_dev, lba_dev; } slots[4] = {
        { 0x1F0, 0x3F6, 0xA0, 0xE0 },   /* primary   master */
        { 0x1F0, 0x3F6, 0xB0, 0xF0 },   /* primary   slave  */
        { 0x170, 0x376, 0xA0, 0xE0 },   /* secondary master */
        { 0x170, 0x376, 0xB0, 0xF0 },   /* secondary slave  */
    };
    for (int i = 0; i < 4; i++) {
        uint32_t sec = 0;
        if (ata_try_identify(slots[i].base, slots[i].ctrl,
                             slots[i].identify_dev, &sec) == 0 && sec > 0) {
            g_base = slots[i].base;
            g_ctrl = slots[i].ctrl;
            g_dev  = slots[i].lba_dev;   /* LBA mode for data commands */
            if (sectors) *sectors = sec;
            return 0;
        }
    }
    return -1;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    if (count == 0) return -1;

    ata_wait_ready();
    ata_select(lba);
    outb(g_base + 2, count);
    outb(g_base + 3, (uint8_t)lba);
    outb(g_base + 4, (uint8_t)(lba >> 8));
    outb(g_base + 5, (uint8_t)(lba >> 16));
    outb(g_base + 7, CMD_READ);

    for (int s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) {

        }
        uint16_t *w = (uint16_t *)(p + s * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++) w[i] = inw(g_base);
    }
    ata_wait_ready();
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    if (count == 0) return -1;

    ata_wait_ready();
    ata_select(lba);
    outb(g_base + 2, count);
    outb(g_base + 3, (uint8_t)lba);
    outb(g_base + 4, (uint8_t)(lba >> 8));
    outb(g_base + 5, (uint8_t)(lba >> 16));
    outb(g_base + 7, CMD_WRITE);

    for (int s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) return -1;
        const uint16_t *w = (const uint16_t *)(p + s * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++) outw(g_base, w[i]);
    }
    outb(g_base + 7, CMD_FLUSH);                       /* flush write cache */
    ata_wait_ready();
    return 0;
}
