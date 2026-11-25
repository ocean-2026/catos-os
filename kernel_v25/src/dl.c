/* dl.c -- CSO2 runtime shared-object loader (v2.48).
 *
 * Implements dlopen()/dlsym()/dlclose() as kernel syscalls.  A CSO2 module
 * is linked at a fixed VA inside USER_LIB_BASE..USER_LIB_END; the kernel maps
 * its flat image there and applies the extracted relocations.
 *
 * 2C-3 block ③: the read-only span [0,text_end) is a GLOBAL shared frame
 * pool.  The pool key is the source file's first cluster (NOT the task/pid):
 * the same library opened by several tasks reuses one set of RO physical
 * frames, mapped read-only (PTE RW=0) into each opener's PDE 9.  The
 * [text_end,data_end) .data span and the .bss stay private per task.  A page
 * that straddles the text_end boundary is kept private (a frame cannot be
 * shared read-only for part of it and writable for the rest).
 *
 * Library-level refcount: the first opener builds the pool, every later
 * opener increments it, and the last dlclose (or task cleanup) drops it to
 * zero, at which point the shared RO frames and metadata are released.
 *
 * Security: every field of the on-disk header is validated before it is
 * trusted.  The image/bss span must fit in the library zone, the export
 * table and name blob must lie inside the file and inside one 4 KiB kernel
 * metadata frame, and dlsym() range-checks every symbol offset.
 */

#include "dl.h"
#include "task.h"
#include "fs.h"
#include "vga.h"
#include "serial_log.h"

/* One descriptor table, fixed size.  The kernel is non-preemptive inside a
 * syscall, so no locking is needed.  There is NO large static staging
 * buffer here: the file is read in chunks through a single on-demand frame
 * (allocated in dl_open and freed before return), keeping .bss small. */
static dl_module_t dl_mods[MAX_DL_MODULES];
static dl_shared_t dl_sh[MAX_DL_MODULES];
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

static dl_shared_t *dl_sh_find(uint32_t cluster) {
    for (int i = 0; i < MAX_DL_MODULES; i++)
        if (dl_sh[i].used && dl_sh[i].cluster == cluster) return &dl_sh[i];
    return 0;
}

static dl_shared_t *dl_sh_alloc(void) {
    for (int i = 0; i < MAX_DL_MODULES; i++)
        if (!dl_sh[i].used) return &dl_sh[i];
    return 0;
}

/* A page is shared read-only only when the WHOLE page lies below text_end.
 * A page that straddles the boundary (text_end not page-aligned) stays
 * private: one frame cannot carry both a shared-RO and a private-RW view. */
static int dl_page_ro(uint32_t text_end, uint32_t off) {
    return text_end != 0 && off + 0x1000u <= text_end;
}

/* ---- shared relocation engine (block ①) ------------------------------ */
/* Supported R_386 types; anything else is rejected explicitly rather than
 * skipped.  GLOB_DAT and JMP_SLOT both write the resolved symbol VA into the
 * GOT/PLT slot; R_386_32 writes an absolute value; PC32 is relative to the
 * slot (minus the 4-byte instruction length, as the ELF ABI specifies). */
int dl_reloc_apply(struct task_space *sp, uint32_t base, uint32_t img_size,
                   uint32_t loc_off, uint32_t type, uint32_t sym_val,
                   int32_t addend) {
    if (!sp || loc_off >= img_size) return DL_ERR_RELOC;
    uint32_t loc_va = base + loc_off;
    uint32_t pa = task_virt_to_phys(sp, loc_va);
    if (!pa) return DL_ERR_RELOC;
    uint32_t *slot = (uint32_t *)pa;
    uint32_t val;
    switch (type) {
    case R_386_32:
    case R_386_GLOB_DAT:
    case R_386_JMP_SLOT:
        val = sym_val + (uint32_t)addend;
        break;
    case R_386_PC32:
        val = sym_val - loc_va - 4u + (uint32_t)addend;
        break;
    case R_386_RELATIVE:
        /* Base-relative fixup: slot holds the link-time offset; relocate
         * to the runtime base.  addend carries the pre-relocation slot value. */
        val = base + (uint32_t)addend;
        break;
    default:
        /* Unsupported reloc type: reject, do not silently skip. */
        return DL_ERR_RELOC;
    }
    *slot = val;
    return DL_OK;
}

