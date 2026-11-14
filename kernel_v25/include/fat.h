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
 * v2.18: VFAT long file names are supported for reading and writing;
 * names that survive the 8.3 conversion losslessly stay plain short
 * entries (byte-identical layout to pre-v2.18 volumes).  Long names
 * get a unique BODY~N.EXT alias plus a run of LFN entries.
 */

#define FAT_CLUSTER_SZ_MAX (64u * 512u)        /* 32 KiB, max with SPC=64 */
#define FAT_CACHE_MAX      (4u * 1024 * 1024)  /* FAT #1 cache cap */

#define FAT_EOC        0x0FFFFFFF
/* Standard FAT32 attribute bits.  FAT_ATTR_RO is the read-only bit:
 * the FAT32 equivalent of a POSIX read-only file and the only
 * protection that also holds for uid 0 -- fs_perm_check() short-circuits
 * to "allowed" for root, so the ACL alone cannot keep the kernel's own
 * built-in apps from being replaced (H8). */
#define FAT_ATTR_RO    0x01
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
int  fat_mkfs(void);            /* v2.32: -1 = no IDE disk */

uint32_t fat_root(void);                    /* root directory cluster */
uint32_t fat_cluster_size(void);            /* bytes per cluster */
uint32_t fat_disk_sectors(void);            /* total sectors on the drive */
uint32_t fat_total_clusters(void);          /* v2.34: volume stats for df */
uint32_t fat_free_clusters(void);           /* scan FAT for zero entries  */

/* convert a user name ("note", "a.txt") to an 8.3 padded 11-byte name */
void fat_name83(const char *name, uint8_t out[11]);
/* render an 8.3 name back to a human string (lower-case) */
void fat_name_show(const uint8_t *n83, char *out);

/* ---- v2.18 long-name API ---- */
#ifndef FS_LFN_MAX
#define FS_LFN_MAX     96      /* max long-name bytes (UTF-8, incl NUL) */
#endif

/* lookup by full name (exact 8.3 first, then VFAT LFN, case-insensitive);
 * on success fills out and the actual 11-byte short name of the entry */
int  fat_lookup_name(uint32_t dirclus, const char *name,
                     fat_dirent_t *out, uint8_t out_n83[11]);
/* create dir/file by full name (LFN + BODY~N.EXT alias when needed) */
int  fat_mkdir_name(uint32_t parent, const char *name);   /* 0 ok */
int  fat_mkfile_name(uint32_t parent, const char *name);  /* 0 ok */
/* same, but with an explicit attribute byte (e.g. FAT_ATTR_FILE |
 * FAT_ATTR_RO for the read-only built-ins) */
int  fat_mkfile_name_attr(uint32_t parent, const char *name, uint8_t attr);
/* rewrite ONLY the attribute byte of an existing entry; the name, the
 * cluster chain and the size are left exactly as they were */
int  fat_set_attr(uint32_t dirclus, const uint8_t *name83, uint8_t attr);
/* delete by full name (wipes the short entry + preceding LFN run) */
int  fat_delete_name(uint32_t dirclus, const char *name);
/* enumerate entries with display names (long name if present) */
void fat_list_dir(uint32_t dirclus, void (*cb)(const char *name,
                  uint8_t attr, uint32_t clus, uint32_t size, void *ud),
                  void *ud);

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
