#ifndef FS_H
#define FS_H

#include <stdint.h>

/*
 * CatOS file system API.  Backed by a real FAT32 volume on the IDE drive
 * (see fat.c).  This header only exposes the API used by the shell and
 * the nano editor; no on-disk structures leak through.
 */

#define FS_NAME_MAX     48      /* max path component incl. NUL (LFN) */
#define FS_DATA_MAX     4096    /* max file size supported by the editor */
#define FS_PATH_MAX     160     /* max absolute path length */

#define FS_DIR  0
#define FS_FILE 1

/* open-file handle: a memory view of one file that can be edited and
 * written back to the FAT32 volume with fs_save(). */
typedef struct fs_entry {
    char     name[FS_NAME_MAX];
    uint8_t  type;             /* FS_DIR / FS_FILE */
    uint8_t  used;
    uint32_t cluster;          /* first FAT cluster on disk */
    uint32_t parent_cluster;   /* directory containing this entry */
    uint8_t  name83[11];       /* 8.3 name on disk */
    uint16_t size;             /* bytes used in data */
    char     data[FS_DATA_MAX];/* file content (editor buffer) */
} fs_entry_t;

/* return codes shared by mkdir/touch/chdir/create */
#define FS_OK          0
#define FS_ERR_NOENT  -1   /* path / parent not found */
#define FS_ERR_NOTDIR -2   /* expected a directory */
#define FS_ERR_INVAL  -3   /* bad name (empty, "." "..", too long, bad chars) */
#define FS_ERR_FULL   -4   /* disk / entry pool exhausted */
#define FS_ERR_EXIST  -5   /* already exists */

void       fs_init(void);                 /* probe FAT32 volume */
int        fs_fs_ready(void);             /* 1 when a FAT32 volume is mounted */
void       fs_format(void);               /* create FAT32 volume + std dirs */
uint16_t   fs_cwd_index(void);
int        fs_chdir(const char *path);    /* empty path = root */
int        fs_mkdir(const char *path);
int        fs_touch(const char *path);    /* create empty file if absent */
void       fs_list(const char *path);     /* NULL = current dir */
int        fs_enum_files(const char *path, char out[][FS_NAME_MAX], int maxn);
void       fs_pwd(char *out, int outlen); /* absolute path of cwd */
/* Canonical absolute path: abs_path() + ASCII lowercase folding.  FAT32
 * is case-insensitive, so security lookups (ACL) MUST compare this form
 * and never the raw user spelling -- "/BIN/X.CE" and "/bin/x.ce" are the
 * same file on disk but were different keys in the old ACL table. */
void       fs_canon_path(const char *in, char *out, int outlen);

/* editor helpers: operate on a resolved file entry */
fs_entry_t *fs_open_file(const char *path); /* resolves; NULL if absent/not file */
fs_entry_t *fs_create_file(const char *path);/* touch + resolve; NULL on failure */
int        fs_file_size(const fs_entry_t *f);
int        fs_append_line(fs_entry_t *f, const char *line); /* 0 ok, -1 if full */
int        fs_line_count(const fs_entry_t *f);
int        fs_get_line(const fs_entry_t *f, int idx, char *out, int outlen);
int        fs_delete_line(fs_entry_t *f, int idx);           /* 0 ok, -1 bad idx */
void       fs_clear_file(fs_entry_t *f);
int        fs_save(fs_entry_t *f);        /* write memory view back to disk */
/* is the file read-only (FAT_ATTR_RO, e.g. a built-in app)?
 * 1 = yes, 0 = no or absent */
int        fs_is_readonly(const char *path);

/* raw (non-editor) access, used by the CE loader and CE system calls */
typedef void (*fs_foreach_cb)(int is_dir, const char *name,
                              uint32_t size, void *ud);
int  fs_resolve_file(const char *path, uint32_t *cluster, uint32_t *size);
int  fs_foreach(const char *path, fs_foreach_cb cb, void *ud);
int  fs_read_text(const char *path, char *buf, int max);
int  fs_write_text(const char *path, const char *data, uint32_t size);
/* raw write of arbitrary-size data (no FS_DATA_MAX limit); skips if the
 * file already exists.  Used to install built-in .ce apps and docs. */
int  fs_write_raw(const char *path, const char *data, uint32_t size);
/* same as fs_write_raw(), but the file is created with `attr` (the
 * built-in apps are installed FAT_ATTR_FILE | FAT_ATTR_RO) */
int  fs_write_raw_attr(const char *path, const char *data, uint32_t size,
                       uint8_t attr);
/* set the FAT attribute byte of an existing file; the content is left
 * untouched (used to seal the built-in apps on older volumes) */
int  fs_set_attr(const char *path, uint8_t attr);
/* v2.5 big files: offset read, mode write (0=truncate, 1=append), delete */
int  fs_read_at(const char *path, char *buf, int offset, int max);
int  fs_write_raw_mode(const char *path, const char *data, uint32_t size, int mode);
int  fs_delete(const char *path);

#endif
