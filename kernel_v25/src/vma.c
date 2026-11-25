/* vma.c - per-task virtual memory area list + page-fault-driven lazy mapping.
 *
 * Every ring3 task owns a singly-linked, address-sorted list of VMA nodes
 * describing every mapped region in its 32-bit virtual address space.
 *
 * Why not a tree?  With at most ~64 VMAs per task (CE image + vsyscall +
 * heap + stack + a few mmaps), a linear scan is fast enough and keeps
 * the code tiny (~200 lines) for an educational OS.
 *
 * Thread safety: this runs in ring0 context (task creation / cleanup /
 * syscalls), never re-entrant, so no locking is needed.
 */

#include "paging.h"
#include "vma.h"
#include "task.h"
#include "vga.h"

/* ---- VMA list ops ---------------------------------------------------- */

vma_t *vma_new(uint32_t start, uint32_t end, uint8_t kind, uint8_t writable) {
    if (end <= start) return 0;
    vma_t *v = (vma_t *)kzalloc_frame();
    if (!v) return 0;
    v->start      = start;
    v->end        = end;
    v->kind       = kind;
    v->writable   = writable;
    v->guard_pages = 0;
    v->ref_count  = 1;
    v->next       = 0;
    return v;
}

void vma_free(vma_t *v) {
    if (!v) return;
    frame_free((uint32_t)v);
}

vma_t *vma_insert(vma_t *list, vma_t *v) {
    if (!list || v->start < list->start) {
        v->next = list;
        return v;
    }
    vma_t *prev = list;
    while (prev->next && prev->next->start <= v->start)
        prev = prev->next;
    v->next = prev->next;
    prev->next = v;
    return list;
}

vma_t *vma_remove(vma_t *list, vma_t *v) {
    if (!list) return 0;
    if (list == v) { list = list->next; v->next = 0; return list; }
    vma_t *prev = list;
    while (prev->next && prev->next != v) prev = prev->next;
    if (prev->next == v) { prev->next = v->next; v->next = 0; }
    return list;
}

vma_t *vma_find(const vma_t *list, uint32_t addr) {
    for (const vma_t *v = list; v; v = v->next)
        if (addr >= v->start && addr < v->end) return (vma_t *)v;
    return 0;
}

int vma_contains(const vma_t *list, uint32_t addr) {
    return vma_find(list, addr) != 0;
}

void vma_foreach(const vma_t *list, void (*fn)(void *ud, const vma_t *v), void *ud) {
    for (const vma_t *v = list; v; v = v->next) fn(ud, v);
}

/* ---- page-fault-driven lazy mapping ---------------------------------- */
/*
 * When a user task accesses a virtual address that is inside a VMA but
 * not yet backed by a physical frame, the CPU raises a page fault (INT
 * 0xE).  The kernel's fault handler (task_page_fault) allocates a zeroed
 * page on demand, maps it, and returns so the faulting instruction
 * retries.
 *
 * Guard pages: only mmap() regions (guard_pages = VMA_GUARD_PAGES) keep
 * their first VMA_GUARD_PAGES pages unmapped; the pointer mmap() returns
 * sits just past them, so an underrun kills the task.  Heap, stack and
 * image VMAs have guard_pages = 0 -- their first page is a real page.
 */

#define VMA_GUARD_PAGES 2   /* 8 KiB guard at the bottom of each writable VMA */
#define VMA_GUARD_SZ    (VMA_GUARD_PAGES * 4096u)

/* Called from task_page_fault() when a ring3 page fault lands inside a
 * VMA but on an unbacked page.  Maps a fresh zeroed page. */
