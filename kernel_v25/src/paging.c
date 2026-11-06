/* paging.c - the memory-management pillar of CatOS.
 *
 * The kernel identity-maps the low 64 MiB with 4 MiB PSE pages
 * (supervisor-only).  Every ring3 task gets its own page directory that
 * copies the kernel entries but replaces the 32..36 MiB entry (PDE 8)
 * with a private 4 KiB page table: the task's image, bss, vsyscall page,
 * heap and stack live there with user permissions, so tasks are fully
 * isolated from each other AND from the kernel.
 *
 * Physical frames come from a bitmap allocator over the free RAM between
 * the kernel image and the top of (low) memory.  The PCnet DMA area at
 * 0x500000..0x510000 is reserved (net.c parks its rings there).
 */

#include "paging.h"
#include "cpu.h"
#include "vga.h"

#define KBASE        0x1000000u     /* kernel link base  */
#define MEM_TOP      0x4000000u     /* we manage the low 64 MiB only   */
#define PDE_USER     8u             /* 32..36 MiB = task arena         */
#define DMA_LO       0x500000u      /* PCnet DMA area (see net.c)      */
#define DMA_HI       0x510000u

extern char _bss_end[];             /* end of the flat kernel image     */

/* ---- kernel directory: 16 PSE entries, identity map 0..64 MiB -------- */

static uint32_t kdir[1024] __attribute__((aligned(4096)));
/* ---- frame allocator --------------------------------------------------- */

static uint32_t frames_base;                  /* first managed frame   */
static uint32_t frames_count;
static uint32_t bitmap[MEM_TOP / (4096 * 32)];

static void mark_frame(uint32_t pa, int used) {
    uint32_t idx = (pa - frames_base) >> 12;
    if (used) bitmap[idx >> 5] |= 1u << (idx & 31);
    else      bitmap[idx >> 5] &= ~(1u << (idx & 31));
}

uint32_t frame_alloc(void) {
    for (uint32_t i = 0; i < frames_count; i++) {
        if (!(bitmap[i >> 5] & (1u << (i & 31)))) {
            bitmap[i >> 5] |= 1u << (i & 31);
            return frames_base + (i << 12);
        }
    }
    return 0;
}

void frame_free(uint32_t pa) {
    if (pa < frames_base || pa >= frames_base + (frames_count << 12)) return;
    mark_frame(pa, 0);
}

/* ---- kernel paging init ------------------------------------------------ */

void paging_init(uint32_t mem_lower_kb, uint32_t mem_upper_kb) {
    (void)mem_lower_kb;
    uint32_t top = 0x100000u + (mem_upper_kb ? mem_upper_kb << 10 : MEM_TOP - 0x100000u);
    if (top > MEM_TOP) top = MEM_TOP;

    /* identity-map 0..64 MiB with 4 MiB supervisor pages (P=1 RW=1 PS=1) */
    for (int i = 0; i < 16; i++)
        kdir[i] = (i << 22) | 0x083u;

    /* frames start after the kernel image (bss included) */
    frames_base = ((uint32_t)_bss_end + 0xFFF) & ~0xFFFu;
    if (frames_base < DMA_HI + 0x1000u) frames_base = DMA_HI;  /* (never on this kernel) */
    frames_count = (top - frames_base) >> 12;

    /* the PCnet DMA area must never be handed out */
    for (uint32_t pa = frames_base; pa < DMA_HI; pa += 4096) {
        if (pa >= DMA_LO) mark_frame(pa, 1);
    }
    /* the low frames (0..kernel) are outside the managed range; nothing
     * else to reserve. */

    cpu_paging_enable((uint32_t)kdir);
}

uint32_t paging_kdir(void) { return (uint32_t)kdir; }

void frame_stats(uint32_t *total, uint32_t *used) {
    if (total) *total = frames_count;
    if (used) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < frames_count; i++)
            if (bitmap[i >> 5] & (1u << (i & 31))) n++;
        if (used) *used = n;
    }
}

