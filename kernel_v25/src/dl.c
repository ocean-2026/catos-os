/* dl.c -- CSO1 runtime shared-object loader (v2.47).
 *
 * Implements dlopen()/dlsym()/dlclose() as kernel syscalls.  A CSO1 module
 * is linked at a fixed VA inside USER_LIB_BASE..USER_LIB_END, so loading is
 * a straightforward "map frames at that base and copy the image" operation
 * -- no relocation pass is needed.  Each task gets its own copy of the
 * frames; task_space_destroy() reclaims them on exit.
 *
 * Security: every field of the on-disk header is validated before it is
 * trusted.  The image/bss span must fit in the library zone, the export
 * table and name blob must lie inside the file and inside one 4 KiB kernel
 * metadata frame, and dlsym() range-checks every function offset.
 */

#include "dl.h"
#include "task.h"
#include "fs.h"
#include "vga.h"

/* One static staging buffer, reused by every dlopen().  The kernel is
 * non-preemptive inside a syscall, so no locking is needed.  Keeping it
 * in BSS (rather than on the 8 KiB kernel stack) avoids a stack overflow. */
static uint8_t cso_file_buf[CSO_MAX_FILE];

static dl_module_t dl_mods[MAX_DL_MODULES];
static int dl_next_handle = 1;

static int dl_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static dl_module_t *dl_find(int handle, int tid) {
    if (handle <= 0) return 0;
    for (int i = 0; i < MAX_DL_MODULES; i++)
        if (dl_mods[i].used && dl_mods[i].handle == handle &&
            dl_mods[i].owner_tid == tid) return &dl_mods[i];
    return 0;
}

static dl_module_t *dl_find_base(uint32_t base, int tid) {
    for (int i = 0; i < MAX_DL_MODULES; i++)
        if (dl_mods[i].used && dl_mods[i].base == base &&
            dl_mods[i].owner_tid == tid) return &dl_mods[i];
    return 0;
}

int dl_open(struct task *t, const char *path) {
    if (!t || !path || !*path) return DL_ERR_INVAL;

    uint32_t fsize = 0;
    if (fs_resolve_file(path, 0, &fsize) != 0 || fsize == 0) return DL_ERR_NOENT;
    if (fsize > CSO_MAX_FILE) return DL_ERR_INVAL;

    int got = fs_read_at(path, (char *)cso_file_buf, 0, (int)fsize);
    if (got < (int)sizeof(cso_header_t)) return DL_ERR_INVAL;
    fsize = (uint32_t)got;

    cso_header_t h;
    for (uint32_t i = 0; i < sizeof(h); i++) ((uint8_t *)&h)[i] = cso_file_buf[i];
    if (h.magic[0] != CSO_MAGIC0 || h.magic[1] != CSO_MAGIC1 ||
        h.magic[2] != CSO_MAGIC2 || h.magic[3] != CSO_MAGIC3)
        return DL_ERR_INVAL;
    if (h.version != CSO_VERSION) return DL_ERR_INVAL;
    if (h.image_size == 0 || h.image_size > CSO_MAX_IMAGE) return DL_ERR_INVAL;
    if (h.bss_size > CSO_MAX_BSS) return DL_ERR_INVAL;
    if ((h.base & 0xFFFu) != 0 || h.base < USER_LIB_BASE ||
        h.base >= USER_LIB_END) return DL_ERR_INVAL;
    if (h.export_cnt > CSO_MAX_EXPORTS) return DL_ERR_INVAL;
    if (h.export_off < sizeof(cso_header_t) ||
        h.export_off > fsize - h.export_cnt * sizeof(cso_export_t))
        return DL_ERR_INVAL;

    /* The raw image is the tail of the file. */
    uint32_t image_off = fsize - h.image_size;
    if (image_off < h.export_off + h.export_cnt * sizeof(cso_export_t))
        return DL_ERR_INVAL;

    uint32_t span = (h.image_size + h.bss_size + 0xFFFu) & ~0xFFFu;
    if (span == 0 || h.base > USER_LIB_END - span) return DL_ERR_INVAL;

    /* Already loaded by this task at this base?  Idempotent open. */
    dl_module_t *old = dl_find_base(h.base, t->tid);
    if (old) return old->handle;

    if (vma_find(t->vm, h.base)) return DL_ERR_INVAL;   /* zone occupied */

    int slot = -1;
    for (int i = 0; i < MAX_DL_MODULES; i++)
        if (!dl_mods[i].used) { slot = i; break; }
    if (slot < 0) return DL_ERR_FULL;

    /* Copy export entries + name blob into one kernel frame. */
    uint32_t meta_bytes = image_off - h.export_off;
    if (meta_bytes == 0 || meta_bytes > 4096) return DL_ERR_INVAL;
    uint8_t *meta = (uint8_t *)kzalloc_frame();
    if (!meta) return DL_ERR_NOMEM;
    for (uint32_t i = 0; i < meta_bytes; i++)
        meta[i] = cso_file_buf[h.export_off + i];

    /* Map the image/bss span into this task's PDE 9 page table. */
    vma_t *v = vma_new(h.base, h.base + span, VMA_LIB, 1);
    if (!v) { frame_free((uint32_t)meta); return DL_ERR_NOMEM; }
    for (uint32_t off = 0; off < span; off += 4096) {
        uint32_t pa = (uint32_t)kzalloc_frame();       /* zeroed == bss */
        if (!pa || task_map_page(&t->space, h.base + off, pa, 1) != 0) {
            if (pa) frame_free(pa);
            /* unmap the pages already installed */
            for (uint32_t o = 0; o < off; o += 4096)
                task_unmap_page(&t->space, h.base + o);
            vma_free(v);
            frame_free((uint32_t)meta);
            return DL_ERR_NOMEM;
        }
    }
    t->vm = vma_insert(t->vm, v);

    /* Copy the image bytes through the task's page tables. */
    for (uint32_t off = 0; off < h.image_size; ) {
        uint32_t va = h.base + off;
        uint32_t pa = task_virt_to_phys(&t->space, va);
        if (!pa) break;
        uint32_t n = 4096 - (va & 0xFFFu);
        if (n > h.image_size - off) n = h.image_size - off;
        uint8_t *dst = (uint8_t *)(pa + (va & 0xFFFu));
        for (uint32_t i = 0; i < n; i++) dst[i] = cso_file_buf[image_off + off + i];
        off += n;
    }

    /* Inject the vsyscall table pointer, exactly as the CE01 loader does.
     * The value is the SAME fixed VA in every task (VS_TABLE = 0x2080800),
     * so it remains correct even if the library image were shared later. */
    if (h.api_offset != CSO_NO_API && h.api_offset + 4 <= h.image_size) {
        uint32_t va = h.base + h.api_offset;
        uint32_t pa = task_virt_to_phys(&t->space, va);
        if (pa) *(uint32_t *)pa = 0x2080800u;           /* CATOS_VS_TABLE */
    }

    dl_module_t *m = &dl_mods[slot];
    m->used        = 1;
    m->handle      = dl_next_handle++;
    if (dl_next_handle <= 0) dl_next_handle = 1;
    m->owner_tid   = t->tid;
    m->base        = h.base;
    m->span        = span;
    m->export_cnt  = h.export_cnt;
    m->meta        = meta;
    m->meta_bytes  = meta_bytes;
    return m->handle;
}