static int vma_map_page_on_fault(task_t *t, uint32_t vaddr) {
    if (!t->vm) return -1;
    vma_t *v = vma_find(t->vm, vaddr);
    if (!v) return -1;                 /* truly unmapped -> fatal */
    /* Issue #78: the 8 KiB guard belongs to mmap() regions only, where
     * the returned pointer sits 8 KiB past the VMA start.  Heap, stack and
     * image VMAs start at their first USABLE page; treating their first two
     * pages as guard made validate_uaddr() unable to fault in page 2 of a
     * heap buffer, so every n-byte syscall copy was rejected. */
    uint32_t guard_start = v->start + (uint32_t)v->guard_pages * 4096u;
    if (vaddr < guard_start) return -1;  /* hit a guard page -> fatal */
    /* Align DOWN to the containing page.  The old "align up" moved a
     * non-page-aligned CR2 to the NEXT page, so the retried instruction
     * faulted again on the page that was actually requested. */
    vaddr &= ~0xFFFu;
    /* Already mapped by a user PTE?  A supervisor-only #D11 mirror PTE does
     * not count: ring3 still cannot reach it, so remap to a user frame. */
    if (task_page_is_user(&t->space, vaddr)) return 0;
    uint32_t pa = (uint32_t)kzalloc_frame();
    if (!pa) return -1;
    if (task_map_page(&t->space, vaddr, pa, v->writable ? 1 : 0) != 0) {
        frame_free(pa);
        return -1;
    }
    return 0;
}

/* Called from isr_exception() when a ring3 task faults.
 * Returns:
 *   0   - fault handled (mapped the page), task resumes via iret
 *  -14  - fatal (guard page / OOB / OOM), task is killed
 */
int task_page_fault(int_frame_t *f) {
    uint32_t cr2 = cpu_cr2_read();
    (void)f;
    task_t *t = task_current();
    if (!t || t->ring0) return -14;
    if (vma_map_page_on_fault(t, cr2) == 0) return 0;  /* retry the faulting instruction */
    /* TD-01 (fixed): unrecoverable fault.  Return -14 and let the
     * exception dispatcher (intr.c) route to task_die_fault(), which
     * performs task_cleanup + zombie + reschedule.  The old code here
     * printed the kill banner and then hit __builtin_unreachable()
     * without ever returning, so the task was neither cleaned up nor
     * killed -- and control could fall into garbage code.  No circular
     * dependency exists: task.c is the caller's caller, not a callee. */
    return -14;
}

/* ---- VMA-backed user-pointer helpers --------------------------------- */

int uaddr_in_vma(task_t *t, uint32_t addr) {
    if (!t || t->ring0) return 0;
    return vma_contains(t->vm, addr) ? 1 : 0;
}

int validate_uaddr(task_t *t, uint32_t addr, uint32_t len) {
    if (len == 0) return 0;
    if (!t || t->ring0) return -14;
    /* Issue #78: reject ranges whose end wraps past 2^32.  Otherwise
     * addr + len - 1 can land on a low address that happens to be inside
     * a VMA, and the page-walk loop below is silently skipped. */
    if (len - 1 > 0xFFFFFFFFu - addr) return -14;
    uint32_t end = addr + len - 1;
    if (!vma_contains(t->vm, addr) || !vma_contains(t->vm, end))
        return -14;
    /* Walk page by page; map any unmapped page in a writable VMA. */
    uint32_t p = addr & ~0xFFFu;
    while (p <= end) {
        if (!task_page_is_user(&t->space, p)) {
            if (vma_map_page_on_fault(t, p) != 0) return -14;
        }
        p += 4096;
    }
    return 0;
}

int copy_to_user(task_t *t, void *dst, const void *src, uint32_t len) {
    if (validate_uaddr(t, (uint32_t)dst, len) != 0) return -14;
    const uint8_t *sp = (const uint8_t *)src;
    uint32_t d = (uint32_t)dst;
    while (len) {
        uint32_t pa = task_virt_to_phys(&t->space, d & ~0xFFFu);
        if (!pa) return -14;
        uint32_t off = d & 0xFFF;
        uint32_t n = 4096 - off; if (n > len) n = len;
        uint8_t *dp = (uint8_t *)(pa + off);
        for (uint32_t i = 0; i < n; i++) dp[i] = sp[i];
        d += n; sp += n; len -= n;
    }
    return 0;
}