/* ---- per-task address space --------------------------------------------
 * Layout inside PDE 8 (32..36 MiB), all 4 KiB pages:
 *   0x2000000 image + bss        (user RW)
 *   0x2080000 vsyscall page      (user RO)
 *   0x2100000 heap, grows up     (user RW, on demand)
 *   0x21F0000 stack, top 0x2200000 (user RW)
 * (types and constants live in paging.h)
 */

int  task_space_create(task_space_t *s) {
    s->dir = frame_alloc();
    s->pt  = frame_alloc();
    if (!s->dir || !s->pt) return -1;
    uint32_t *d = (uint32_t *)s->dir;
    uint32_t *t = (uint32_t *)s->pt;
    for (int i = 0; i < 1024; i++) d[i] = 0;
    for (int i = 0; i < 1024; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) d[i] = kdir[i];        /* kernel, supervisor */
    d[PDE_USER] = s->pt | 0x007u;                        /* P RW US */
    return 0;
}

/* map a user virtual page inside PDE 8 to a physical frame */
int task_map_page(task_space_t *s, uint32_t vaddr, uint32_t pa, int user_writable) {
    if ((vaddr >> 22) != PDE_USER) return -1;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    uint32_t *t = (uint32_t *)s->pt;
    t[pte] = (pa & ~0xFFFu) | 0x005u | (user_writable ? 0x002u : 0);
    cpu_invlpg(vaddr);
    return 0;
}

uint32_t task_virt_to_phys(task_space_t *s, uint32_t vaddr) {
    if ((vaddr >> 22) != PDE_USER) return 0;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    uint32_t *t = (uint32_t *)s->pt;
    if (!(t[pte] & 1)) return 0;
    return (t[pte] & ~0xFFFu) + (vaddr & 0xFFF);
}

void task_space_destroy(task_space_t *s) {
    if (!s->pt) return;
    uint32_t *t = (uint32_t *)s->pt;
    for (uint32_t i = 0; i < 1024; i++)
        if (t[i] & 1) frame_free(t[i] & ~0xFFFu);
    frame_free(s->pt);
    if (s->dir) frame_free(s->dir);
    s->pt = 0;
    s->dir = 0;
}

/* ---- helpers exposed to the task layer --------------------------------- */
/* (thin wrappers so task.c does not touch raw frames) */
void *kzalloc_frame(void) {
    uint32_t f = frame_alloc();
    if (!f) return 0;
    uint8_t *p = (uint8_t *)f;
    for (uint32_t i = 0; i < 4096; i++) p[i] = 0;
    return p;
}

/* ---- VBE linear framebuffer -------------------------------------------
 * The low 64 MiB is 16 PSE (4 MiB) pages.  A VBE LFB lives far above it
 * (typically PCI BAR2 at 0xE0000000 / 0x10000000), so it needs its own
 * 4 KiB page tables.  We refuse to touch PDEs 0..15: splitting one of
 * those would expose the framebuffer to every ring3 task, which would
 * break the "no user drawing API" policy. */
void *vbe_lfb_map(uint32_t phys, uint32_t size) {
    if (!phys || !size) return 0;
    uint32_t pa  = phys & ~0xFFFu;
    uint32_t end = (phys + size + 0xFFFu) & ~0xFFFu;
    uint32_t pde0 = pa >> 22, pde1 = (end - 1) >> 22;
    if (pde0 < 16 || pde1 < 16) return 0;

    for (uint32_t pde = pde0; pde <= pde1; pde++) {
        if (kdir[pde] & 0x80u) return 0;      /* would split a PSE page */
        if (!(kdir[pde] & 1u)) {
            uint32_t pt = frame_alloc();
            if (!pt) return 0;
            uint32_t *t = (uint32_t *)pt;
            for (int i = 0; i < 1024; i++) t[i] = 0;
            kdir[pde] = pt | 0x003u;         /* P | RW, supervisor-only */
        }
    }
    for (; pa < end; pa += 4096) {
        uint32_t *t = (uint32_t *)(kdir[pa >> 22] & ~0xFFFu);
        t[(pa >> 12) & 0x3FFu] = pa | 0x003u;
    }
    cpu_cr3_write((uint32_t)kdir);            /* flush TLB */
    return (void *)phys;
}