/* Invoke a constructor/destructor array (init_array / fini_array) in order.
 * Each entry is a fnptr VA inside the module span; out-of-span entries are
 * rejected (skipped with a COM1 note) rather than branched through. */
static void dl_call_array(struct task *t, const dl_shared_t *sh,
                          uint32_t off, uint32_t cnt, const char *what) {
    if (!cnt) return;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t slot_va = sh->base + off + i * 4;
        uint32_t pa = task_virt_to_phys(&t->space, slot_va);
        if (!pa) { ser_write("dl "); ser_write(what); ser_write(" slot missing\n"); return; }
        uint32_t fn = *(uint32_t *)pa;
        if (fn < sh->base || fn >= sh->base + sh->span) {
            ser_write("dl "); ser_write(what); ser_write(" bad fnptr\n");
            return;
        }
        ((void (*)(void))fn)();
    }
}

/* Walk the shared rela table and apply the fixups to this task's mapped
 * pages.  On a subsequent open (first_load == 0) relocs targeting the shared
 * RO span are skipped: those slots were already patched into the shared
 * frame when the pool was built, and the fixed load base makes the value
 * identical in every task. */
static int dl_apply_relocs(struct task *t, dl_shared_t *sh, int first_load) {
    if (!sh->rela_cnt) return 1;
    cso_export_t *ex = (cso_export_t *)sh->meta;
    const char *names = (const char *)(sh->meta + sh->export_cnt * sizeof(cso_export_t));
    for (uint32_t i = 0; i < sh->rela_cnt; i++) {
        cso_rela_t *r = (cso_rela_t *)(sh->rela + i * sizeof(cso_rela_t));
        uint32_t type = r->r_info & 0xFFFFu;
        uint32_t sym  = r->r_info >> 16;
        if (r->r_offset >= sh->image_size) return 0;
        if (!first_load && r->r_offset < sh->text_end) continue;

        uint32_t symval = 0;
        const char *symname = "";
        if (sym) {
            uint32_t idx = sym - 1;     /* rela sym_idx is 1-based; 0 = null */
            if (idx >= sh->export_cnt) return 0;
            symval = sh->base + ex[idx].sym_off;
            symname = names + ex[idx].name_off;
        }
        /* RELATIVE(8) carries its addend in the pre-relocation slot;
         * symbolic fixups resolve to sym_val with addend 0. */
        int32_t addend = r->r_addend;
        if (type == R_386_RELATIVE) {
            uint32_t pa0 = task_virt_to_phys(&t->space, sh->base + r->r_offset);
            if (pa0) addend = (int32_t) * (uint32_t *)pa0;
        }
        if (dl_reloc_apply(&t->space, sh->base, sh->image_size,
                            r->r_offset, type, symval, addend) != DL_OK) {
            ser_write("dl reloc reject type=");
            ser_hex("reloc", type);
            return 0;
        }
        uint32_t patched = sh->base + r->r_offset;
        ser_write("reloc: type=");
        ser_hex("t", type);
        if (type == R_386_RELATIVE) {
            ser_write(" slot="); ser_hex("slot", patched);
            ser_write(" base="); ser_hex("base", sh->base);
        } else {
            ser_write(" sym="); ser_write(symname);
            ser_write(" patched="); ser_hex("va", patched);
        }
        ser_write("\n");
    }
    return 1;
}

