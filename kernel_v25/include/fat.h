#ifndef FAT_H
#define FAT_H

#include <stdint.h>

/*
 * CatOS FAT32 on-disk layer over the ATA driver.
 *
 * The volume geometry is NOT hard-coded: the drive size comes from ATA
 * IDENTIFY and fat_mkfs() picks sectors/cluster (SPC) and the FAT size
 * from the actual capacity.  The whole FAT #1 is cached in RAM.
 *
 * Volume layout (partition starts at LBA 1, right after the MBR):
 *   LBA 0          MBR (one 0x0C partition)
 *   LBA 1          FAT32 DBR (BPB)
 *   LBA 2          FSInfo
 *   LBA 3..31      reserved (DBR backup at LBA 7)
 *   LBA 33..       FAT #1 (FATSz32 sectors)
 *   ...            FAT #2
 *   ...            data area, cluster 2 = root directory
 *
 * Only 8.3 short names are supported (stored upper-case); LFN entries
 * are skipped when reading.
 */

#define FAT_CLUSTER_SZ_MAX (64u * 512u)        /* 32 KiB, max with SPC=64 */
#define FAT_CACHE_MAX      (4u * 1024 * 1024)  /* FAT #1 cache cap */

#define FAT_EOC        0x0FFFFFFF
#define FAT_ATTR_DIR   0x10
#define FAT_ATTR_FILE  0x20

typedef struct {
    uint8_t  name[11];      /* 8.3 padded with spaces */
    uint8_t  attr;
    uint32_t cluster;       /* first cluster */
    uint32_t size;
} fat_dirent_t;

/* probe the drive; returns 0 if a FAT32 volume is ready, -1 otherwise.
 * On success caches the FAT table and geometry. */
int  fat_probe(void);
/* build a fresh FAT32 volume (size from ATA IDENTIFY) and standard
 * top-level directories. */
void fat_mkfs(void);

uint32_t fat_root(void);                    /* root directory cluster */
uint32_t fat_cluster_size(void);            /* bytes per cluster */
uint32_t fat_disk_sectors(void);            /* total sectors on the drive */

/* convert a user name ("note", "a.txt") to an 8.3 padded 11-byte name */
void fat_name83(const char *name, uint8_t out[11]);
/* render an 8.3 name back to a human string (lower-case) */
void fat_name_show(const uint8_t *n83, char *out);

/* directory ops */
int  fat_lookup(uint32_t dirclus, const uint8_t *name83, fat_dirent_t *out);
int  fat_mkdir(uint32_t parent, const uint8_t *name83);   /* 0 ok */
int  fat_mkfile(uint32_t parent, const uint8_t *name83);  /* 0 ok */
/* update size (and cluster) of an existing entry in dir */
int  fat_update_entry(uint32_t dirclus, const uint8_t *name83,
                      uint32_t cluster, uint32_t size);

/* file data */
int  fat_read_file(uint32_t clus, uint32_t size, char *out);
/* rewrite a file: reuses cluster chain, extends with new clusters as needed */
int  fat_write_file(uint32_t clus, const char *data, uint32_t size);
void fat_free_chain(uint32_t clus);
/* v2.5: append data after old_size in the existing chain (no full read-back) */
int  fat_append_file(uint32_t clus, uint32_t old_size,
                     const char *data, uint32_t size);
/* v2.5: mark the directory entry deleted (0xE5) */
int  fat_delete_entry(uint32_t dirclus, const uint8_t *name83);

/* cluster / FAT access (also used for directory walking) */
uint32_t fat_get(uint32_t clus);
uint32_t fat_alloc(void);
int  fat_read_cluster(uint32_t clus, uint8_t *out);
int  fat_write_cluster(uint32_t clus, const uint8_t *buf);

/* crash-consistency: verify FAT #2 mirrors FAT #1, repair #2 from #1.
 * returns sectors repaired, -1 on ATA error; fat_mirror_last keeps the
 * last result for the shell's fsck command (-2 = not run yet). */
extern int fat_mirror_last;
int  fat_verify_mirror(void);

#endif
