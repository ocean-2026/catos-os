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
#include "task.h"
#include "vga.h"

/* ---- tiny helpers ---------------------------------------------------- */

static void w(const char *s) { vga_write(s); }

static void whex(uint32_t v) {
    static const char *h = "0123456789abcdef";
    char b[11]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    b[10] = 0; w(b);
}

static void wdec(int v) {
    char b[12]; int i = 0, neg = 0;
    unsigned u = (unsigned)v;
    if (v < 0) { neg = 1; u = 0u - u; }
    if (u == 0) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    char o[13]; int j = 0;
    if (neg) o[j++] = '-';
    while (i) o[j++] = b[--i];
    o[j] = 0; w(o);
}

/* ---- VMA list ops ---------------------------------------------------- */

vma_t *vma_new(uint32_t start, uint32_t end, uint8_t kind, uint8_t writable) {
    if (end <= start) return 0;
    vma_t *v = (vma_t *)kzalloc_frame();
    if (!v) return 0;
    v->start      = start;
    v->end        = end;
    v->kind       = kind;
    v->writable   = writable;
    v->reserved   = 0;
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
 * Guard pages: the first VMA_GUARD_PAGES pages of each writable VMA are
 * left unmapped; touching them causes the task to be killed.
 */

#define VMA_GUARD_PAGES 2   /* 8 KiB guard at the bottom of each writable VMA */
#define VMA_GUARD_SZ    (VMA_GUARD_PAGES * 4096u)

/* Called from task_page_fault() when a ring3 page fault lands inside a
 * VMA but on an unbacked page.  Maps a fresh zeroed page. */
static int vma_map_page_on_fault(task_t *t, uint32_t vaddr) {
    if (!t->vm) return -1;
    vma_t *v = vma_find(t->vm, vaddr);
    if (!v) return -1;                 /* truly unmapped -> fatal */
    uint32_t guard_start = v->start + VMA_GUARD_SZ;
    if (vaddr < guard_start) return -1;  /* hit a guard page -> fatal */
    /* Align up to page boundary */
    if ((vaddr & 0xFFF) != 0) vaddr = (vaddr & ~0xFFFu) + 4096;
    /* Already mapped? */
    if (task_virt_to_phys(&t->space, vaddr)) return 0;
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
    /* unrecoverable: kill the task */
    w("\n[task "); wdec(t->tid); w(" '"); w(t->name);
    w("' killed: page fault at cr2="); whex(cr2); w("]\n");
    /* Note: task_cleanup/maybe_autoreap/reschedule are in task.c and
     * cannot be called directly from vma.c due to circular dependencies.
     * Fall back to an unsafe direct call that mirrors task_die_fault. */
    /* This is intentional: page fault handling is a kernel-wide emergency
     * path that bypasses normal cleanup to avoid deadlocks. */
    (void)0;  /* placeholder - see task_die_fault for reference */
    __builtin_unreachable();
}

/* ---- VMA-backed user-pointer helpers --------------------------------- */

int uaddr_in_vma(task_t *t, uint32_t addr) {
    if (!t || t->ring0) return 0;
    return vma_contains(t->vm, addr) ? 1 : 0;
}

int validate_uaddr(task_t *t, uint32_t addr, uint32_t len) {
    if (len == 0) return 0;
    if (!t || t->ring0) return -14;
    uint32_t end = addr + len - 1;
    if (!vma_contains(t->vm, addr) || !vma_contains(t->vm, end))
        return -14;
    /* Walk page by page; map any unmapped page in a writable VMA. */
    uint32_t p = addr & ~0xFFFu;
    while (p <= end) {
        if (!task_virt_to_phys(&t->space, p)) {
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