int dl_open(struct task *t, const char *path) {
    if (!t || !path || !*path) return DL_ERR_INVAL;

    uint32_t fsize = 0, clus = 0;
    if (fs_resolve_file(path, &clus, &fsize) != 0 || fsize == 0) return DL_ERR_NOENT;
    if (fsize > CSO_MAX_FILE) return DL_ERR_INVAL;

    /* Read the header into a small stack buffer first.  CSO2 extends the
     * header to 64 bytes; a CSO1 file only supplies the first 32, so the
     * trailing v2 fields stay zeroed. */
    uint8_t hb[sizeof(cso_header_t)];
    for (uint32_t i = 0; i < sizeof(hb); i++) hb[i] = 0;
    int hdr_rd = fs_read_at(path, (char *)hb, 0, (int)sizeof(hb));
    if (hdr_rd < (int)sizeof(cso_header_t) - 32) return DL_ERR_INVAL;

    cso_header_t h;
    for (uint32_t i = 0; i < sizeof(h); i++) ((uint8_t *)&h)[i] = hb[i];
    if (h.magic[0] != CSO_MAGIC0 || h.magic[1] != CSO_MAGIC1 ||
        h.magic[2] != CSO_MAGIC2 ||
        (h.magic[3] != CSO_MAGIC3 && h.magic[3] != CSO2_MAGIC3))
        return DL_ERR_INVAL;
    int is_v2 = (h.magic[3] == CSO2_MAGIC3);
    if (h.version != CSO_VERSION && h.version != CSO2_VERSION) return DL_ERR_INVAL;
    if (h.image_size == 0 || h.image_size > CSO_MAX_IMAGE) return DL_ERR_INVAL;
    if (h.bss_size > CSO_MAX_BSS) return DL_ERR_INVAL;
    if ((h.base & 0xFFFu) != 0 || h.base < USER_LIB_BASE ||
        h.base >= USER_LIB_END) return DL_ERR_INVAL;
    if (h.export_cnt > CSO_MAX_EXPORTS) return DL_ERR_INVAL;
    if (h.export_off < sizeof(cso_header_t) ||
        h.export_off > fsize - h.export_cnt * sizeof(cso_export_t))
        return DL_ERR_INVAL;

    /* CSO2 v2 layout sanity: RO text span ends no after data end, which ends
     * no after the image; rela/array offsets lie inside the file/image. */
    if (is_v2) {
        if (h.text_end > h.data_end || h.data_end > h.image_size)
            return DL_ERR_INVAL;
        if (h.rela_off != 0 &&
            (h.rela_off < sizeof(cso_header_t) ||
             h.rela_off + h.rela_cnt * sizeof(cso_rela_t) > fsize))
            return DL_ERR_INVAL;
        if (h.init_array_cnt > 64 || h.fini_array_cnt > 64) return DL_ERR_INVAL;
        if (h.init_array_cnt &&
            (h.init_array_off >= h.image_size ||
             h.init_array_off + h.init_array_cnt * 4 > h.image_size))
            return DL_ERR_INVAL;
        if (h.fini_array_cnt &&
            (h.fini_array_off >= h.image_size ||
             h.fini_array_off + h.fini_array_cnt * 4 > h.image_size))
            return DL_ERR_INVAL;
    } else {
        h.rela_off = h.rela_cnt = h.text_end = h.data_end = 0;
        h.init_array_off = h.init_array_cnt = 0;
        h.fini_array_off = h.fini_array_cnt = 0;
    }

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

    dl_shared_t *sh = dl_sh_find(clus);
    int first_load = (sh == 0);

    /* ---- metadata + rela frames (shared, owned by the pool) ------------- */
    uint8_t *meta = 0;
    uint8_t *relaf = 0;
    uint32_t meta_bytes = image_off - h.export_off;
    if (first_load) {
        sh = dl_sh_alloc();
        if (!sh) return DL_ERR_FULL;
        if (meta_bytes == 0 || meta_bytes > 4096) return DL_ERR_INVAL;
        meta = (uint8_t *)kzalloc_frame();
        if (!meta) return DL_ERR_NOMEM;
        if (fs_read_at(path, (char *)meta, (int)h.export_off, (int)meta_bytes)
            < (int)meta_bytes) {
            frame_free((uint32_t)meta);
            return DL_ERR_INVAL;
        }
        if (h.rela_cnt) {
            uint32_t relab = h.rela_cnt * sizeof(cso_rela_t);
            relaf = (uint8_t *)kzalloc_frame();
            if (!relaf) { frame_free((uint32_t)meta); return DL_ERR_NOMEM; }
            if (fs_read_at(path, (char *)relaf, (int)h.rela_off, (int)relab)
                < (int)relab) {
                frame_free((uint32_t)relaf);
                frame_free((uint32_t)meta);
                return DL_ERR_INVAL;
            }
        }
    } else {
        /* Reuse the shared metadata/rela; reject if this file disagrees with
         * the pooled image (same cluster, different layout = tamper/dupe). */
        if (sh->base != h.base || sh->span != span ||
            sh->image_size != h.image_size || sh->text_end != h.text_end) {
            return DL_ERR_INVAL;
        }
        meta = sh->meta;
        relaf = sh->rela;
    }

    /* One on-demand staging frame for the image chunk copy; freed before
     * dl_open returns. */
    uint8_t *st = (uint8_t *)kzalloc_frame();
    if (!st) {
        if (first_load) {
            if (relaf) frame_free((uint32_t)relaf);
            frame_free((uint32_t)meta);
        }
        return DL_ERR_NOMEM;
    }

    /* Map the image/bss span into this task's PDE 9 page table. */
    vma_t *v = vma_new(h.base, h.base + span, VMA_LIB, 1);
    if (!v) {
        frame_free((uint32_t)st);
        if (first_load) {
            if (relaf) frame_free((uint32_t)relaf);
            frame_free((uint32_t)meta);
        }
        return DL_ERR_NOMEM;
    }

    int ro_idx = 0;
    int map_ok = 1;
    for (uint32_t off = 0; off < span; off += 4096) {
        int ro = dl_page_ro(h.text_end, off);
        uint32_t pa;
        if (ro && !first_load) {
            /* Reuse the pooled RO frame, mapped read-only. */
            if (ro_idx >= sh->ro_cnt) { map_ok = 0; break; }
            pa = sh->ro_pa[ro_idx++];
            if (task_map_page(&t->space, h.base + off, pa, 0) != 0) { map_ok = 0; break; }
        } else {
            pa = (uint32_t)kzalloc_frame();       /* zeroed == bss */
            if (!pa) { map_ok = 0; break; }
            if (task_map_page(&t->space, h.base + off, pa, 1) != 0) {
                frame_free(pa); map_ok = 0; break;
            }
            if (ro) {
                if (first_load) {
                    if (sh->ro_cnt >= DL_MAX_RO_PAGES) { map_ok = 0; break; }
                    sh->ro_pa[sh->ro_cnt++] = pa;
                }
                ro_idx++;
            }
        }
    }
    if (!map_ok) {
        for (uint32_t o = 0; o < span; o += 4096) {
            int ro = dl_page_ro(h.text_end, o);
            if (ro && !first_load) task_scrub_page(&t->space, h.base + o);
            else task_unmap_page(&t->space, h.base + o);
        }
        vma_free(v);
        frame_free((uint32_t)st);
        if (first_load) {
            for (int i = 0; i < sh->ro_cnt; i++) frame_free(sh->ro_pa[i]);
            sh->ro_cnt = 0;
            if (relaf) frame_free((uint32_t)relaf);
            frame_free((uint32_t)meta);
        }
        return DL_ERR_NOMEM;
    }
    t->vm = vma_insert(t->vm, v);

    /* Copy the image bytes through the task's page tables, one 4 KiB chunk
     * at a time via the staging frame.  On a subsequent open only the
     * private [text_end,image_size) portion needs copying; the shared RO
     * frames already hold the pooled image. */
    uint32_t copy_from = first_load ? 0 : h.text_end;
    int img_ok = 1;
    for (uint32_t off = copy_from; off < h.image_size; ) {
        uint32_t va = h.base + off;
        uint32_t pa = task_virt_to_phys(&t->space, va);
        if (!pa) { img_ok = 0; break; }
        uint32_t n = 4096 - (va & 0xFFFu);
        if (n > h.image_size - off) n = h.image_size - off;
        int rd = fs_read_at(path, (char *)st, (int)(image_off + off), (int)n);
        if (rd < (int)n) { img_ok = 0; break; }
        /* task_virt_to_phys already includes the page offset; do not add it
         * a second time (first-load chunks happen to start on a page boundary,
         * which hid this on block ①). */
        uint8_t *dst = (uint8_t *)pa;
        for (uint32_t i = 0; i < n; i++) dst[i] = st[i];
        off += n;
    }
    if (!img_ok) {
        for (uint32_t o = 0; o < span; o += 4096) {
            int ro = dl_page_ro(h.text_end, o);
            if (ro && !first_load) task_scrub_page(&t->space, h.base + o);
            else task_unmap_page(&t->space, h.base + o);
        }
        t->vm = vma_remove(t->vm, v);
        vma_free(v);
        frame_free((uint32_t)st);
        if (first_load) {
            for (int i = 0; i < sh->ro_cnt; i++) frame_free(sh->ro_pa[i]);
            sh->ro_cnt = 0;
            if (relaf) frame_free((uint32_t)relaf);
            frame_free((uint32_t)meta);
        }
        return DL_ERR_INVAL;
    }
    frame_free((uint32_t)st);

    /* Inject the vsyscall table pointer, exactly as the CE01 loader does.
     * The value is the SAME fixed VA in every task (VS_TABLE = 0x2080800),
     * so it remains correct even if the library image were shared later. */
    if (h.api_offset != CSO_NO_API && h.api_offset + 4 <= h.image_size) {
        uint32_t va = h.base + h.api_offset;
        uint32_t pa = task_virt_to_phys(&t->space, va);
        if (pa) *(uint32_t *)pa = 0x2080800u;           /* CATOS_VS_TABLE */
    }

    /* ---- relocation pass (block ① / ③) -------------------------------- */
    /* On the first load the shared bookkeeping that the reloc pass reads
     * (meta, rela, export count, span geometry) must be attached to the pool
     * before dl_apply_relocs walks it. */
    if (first_load) {
        sh->base        = h.base;
        sh->span        = span;
        sh->image_size  = h.image_size;
        sh->text_end    = h.text_end;
        sh->data_end    = h.data_end;
        sh->meta        = meta;
        sh->meta_bytes  = meta_bytes;
        sh->rela        = relaf;
        sh->rela_cnt    = h.rela_cnt;
        sh->export_cnt  = h.export_cnt;
    }
    if (!dl_apply_relocs(t, sh, first_load)) {
        for (uint32_t o = 0; o < span; o += 4096) {
            int ro = dl_page_ro(h.text_end, o);
            if (ro && !first_load) task_scrub_page(&t->space, h.base + o);
            else task_unmap_page(&t->space, h.base + o);
        }
        t->vm = vma_remove(t->vm, v);
        vma_free(v);
        if (first_load) {
            for (int i = 0; i < sh->ro_cnt; i++) frame_free(sh->ro_pa[i]);
            sh->ro_cnt = 0;
            if (relaf) frame_free((uint32_t)relaf);
            frame_free((uint32_t)meta);
        }
        return DL_ERR_RELOC;
    }

    /* After the first load has patched the pooled RO span, flip those PTEs
     * read-only (RW=0).  The frame was mapped writable only for the load
     * copy/reloc window. */
    if (first_load) {
        for (int i = 0; i < sh->ro_cnt; i++)
            task_map_page(&t->space, h.base + (uint32_t)i * 4096,
                          sh->ro_pa[i], 0);
        /* evidence: dump each pooled RO frame's physical number. */
        for (int i = 0; i < sh->ro_cnt; i++) {
            ser_write("dl ro frame=");
            ser_hex("pa", sh->ro_pa[i]);
            ser_write(" base="); ser_hex("base", h.base);
            ser_write(" task="); ser_hex("tid", (uint32_t)t->tid);
            ser_write("\n");
        }
    }

    if (first_load) {
        sh->used        = 1;
        sh->cluster     = clus;
        sh->base        = h.base;
        sh->span        = span;
        sh->image_size  = h.image_size;
        sh->bss_size    = h.bss_size;
        sh->text_end    = h.text_end;
        sh->data_end    = h.data_end;
        sh->api_offset  = h.api_offset;
        sh->rela_off    = h.rela_off;
        sh->rela_cnt    = h.rela_cnt;
        sh->init_off    = h.init_array_off;
        sh->init_cnt    = h.init_array_cnt;
        sh->fini_off    = h.fini_array_off;
        sh->fini_cnt    = h.fini_array_cnt;
        sh->fsize       = fsize;
        sh->image_off   = image_off;
        sh->meta        = meta;
        sh->meta_bytes  = meta_bytes;
        sh->rela        = relaf;
        sh->export_cnt  = h.export_cnt;
        for (uint32_t i = 0; i < FS_NAME_MAX - 1 && path[i]; i++)
            sh->path[i] = path[i];
        sh->path[FS_NAME_MAX - 1] = 0;
        sh->refcnt      = 1;
    } else {
        sh->refcnt++;
        ser_write("dl open shared base=");
        ser_hex("base", h.base);
        ser_write(" refcnt="); ser_hex("n", (uint32_t)sh->refcnt);
        ser_write(" task="); ser_hex("tid", (uint32_t)t->tid);
        ser_write("\n");
        for (int i = 0; i < sh->ro_cnt; i++) {
            ser_write("dl ro frame=");
            ser_hex("pa", sh->ro_pa[i]);
            ser_write(" base="); ser_hex("base", h.base);
            ser_write(" task="); ser_hex("tid", (uint32_t)t->tid);
            ser_write(" (shared)\n");
        }
    }

    dl_module_t *m = &dl_mods[slot];
    m->used        = 1;
    m->handle      = dl_next_handle++;
    if (dl_next_handle <= 0) dl_next_handle = 1;
    m->owner_tid   = t->tid;
    m->base        = h.base;
    m->span        = span;
    m->export_cnt  = h.export_cnt;
    m->sh          = sh;

    /* Run constructors in order after relocations are applied. */
    dl_call_array(t, sh, sh->init_off, sh->init_cnt, "init_array");
    return m->handle;
}

