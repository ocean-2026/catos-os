#ifndef DL_H
#define DL_H

/* dl.h -- CSO1 (CatOS Shared Object v1) runtime dynamic-linker interface.
 *
 * CSO1 is a deliberately small shared-library format: a flat PE-style image
 * linked at a FIXED virtual base inside USER_LIB_BASE..USER_LIB_END, plus an
 * export table and a string blob that the kernel reads at dlopen() time.
 * No relocation engine is required -- the linker base IS the load address.
 *
 * File layout (all integers little-endian):
 *
 *   offset  size   field
 *   0       4      magic "CSO1"
 *   4       4      version (1)
 *   8       4      load base VA (page aligned, in the library zone)
 *   12      4      image_size (code+data bytes, > 0)
 *   16      4      bss_size (zeroed bytes following the image)
 *   20      4      api_offset (offset of the 4-byte API pointer slot,
 *                   0xFFFFFFFF = none)
 *   24      4      export_off (file offset of the export table)
 *   28      4      export_cnt
 *   ...    8*cnt   export entries: { name_off, func_off }
 *   ...    N       name blob (NUL-terminated export names)
 *   ...    image_size  raw image bytes
 *
 * The kernel injects the vsyscall table pointer into the API slot at load
 * time (same mechanism as CE01), so a library calls the OS through exactly
 * the same append-only catos_api_t table a normal .ce program uses.
 *
 * Only the raw image is copied into the task's address space.  The export
 * metadata lives in a kernel frame so dlsym() never has to read user memory.
 */

#include <stdint.h>
#include "paging.h"

#define CSO_MAGIC0 'C'
#define CSO_MAGIC1 'S'
#define CSO_MAGIC2 'O'
#define CSO_MAGIC3 '1'
#define CSO_VERSION  1u

#define CSO_NO_API   0xFFFFFFFFu

/* Build-time/runtime limits (kernel rejects anything larger). */
#define CSO_MAX_IMAGE    0x10000u   /* 64 KiB of code+data per library */
#define CSO_MAX_BSS      0x10000u   /* 64 KiB of bss */
#define CSO_MAX_EXPORTS  128u
#define CSO_MAX_NAMES    2048u
#define CSO_MAX_FILE     (0x10000u + 0x10000u + 0x4000u)
#define MAX_DL_MODULES   16

typedef struct {
    uint8_t  magic[4];
    uint32_t version;
    uint32_t base;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t api_offset;
    uint32_t export_off;
    uint32_t export_cnt;
} cso_header_t;                     /* 32 bytes */

typedef struct {
    uint32_t name_off;              /* offset into the name blob */
    uint32_t func_off;              /* offset from the library base VA */
} cso_export_t;                     /* 8 bytes */

typedef struct {
    int      used;
    int      handle;                /* small positive integer, 1..N */
    int      owner_tid;
    uint32_t base;
    uint32_t span;                  /* image+bss, page-rounded */
    uint32_t export_cnt;
    uint8_t *meta;                  /* one kernel frame: exports + names */
    uint32_t meta_bytes;
} dl_module_t;

/* Return codes (negative errno, matching the rest of the syscall ABI). */
#define DL_OK            0
#define DL_ERR_NOENT    -1
#define DL_ERR_INVAL    -3
#define DL_ERR_FULL     -4
#define DL_ERR_NOMEM   -12
#define DL_ERR_BADH    -22          /* bad handle / unknown symbol */

struct task;

int   dl_open(struct task *t, const char *path);
void *dl_sym(struct task *t, int handle, const char *name);
int   dl_close(struct task *t, int handle);
/* Release only the kernel-side metadata of every module owned by t; the
 * caller (task_cleanup) unmaps/frees the user pages via task_space_destroy. */
void  dl_close_all(struct task *t);

#endif /* DL_H */