int copy_from_user(task_t *t, void *dst, const void *src, uint32_t len) {
    if (validate_uaddr(t, (uint32_t)src, len) != 0) return -14;
    uint8_t *dp = (uint8_t *)dst;
    uint32_t s = (uint32_t)src;
    while (len) {
        uint32_t pa = task_virt_to_phys(&t->space, s & ~0xFFFu);
        if (!pa) return -14;
        uint32_t off = s & 0xFFF;
        uint32_t n = 4096 - off; if (n > len) n = len;
        const uint8_t *sp = (const uint8_t *)(pa + off);
        for (uint32_t i = 0; i < n; i++) dp[i] = sp[i];
        s += n; dp += n; len -= n;
    }
    return 0;
}

/* ---- anonymous mmap / munmap ------------------------------------------
 * Regions live in the extension arena (PDE 9, 36..40 MiB) so they can
 * coexist with the CE legacy layout in PDE 8.  Pages are mapped lazily
 * by the page-fault path; a munmap'd page is unmapped and freed.
 * Layout per mapping: [guard 8 KiB | user region | (tail slack)]
 * The returned address sits after the guard, so an underrun write hits
 * an unbacked guard page and kills the task instead of a neighbour. */

#include "task.h"

uint32_t vma_mmap_anon(task_t *t, uint32_t len, int writable) {
    if (!t || len == 0) return 0;
    /* first-fit gap scan over the (start-sorted) VMA list, restricted
     * to the anonymous part of the extension arena; the CSO1 library
     * zone (USER_LIB_BASE..USER_LIB_END) sits above and is excluded.
     * The returned base needs room for the guard pages plus len,
     * page-aligned */
    uint32_t want = VMA_GUARD_SZ + ((len + 0xFFFu) & ~0xFFFu);
    uint32_t cur = USER_EXT_BASE;
    while (1) {
        uint32_t next = USER_LIB_BASE;
        for (vma_t *x = t->vm; x; x = x->next)
            if (x->start >= cur && x->start < next) next = x->start;
        if (next - cur >= want && cur + want <= USER_LIB_BASE) {
            uint32_t base = cur;
            vma_t *nv = vma_new(base, base + want, VMA_ANON, writable ? 1 : 0);
            if (!nv) return 0;
            /* Only mmap() regions carry the leading guard pages. */
            nv->guard_pages = VMA_GUARD_PAGES;
            t->vm = vma_insert(t->vm, nv);
            return base + VMA_GUARD_SZ;
        }
        if (next >= USER_LIB_BASE) return 0;     /* anon arena exhausted */
        cur = next;
        /* skip past the whole VMA that starts at cur */
        for (vma_t *x = t->vm; x; x = x->next)
            if (x->start == cur && x->end > cur) { cur = x->end; break; }
        if (cur >= USER_LIB_BASE) return 0;
    }
}

int vma_munmap(task_t *t, uint32_t addr, uint32_t len) {
    if (!t || len == 0) return -22;
    if (addr < USER_EXT_BASE || addr + ((len + 0xFFFu) & ~0xFFFu) > USER_LIB_BASE)
        return -22;                             /* only ext-arena anon mappings */
    uint32_t start = addr & ~0xFFFu;
    uint32_t end   = addr + ((len + 0xFFFu) & ~0xFFFu);
    /* the whole range must lie inside ONE mapping we created */
    vma_t *v = vma_find(t->vm, start);
    if (!v || v->kind != VMA_ANON || end > v->end || start < v->start + VMA_GUARD_SZ)
        return -22;
    for (uint32_t p = start; p < end; p += 4096)
        task_unmap_page(&t->space, p);          /* frees frames; lazy pages are fine */
    if (start == v->start + VMA_GUARD_SZ && end == v->end) {
        /* whole region gone: drop the VMA (incl. its guard pages) */
        t->vm = vma_remove(t->vm, v);
        vma_free(v);
    } else {
        v->start = end;                          /* partial: shrink the region */
    }
    return 0;
}
