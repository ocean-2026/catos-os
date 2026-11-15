#include "part.h"
#include "ata.h"

/* P2: MBR partition-table parser over the multi-drive ATA layer. */

static part_t g_part[PART_MAX];
static int    g_npart = 0;
static int    g_scanned = 0;

static int is_fat32_type(uint8_t t) { return t == 0x0B || t == 0x0C; }
static int is_extended_type(uint8_t t) {
    return t == 0x05 || t == 0x0F || t == 0x85 || t == 0xEE || t == 0xC5;
}

static int pnum(const char *s, int *out) {
    int n = 0;
    if (!s || !*s) return -1;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    if (*s) return -1;
    *out = n;
    return 0;
}

int part_scan(void) {
    if (g_scanned) return g_npart;
    g_npart = 0;

    int nd = ata_drive_count();
    uint8_t mbr[512];
    for (int d = 0; d < nd && g_npart < PART_MAX; d++) {
        if (ata_read(d, 0, 1, mbr) != 0) continue;
        if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) continue;  /* no MBR */
        for (int e = 0; e < 4 && g_npart < PART_MAX; e++) {
            const uint8_t *en = mbr + 446 + e * 16;
            uint8_t type = en[4];
            if (type == 0x00) continue;
            uint32_t start = (uint32_t)en[8]
                           | ((uint32_t)en[9] << 8)
                           | ((uint32_t)en[10] << 16)
                           | ((uint32_t)en[11] << 24);
            uint32_t size = (uint32_t)en[12]
                          | ((uint32_t)en[13] << 8)
                          | ((uint32_t)en[14] << 16)
                          | ((uint32_t)en[15] << 24);
            if (size == 0) continue;
            part_t *p = &g_part[g_npart];
            p->drive   = (uint8_t)d;
            p->entry   = (uint8_t)e;
            p->type    = type;
            p->active  = en[0];
            p->start   = start;
            p->size    = size;
            p->fat32   = is_fat32_type(type);
            p->extended = is_extended_type(type);
            g_npart++;
        }
    }
    g_scanned = 1;
    return g_npart;
}

int part_count(void) { return g_scanned ? g_npart : part_scan(); }

const part_t *part_get(int i) {
    if (i < 0 || i >= g_npart) return 0;
    return &g_part[i];
}

int part_lookup(const char *spec) {
    if (!spec || !*spec) return -1;
    /* hd<d>p<e> form */
    if (spec[0] == 'h' && spec[1] == 'd') {
        int di = 2;
        int drive = 0;
        while (spec[di] >= '0' && spec[di] <= '9') {
            drive = drive * 10 + (spec[di] - '0'); di++;
        }
        if (spec[di] != 'p') return -1;
        di++;
        int entry = 0;
        while (spec[di] >= '0' && spec[di] <= '9') {
            entry = entry * 10 + (spec[di] - '0'); di++;
        }
        if (spec[di] || entry < 1 || entry > 4) return -1;
        for (int i = 0; i < g_npart; i++) {
            if (g_part[i].drive == (uint8_t)drive &&
                g_part[i].entry == (uint8_t)(entry - 1)) return i;
        }
        return -1;
    }
    /* plain number: entry on drive 0, 1-based */
    int entry;
    if (pnum(spec, &entry) != 0 || entry < 1 || entry > 4) return -1;
    for (int i = 0; i < g_npart; i++) {
        if (g_part[i].drive == 0 &&
            g_part[i].entry == (uint8_t)(entry - 1)) return i;
    }
    return -1;
}

void part_devname(const part_t *p, char *buf, int bufsz) {
    if (!buf || bufsz < 12) return;
    int o = 0;
    buf[o++] = 'h'; buf[o++] = 'd';
    buf[o++] = (char)('0' + p->drive);
    buf[o++] = 'p';
    int e = p->entry + 1;
    if (e >= 10) buf[o++] = (char)('0' + e / 10);
    buf[o++] = (char)('0' + (e % 10));
    buf[o] = 0;
}