void *dl_sym(struct task *t, int handle, const char *name) {
    if (!t || !name || !*name) return 0;
    dl_module_t *m = dl_find(handle, t->tid);
    if (!m || !m->sh) return 0;
    dl_shared_t *sh = m->sh;

    const cso_export_t *ex = (const cso_export_t *)sh->meta;
    const char *names = (const char *)(sh->meta + sh->export_cnt * sizeof(cso_export_t));
    if (sh->meta_bytes < sh->export_cnt * sizeof(cso_export_t)) return 0;

    for (uint32_t i = 0; i < sh->export_cnt; i++) {
        if (ex[i].name_off >= sh->meta_bytes) continue;
        const char *nm = names + ex[i].name_off;
        if (!dl_streq(nm, name)) continue;
        if (ex[i].sym_off >= sh->span) return 0;        /* out of bounds */
        return (void *)(sh->base + ex[i].sym_off);
    }
    return 0;
}

/* Detach this task's mapping of `m`: run destructors, scrub the shared RO
 * PTEs (frames stay in the pool), free this task's private frames, drop the
 * refcount, and release the pool only when the last opener leaves. */
static void dl_detach(struct task *t, dl_module_t *m) {
    dl_shared_t *sh = m->sh;
    if (!sh) { m->used = 0; return; }

    /* Run destructors before the image is unmapped. */
    dl_call_array(t, sh, sh->fini_off, sh->fini_cnt, "fini_array");

    vma_t *v = vma_find(t->vm, m->base);
    if (v && v->kind == VMA_LIB && v->start == m->base) {
        for (uint32_t off = 0; off < m->span; off += 4096) {
            int ro = dl_page_ro(sh->text_end, off);
            if (ro) {
                task_scrub_page(&t->space, m->base + off);  /* keep pooled frame */
            } else {
                task_unmap_page(&t->space, m->base + off); /* free private frame */
            }
        }
        t->vm = vma_remove(t->vm, v);
        vma_free(v);
    }

    sh->refcnt--;
    ser_write("dl close base=");
    ser_hex("base", m->base);
    ser_write(" refcnt="); ser_hex("n", (uint32_t)sh->refcnt);
    ser_write(" task="); ser_hex("tid", (uint32_t)t->tid);
    ser_write("\n");

    if (sh->refcnt <= 0) {
        /* Last opener: release the pooled RO frames and shared metadata. */
        for (int i = 0; i < sh->ro_cnt; i++) frame_free(sh->ro_pa[i]);
        if (sh->meta)  frame_free((uint32_t)sh->meta);
        if (sh->rela)  frame_free((uint32_t)sh->rela);
        ser_write("dl release base=");
        ser_hex("base", m->base);
        ser_write(" pages="); ser_hex("n", (uint32_t)sh->ro_cnt);
        ser_write(" refcnt=0\n");
        sh->used = 0;
        sh->meta = 0;
        sh->rela = 0;
        sh->ro_cnt = 0;
    }
    m->used = 0;
    m->sh = 0;
}

int dl_close(struct task *t, int handle) {
    if (!t) return DL_ERR_BADH;
    dl_module_t *m = dl_find(handle, t->tid);
    if (!m) return DL_ERR_BADH;
    dl_detach(t, m);
    return DL_OK;
}

void dl_close_all(struct task *t) {
    if (!t) return;
    for (int i = 0; i < MAX_DL_MODULES; i++) {
        if (!dl_mods[i].used || dl_mods[i].owner_tid != t->tid) continue;
        dl_detach(t, &dl_mods[i]);
    }
}