void *dl_sym(struct task *t, int handle, const char *name) {
    if (!t || !name || !*name) return 0;
    dl_module_t *m = dl_find(handle, t->tid);
    if (!m) return 0;

    const cso_export_t *ex = (const cso_export_t *)m->meta;
    const char *names = (const char *)(m->meta + m->export_cnt * sizeof(cso_export_t));
    if (m->meta_bytes < m->export_cnt * sizeof(cso_export_t)) return 0;

    for (uint32_t i = 0; i < m->export_cnt; i++) {
        if (ex[i].name_off >= m->meta_bytes) continue;
        const char *nm = names + ex[i].name_off;
        if (!dl_streq(nm, name)) continue;
        if (ex[i].func_off >= m->span) return 0;        /* out of bounds */
        return (void *)(m->base + ex[i].func_off);
    }
    return 0;
}

int dl_close(struct task *t, int handle) {
    if (!t) return DL_ERR_BADH;
    dl_module_t *m = dl_find(handle, t->tid);
    if (!m) return DL_ERR_BADH;

    vma_t *v = vma_find(t->vm, m->base);
    if (v && v->kind == VMA_LIB && v->start == m->base) {
        for (uint32_t off = 0; off < m->span; off += 4096)
            task_unmap_page(&t->space, m->base + off); /* frees frames */
        t->vm = vma_remove(t->vm, v);
        vma_free(v);
    }
    if (m->meta) frame_free((uint32_t)m->meta);
    m->used = 0;
    m->meta = 0;
    return DL_OK;
}

void dl_close_all(struct task *t) {
    if (!t) return;
    for (int i = 0; i < MAX_DL_MODULES; i++) {
        if (!dl_mods[i].used || dl_mods[i].owner_tid != t->tid) continue;
        if (dl_mods[i].meta) frame_free((uint32_t)dl_mods[i].meta);
        dl_mods[i].meta = 0;
        dl_mods[i].used = 0;
    }
    /* The user pages are reclaimed by task_space_destroy() in task_cleanup. */
}
