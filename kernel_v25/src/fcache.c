/* fcache.c -- Stage 2B: per-task CE fd handle table + read-only file page
 * cache for file-backed mmap (slot 77 file_mmap / 78 mprotect / 79
 * munmap_shared, plus fd slots 87..91).
 *
 * Design notes:
 *  - The fd table is per-task, fixed size (MAX_OPEN_FILES slots each); user
 *    fds start at FD_FIRST (0/1/2 reserved).  The legacy path-based
 *    file_read/file_write/file_delete syscalls are untouched.
 *  - The page cache is a global fixed pool of read-only file pages keyed by
 *    (first cluster, block).  Frames are allocated on demand; when the last
 *    mapping drops a page's refcount to zero the frame is returned to the
 *    frame pool.  Capacity is a hard cap (FCACHE_MAX_PAGES): on a miss with
 *    the pool full the mmap fails ENOMEM -- no LRU eviction this stage.
 *  - Shared cache frames are scrubbed from a task's PTE without freeing the
 *    frame (task_scrub_page); only the cache release frees the frame.
 *
 * The kernel is single-core and non-preemptive inside a syscall, so no
 * locking is required.  No large static staging buffer lives here: every
 * read/write chunk goes through a single on-demand frame freed before return.
 */

#include "fcache.h"
#include "task.h"
#include "fs.h"
#include "vma.h"
#include "vga.h"

/* ---- per-task fd handle table --------------------------------------- */
typedef struct {
    uint8_t  used;
    uint8_t  ro;
    uint32_t cluster;
    uint32_t size;
    uint32_t off;
    char     path[FS_PATH_MAX];
} fd_ent_t;

static fd_ent_t fdtab[MAX_TASKS][MAX_OPEN_FILES];

static fd_ent_t *fd_lookup(task_t *t, int fd) {
    if (!t) return 0;
    int idx = fd - (int)FD_FIRST;
    if (idx < 0 || idx >= (int)MAX_OPEN_FILES) return 0;
    fd_ent_t *e = &fdtab[t->tid][idx];
    return e->used ? e : 0;
}

int fd_open(task_t *t, const char *path, int flags) {
    (void)flags;
    if (!t || !path || !*path) return FC_ERR_INVAL;

    uint32_t clus = 0, size = 0;
    if (fs_resolve_file(path, &clus, &size) != 0) return FC_ERR_NOENT;
    if (fs_perm_check(path, t->uid, t->gid, PERM_READ) != 0) return FC_ERR_PERM;

    int slot = -1;
    for (int i = 0; i < (int)MAX_OPEN_FILES; i++)
        if (!fdtab[t->tid][i].used) { slot = i; break; }
    if (slot < 0) return FC_ERR_FULL;

    fd_ent_t *e = &fdtab[t->tid][slot];
    for (uint32_t k = 0; k < sizeof(*e); k++) ((uint8_t *)e)[k] = 0;
    e->used    = 1;
    e->ro      = 1;
    e->cluster = clus;
    e->size    = size;
    e->off     = 0;
    for (int i = 0; i < FS_PATH_MAX - 1 && path[i]; i++) e->path[i] = path[i];
    e->path[FS_PATH_MAX - 1] = 0;
    return (int)FD_FIRST + slot;
}

int fd_close(task_t *t, int fd) {
    fd_ent_t *e = fd_lookup(t, fd);
    if (!e) return FC_ERR_BADF;
    e->used = 0;
    return FC_OK;
}

int fd_read(task_t *t, int fd, void *buf, int max) {
    fd_ent_t *e = fd_lookup(t, fd);
    if (!e) return FC_ERR_BADF;
    if (max <= 0) return FC_ERR_INVAL;
    if (max > 16384) max = 16384;
    if (e->off >= e->size) return 0;

    /* one on-demand staging frame; freed before return. */
    uint8_t *st = (uint8_t *)kzalloc_frame();
    if (!st) return FC_ERR_NOMEM;
    int n = fs_read_at(e->path, (char *)st, (int)e->off, max);
    if (n < 0) { frame_free((uint32_t)st); return FC_ERR_NOENT; }
    if (n > 0 && copy_to_user(t, buf, (const char *)st, (uint32_t)n) != 0) {
        frame_free((uint32_t)st); return FC_ERR_INVAL;
    }
    frame_free((uint32_t)st);
    e->off += (uint32_t)n;
    return n;
}

