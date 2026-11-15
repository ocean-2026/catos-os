#ifndef PART_H
#define PART_H

#include <stdint.h>

/*
 * P2: MBR partition-table parser.  Scans every IDE drive found by the ATA
 * layer, parses the four primary entries at LBA 0, and exposes the
 * FAT32 volumes (types 0x0B / 0x0C) to the VFS mount layer.
 *
 * Extended partitions (0x05 / 0x0F / 0x85 / 0xEE) are recorded but
 * flagged unsupported: CatOS does not traverse EBR chains in this
 * iteration.
 */

#define PART_MAX 16

typedef struct {
    uint8_t  drive;       /* ATA drive index 0..3 */
    uint8_t  entry;       /* MBR primary entry 0..3 */
    uint8_t  type;        /* partition type byte */
    uint8_t  active;      /* boot flag 0x80 */
    uint32_t start;       /* LBA start */
    uint32_t size;        /* sector count */
    int      fat32;       /* 1: type 0x0B/0x0C recognised as FAT32 */
    int      extended;    /* 1: extended partition, unsupported */
} part_t;

/* Scan all present drives; fill the internal partition table.  Must run
 * after ata_scan().  Returns the number of usable (non-empty) entries. */
int  part_scan(void);
int  part_count(void);
const part_t *part_get(int i);

/* Find a partition by user-facing spec:
 *   "hd<d>p<e>" (e = 1..4) or "<e>" (drive 0 entry e, 1-based).
 * Returns the table index, or -1 if not found / bad spec. */
int  part_lookup(const char *spec);
/* Human device name, e.g. "hd0p1"; writes into buf (>= 16 bytes). */
void part_devname(const part_t *p, char *buf, int bufsz);

#endif
