#ifndef FCACHE_H
#define FCACHE_H

/* fcache.h -- Stage 2B: per-process CE fd handle table + read-only file
 * page cache for file-backed mmap.
 *
 * fd layer: a small fixed table per task (fd 0/1/2 reserved for
 * stdin/stdout/stderr; user fds start at FD_FIRST = 3).  The legacy
 * path-based file_read/file_write/file_delete syscalls stay unchanged;
 * these fd slots are an additional append-only ABI surface.
 *
 * Page cache: a global fixed pool of read-only file pages keyed by
 * (first cluster, block number).  Frames are allocated on demand and
 * returned to the frame pool when the last mapping drops the refcount to
 * zero.  Capacity is a hard cap: on a miss with the pool full, the mmap
 * fails with ENOMEM (no LRU eviction this stage -- see docs/dynamic-linking.md).
 *
 * The kernel runs single-core and non-preemptively inside a syscall, so no
 * locking is needed.
 */

#include <stdint.h>
#include "paging.h"

/* prot bits (mirror the anonymous mmap slot 55 convention: bit0 = write). */
#define FCB_PROT_READ   0
#define FCB_PROT_WRITE  1

/* lseek whence */
#define FCB_SEEK_SET    0
#define FCB_SEEK_CUR    1
#define FCB_SEEK_END    2

#define FD_FIRST        3u
#define MAX_OPEN_FILES  8u      /* per-task open fd slots */
#define FCACHE_MAX_PAGES 64u    /* 64 x 4 KiB = 256 KiB frame budget */
#define MAX_FILE_MAPS   16u     /* active file-mmap regions, kernel-global */

/* Return codes (negative errno, matching the rest of the syscall ABI). */
#define FC_OK           0
#define FC_ERR_NOENT   -1
#define FC_ERR_INVAL   -3
#define FC_ERR_FULL    -4
#define FC_ERR_NOMEM  -12
#define FC_ERR_PERM   -13       /* EACCES: write request on a read-only map */
#define FC_ERR_EPERM  -11       /* mprotect raise (RO->RW) denied */
#define FC_ERR_BADF   -22
#define FC_ERR_NOSYS   -8

struct task;

/* ---- fd handle table -------------------------------------------------- */
/* open(path, flags): resolve + PERM_READ check, allocate an fd slot.
 * Returns fd >= FD_FIRST, or a negative errno. */
int      fd_open(struct task *t, const char *path, int flags);
/* close(fd): release the slot.  0 ok / negative errno. */
int      fd_close(struct task *t, int fd);
/* read(fd, buf, max): copy up to max bytes from the current offset, advance
 * it.  Bytes read (>0), 0 at EOF, or negative errno. */
int      fd_read(struct task *t, int fd, void *buf, int max);
/* write(fd, buf, len): append-mode raw write (offset-write deferred).
 * Bytes written, or negative errno. */
int      fd_write(struct task *t, int fd, const void *buf, int len);
/* lseek(fd, off, whence): reposition the offset.  Returns new offset, or
 * negative errno. */
int      fd_lseek(struct task *t, int fd, int off, int whence);

/* ---- file-backed page cache ------------------------------------------ */
/* Map [0, len) of the file behind fd into the caller's address space.
 * Read-only: a PROT_WRITE request returns FC_ERR_PERM.  *out_va receives
 * the user VA on success.  Returns FC_OK or a negative errno. */
int      fcache_mmap(struct task *t, int fd, uint32_t len, int prot,
                     uint32_t *out_va);
/* Unmap a previous file mapping: drop cache refs, scrub the task PTEs.
 * 0 ok / negative errno. */
int      fcache_munmap(struct task *t, uint32_t addr, uint32_t len);
/* mprotect: only privilege lowering (RW->RO) is allowed; a raise returns
 * FC_ERR_EPERM. */
int      fcache_mprotect(struct task *t, uint32_t addr, uint32_t len, int prot);
/* Page-fault helper: resolve a VMA_MMAP vaddr to its cached physical frame,
 * filling the cache on first touch.  Returns the frame PA (page-aligned)
 * or 0 on failure. */
uint32_t fcache_fault_page(struct task *t, uint32_t vaddr);
/* Task exit: drop every cache ref held by t and scrub its file PTEs (the
 * frames are NOT freed here unless the refcount hits zero). */
void     fcache_cleanup_task(struct task *t);

/* Diagnostics for the smoke probe. */
uint32_t fcache_stat_pages(void);    /* cached pages currently resident */
uint32_t fcache_stat_hits(void);     /* cache hits since boot */
int      fcache_refcnt_at(uint32_t va); /* refcnt of the cached page covering va, -1 if none */

#endif /* FCACHE_H */