int fd_write(task_t *t, int fd, const void *buf, int len) {
    fd_ent_t *e = fd_lookup(t, fd);
    if (!e) return FC_ERR_BADF;
    if (len < 0 || len > 16384) return FC_ERR_INVAL;
    if (e->ro) return FC_ERR_PERM;      /* 2B opens read-only; offset write deferred */

    uint8_t *st = (uint8_t *)kzalloc_frame();
    if (!st) return FC_ERR_NOMEM;
    if (len > 0 && copy_from_user(t, (char *)st, buf, (uint32_t)len) != 0) {
        frame_free((uint32_t)st); return FC_ERR_INVAL;
    }
    int r = fs_write_raw_mode(e->path, (const char *)st, (uint32_t)len, 1);
    frame_free((uint32_t)st);
    return r == 0 ? len : r;
}

int fd_lseek(task_t *t, int fd, int off, int whence) {
    fd_ent_t *e = fd_lookup(t, fd);
    if (!e) return FC_ERR_BADF;
    uint32_t npos;
    if (whence == FCB_SEEK_SET)      npos = (uint32_t)off;
    else if (whence == FCB_SEEK_CUR) npos = e->off + (uint32_t)off;
    else if (whence == FCB_SEEK_END) npos = e->size + (uint32_t)off;
    else return FC_ERR_INVAL;
    if (npos > e->size) npos = e->size;
    e->off = npos;
    return (int)npos;
}

/* ---- global read-only file page cache ------------------------------- */
typedef struct {
    uint8_t  used;
    uint32_t cluster;
    uint32_t block;
    uint32_t refcnt;
    uint32_t pa;
} fc_ent_t;

static fc_ent_t cache[FCACHE_MAX_PAGES];
static uint32_t g_hits = 0;

static fc_ent_t *cache_find(uint32_t cluster, uint32_t block) {
    for (uint32_t i = 0; i < FCACHE_MAX_PAGES; i++)
        if (cache[i].used && cache[i].cluster == cluster &&
            cache[i].block == block) return &cache[i];
    return 0;
}

static uint32_t cache_get(uint32_t cluster, uint32_t block, uint32_t size,
                          const char *path) {
    fc_ent_t *e = cache_find(cluster, block);
    if (e) { e->refcnt++; g_hits++; return e->pa; }

    int slot = -1;
    for (uint32_t i = 0; i < FCACHE_MAX_PAGES; i++)
        if (!cache[i].used) { slot = (int)i; break; }
    if (slot < 0) return 0;                       /* pool full: fail, no evict */

    uint32_t pa = (uint32_t)kzalloc_frame();
    if (!pa) return 0;
    uint8_t *f = (uint8_t *)pa;
    uint32_t poff = block * 4096u;
    uint32_t n = 4096u;
    if (poff >= size) n = 0;
    else if (poff + n > size) n = size - poff;
    if (n > 0) {
        int r = fs_read_at(path, (char *)f, (int)poff, (int)n);
        if (r < 0) { frame_free(pa); return 0; }
        /* bytes past EOF stay zeroed (kzalloc_frame) */
    }
    fc_ent_t *ce = &cache[slot];
    ce->used = 1; ce->cluster = cluster; ce->block = block;
    ce->refcnt = 1; ce->pa = pa;
    return pa;
}

static void cache_put(uint32_t cluster, uint32_t block) {
    fc_ent_t *e = cache_find(cluster, block);
    if (!e) return;
    if (e->refcnt > 0) e->refcnt--;
    if (e->refcnt == 0) {
        frame_free(e->pa);
        e->used = 0; e->pa = 0;
    }
}

/* ---- active file-mmap region descriptors (kernel-global) ------------- */
typedef struct {
    uint8_t  used;
    uint32_t start;
    uint32_t end;
    uint32_t cluster;
    uint32_t size;
    int      owner_tid;
    char     path[FS_PATH_MAX];
} fmap_t;

static fmap_t maps[MAX_FILE_MAPS];

static fmap_t *map_find(uint32_t addr) {
    for (uint32_t i = 0; i < MAX_FILE_MAPS; i++)
        if (maps[i].used && addr >= maps[i].start && addr < maps[i].end)
            return &maps[i];
    return 0;
}

/* first-fit gap scan over the extension arena, below the CSO1 library zone,
 * skipping every occupied VMA (anon and file alike). */
static uint32_t alloc_va(task_t *t, uint32_t want) {
    want = (want + 0xFFFu) & ~0xFFFu;
    uint32_t cur = USER_EXT_BASE;
    while (1) {
        uint32_t next = USER_LIB_BASE;
        for (vma_t *x = t->vm; x; x = x->next)
            if (x->start >= cur && x->start < next) next = x->start;
        if (next - cur >= want && cur + want <= USER_LIB_BASE) return cur;
        if (next >= USER_LIB_BASE) return 0;
        cur = next;
        for (vma_t *x = t->vm; x; x = x->next)
            if (x->start == cur && x->end > cur) { cur = x->end; break; }
        if (cur >= USER_LIB_BASE) return 0;
    }
}

int fcache_mmap(task_t *t, int fd, uint32_t len, int prot, uint32_t *out_va) {
    if (!t || !out_va) return FC_ERR_INVAL;
    fd_ent_t *e = fd_lookup(t, fd);
    if (!e) return FC_ERR_BADF;
    if (prot & FCB_PROT_WRITE) return FC_ERR_PERM;   /* read-only this stage */
    if (len == 0) return FC_ERR_INVAL;

    uint32_t span = (len + 0xFFFu) & ~0xFFFu;
    uint32_t base = alloc_va(t, span);
    if (!base) return FC_ERR_NOMEM;

    vma_t *v = vma_new(base, base + span, VMA_MMAP, 0);
    if (!v) return FC_ERR_NOMEM;

    int ms = -1;
    for (uint32_t i = 0; i < MAX_FILE_MAPS; i++)
        if (!maps[i].used) { ms = (int)i; break; }
    if (ms < 0) { vma_free(v); return FC_ERR_FULL; }

    /* Lazy: register the file-backed VMA + descriptor only.  Pages are pulled
     * from the read-only cache on first touch via fcache_fault_page() (the
     * VMA_MMAP branch in vma_map_page_on_fault); no frame is pinned here. */
    fmap_t *m = &maps[ms];
    for (uint32_t k = 0; k < sizeof(*m); k++) ((uint8_t *)m)[k] = 0;
    m->used = 1; m->start = base; m->end = base + span;
    m->cluster = e->cluster; m->size = e->size; m->owner_tid = t->tid;
    for (int i = 0; i < FS_PATH_MAX - 1 && e->path[i]; i++) m->path[i] = e->path[i];
    m->path[FS_PATH_MAX - 1] = 0;

    t->vm = vma_insert(t->vm, v);
    *out_va = base;
    return FC_OK;
}

int fcache_munmap(task_t *t, uint32_t addr, uint32_t len) {
    if (!t || len == 0) return FC_ERR_INVAL;
    fmap_t *m = map_find(addr);
    if (!m || m->owner_tid != t->tid) return FC_ERR_BADF;
    uint32_t start = addr & ~0xFFFu;
    uint32_t end = addr + ((len + 0xFFFu) & ~0xFFFu);
    if (start < m->start || end > m->end) return FC_ERR_INVAL;

    for (uint32_t p = start; p < end; p += 4096u) {
        uint32_t block = (p - m->start) / 4096u;
        task_scrub_page(&t->space, p);
        cache_put(m->cluster, block);
    }
    vma_t *v = vma_find(t->vm, start);
    if (v && v->kind == VMA_MMAP) {
        t->vm = vma_remove(t->vm, v);
        vma_free(v);
    }
    if (start == m->start && end == m->end) m->used = 0;
    return FC_OK;
}

int fcache_mprotect(task_t *t, uint32_t addr, uint32_t len, int prot) {
    if (!t || len == 0) return FC_ERR_INVAL;
    fmap_t *m = map_find(addr);
    if (!m || m->owner_tid != t->tid) return FC_ERR_BADF;
    /* file mappings are read-only; raising to write is denied. */
    if (prot & FCB_PROT_WRITE) return FC_ERR_EPERM;
    return FC_OK;
}

uint32_t fcache_fault_page(task_t *t, uint32_t vaddr) {
    if (!t) return 0;
    fmap_t *m = map_find(vaddr);
    if (!m || m->owner_tid != t->tid) return 0;
    uint32_t block = (vaddr - m->start) / 4096u;
    return cache_get(m->cluster, block, m->size, m->path);
}

void fcache_cleanup_task(task_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < MAX_FILE_MAPS; i++) {
        fmap_t *m = &maps[i];
        if (!m->used || m->owner_tid != t->tid) continue;
        for (uint32_t p = m->start; p < m->end; p += 4096u) {
            uint32_t block = (p - m->start) / 4096u;
            task_scrub_page(&t->space, p);
            cache_put(m->cluster, block);
        }
        vma_t *v = vma_find(t->vm, m->start);
        if (v && v->kind == VMA_MMAP) {
            t->vm = vma_remove(t->vm, v);
            vma_free(v);
        }
        m->used = 0;
    }
    /* release the task's open fds */
    for (uint32_t i = 0; i < MAX_OPEN_FILES; i++)
        fdtab[t->tid][i].used = 0;
}

uint32_t fcache_stat_pages(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < FCACHE_MAX_PAGES; i++) if (cache[i].used) n++;
    return n;
}

uint32_t fcache_stat_hits(void) { return g_hits; }

int fcache_refcnt_at(uint32_t va) {
    fmap_t *m = map_find(va);
    if (!m) return -1;
    uint32_t block = (va - m->start) / 4096u;
    fc_ent_t *e = cache_find(m->cluster, block);
    return e ? (int)e->refcnt : -1;
}
