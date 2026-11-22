/* paging.c - the memory-management pillar of CatOS.
 *
 * The kernel identity-maps the installed RAM with 4 MiB PSE pages
 * (supervisor-only).  v2.38 P0: the managed range follows the multiboot
 * mem_upper report instead of being clamped to the low 64 MiB; the ceiling
 * is the 4 GiB 32-bit physical address space.  Every ring3 task gets its own
 * page directory that copies the kernel entries but replaces the 32..36 MiB
 * entry (PDE 8) with a private 4 KiB page table: the task's image, bss,
 * vsyscall page, heap and stack live there with user permissions, so tasks
 * are fully isolated from each other AND from the kernel.
 *
 * Physical frames come from a bitmap allocator over the free RAM between
 * the kernel image and the top of reported memory.  The PCnet DMA area at
 * 0x500000..0x510000 is reserved (net.c parks its rings there).
 *
 * Known boundary limit (see docs/ROADMAP honest list): this still assumes
 * "kernel virtual == physical" for every allocated frame; RAM sitting
 * above the PCI hole (a second multiboot mmap region on large guests) is
 * not enumerated, and a true 4 GiB guest can place PCI BARs inside the
 * last 4 GiB block -- those cases need the per-frame kernel window that
 * belongs to the deferred memory-dynamicization work.
 */

#include "paging.h"
#include "cpu.h"
#include "vga.h"

#define KBASE        0x1000000u     /* kernel link base  */
#define MEM_TOP      0x4000000u     /* legacy fallback: low 64 MiB      */
#define MAX_PHY      0x100000000ull /* 4 GiB physical address ceiling    */
#define PDE_USER     8u             /* 32..36 MiB = task arena         */
#define DMA_LO       0x500000u      /* PCnet DMA area (see net.c)      */
/* 0x510000..0x528000 is the SB16 ISA DMA buffer (SND_DMA_PHYS in
 * include/snd.h).  It must stay out of the frame allocator: the 8237
 * page register is only 8 bits, so this buffer has to live below 16 MiB.
 * Both reservations are below frames_base on this kernel (which is
 * linked at 16 MiB), but recording the upper bound here keeps the
 * invariant explicit if the kernel load address ever moves. */
#define DMA_HI       0x528000u

extern char _bss_end[];             /* end of the flat kernel image     */

/* ---- kernel directory: PSE entries identity-mapping the installed RAM -- */

static uint32_t kdir[1024] __attribute__((aligned(4096)));

/* ---- v2.12: VBE LFB PDE mirroring --------------------------------------
 * A syscall executes on the CALLER's CR3, not kdir.  The VBE LFB lives
 * in a high PDE (>= 16) that ring3 directories historically did not
 * carry, so the kernel #PF'd (and triple-faulted) the moment it drew
 * VRAM on behalf of a user task -- e.g. gfx_rect/gfx_blit, or even
 * con_write() in graphics mode.  Mirror those PDEs into EVERY task
 * directory.  The PTEs stay supervisor-only (0x003, no US bit), so ring3
 * still cannot read or write the framebuffer; only the kernel can. */
static uint32_t lfb_pde_first, lfb_pde_last;
static int      lfb_pdes_ready;

static int sync_lfb_pdes(uint32_t *d) {
    if (!d || !lfb_pdes_ready || d == kdir) return 0;
    int changed = 0;
    for (uint32_t i = lfb_pde_first; i <= lfb_pde_last; i++) {
        if (d[i] != kdir[i]) { d[i] = kdir[i]; changed = 1; }
    }
    return changed;
}

/* Called from isr_syscall() for ring3 callers: cheap no-op once the
 * directory is in sync; flushes the ACTIVE directory only on a change. */
void paging_sync_lfb_dir(uint32_t dir_phys) {
    if (!dir_phys) return;
    if (sync_lfb_pdes((uint32_t *)dir_phys))
        cpu_cr3_write(dir_phys);
}

/* ---- frame allocator --------------------------------------------------- */

/* bitmap sized for the full 4 GiB space: 1 MiB frames / 32 bits = 32768
 * words (128 KiB BSS).  Only frames_base..mem_top are ever touched. */
#define FRAME_MAX   (MAX_PHY / 4096u)

static uint32_t frames_base;                  /* first managed frame   */
static uint32_t frames_count;
static uint32_t mem_top;                     /* exclusive phys bound  */
static uint32_t bitmap[FRAME_MAX / 32u];

static void mark_frame(uint32_t pa, int used) {
    uint32_t idx = (pa - frames_base) >> 12;
    if (idx >= frames_count) return;
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
    if (pa < frames_base || pa >= mem_top) return;
    mark_frame(pa, 0);
}

/* ---- kernel paging init ------------------------------------------------ */

/* v2.67: highest physical address the kernel identity-maps.  Callers that
 * dereference firmware-provided pointers (ACPI tables, SMBIOS) MUST bound
 * them with this before reading, or a bogus pointer becomes a #PF. */
uint32_t paging_phys_top(void) { return mem_top; }

void paging_init(uint32_t mem_lower_kb, uint32_t mem_upper_kb) {
    (void)mem_lower_kb;
    /* KB -> bytes in 64-bit math: near 4 GiB, mem_upper_kb << 10 would
     * overflow a 32-bit unsigned.  mem_upper counts RAM above 1 MiB. */
    uint64_t top64;
    if (mem_upper_kb)
        top64 = 0x100000ull + (uint64_t)mem_upper_kb * 1024ull;
    else
        top64 = MEM_TOP;                       /* legacy fallback */
    if (top64 > MAX_PHY) top64 = MAX_PHY;
    /* 4 GiB itself is not representable in a 32-bit byte address; clamp to
     * the last accessible 4 KiB page. */
    uint32_t top = (top64 >= MAX_PHY) ? 0xFFFFF000u : (uint32_t)top64;

    /* identity-map the installed RAM with 4 MiB supervisor PSE pages
     * (P=1 RW=1 PS=1).  The trailing partial 4 MiB block is mapped whole,
     * matching the old 16-PSE behavior; the allocator never hands out
     * frames at/above `top`, so that tail is never touched. */
    uint32_t pse_blocks = top >> 22;
    if (top & 0x3FFFFFu) pse_blocks++;
    if (pse_blocks > 1024) pse_blocks = 1024;
    for (uint32_t i = 0; i < pse_blocks; i++)
        kdir[i] = (i << 22) | 0x083u;

    /* frames start after the kernel image (bss included) */
    frames_base = ((uint32_t)_bss_end + 0xFFF) & ~0xFFFu;
    if (frames_base < DMA_HI + 0x1000u) frames_base = DMA_HI;  /* (never on this kernel) */
    mem_top = top;
    frames_count = (top > frames_base) ? (top - frames_base) >> 12 : 0;

    /* the PCnet DMA area must never be handed out */
    for (uint32_t pa = frames_base; pa < DMA_HI; pa += 4096) {
        if (pa >= DMA_LO) mark_frame(pa, 1);
    }
    /* the low frames (0..kernel) are outside the managed range; nothing
     * else to reserve. */

    /* #D10: keep kernel-side allocations (VMA nodes, page directories /
     * tables, per-task scratch frames) out of the physical slice that
     * matches the user arena virtual range [USER_IMAGE_BASE, USER_EXT_END).
     *
     * Frames are handed out by physical address and the kernel dereferences
     * them as identity-mapped virtual pointers.  Under a ring3 task's CR3
     * the 0x2000000..0x2800000 range translates through the task's private
     * page tables (PDE 8/9), not the kdir identity map.  A kernel object
     * living at a physical address inside that range would alias whatever
     * the task has mapped at the same virtual offset -- e.g. a VMA node read
     * back as the task's image bytes, corrupting validate_uaddr().  The
     * task's own image/stack/heap frames may still use physical addresses
     * outside this slice; they are reached purely through their PT entries,
     * so physical == virtual is not required there. */
    {
        uint32_t ua_lo = USER_IMAGE_BASE;
        uint32_t ua_hi = USER_EXT_END;
        for (uint32_t pa = ua_lo; pa < ua_hi; pa += 4096) {
            if (pa >= frames_base && pa < mem_top) mark_frame(pa, 1);
        }
    }

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
    if (!s->dir) return -1;
    s->pt  = frame_alloc();
    s->pt2 = frame_alloc();
    if (!s->pt || !s->pt2) {              /* don't leak on partial failure */
        if (s->pt)  frame_free(s->pt);
        if (s->pt2) frame_free(s->pt2);
        frame_free(s->dir);
        s->dir = s->pt = s->pt2 = 0;
        return -1;
    }
    uint32_t *d  = (uint32_t *)s->dir;
    uint32_t *t  = (uint32_t *)s->pt;
    uint32_t *t2 = (uint32_t *)s->pt2;
    for (int i = 0; i < 1024; i++) d[i] = 0;
    for (int i = 0; i < 1024; i++) t[i] = 0;
    for (int i = 0; i < 1024; i++) t2[i] = 0;
    /* mirror every kernel PDE (low PSE RAM map + any high LFB PTs); the
     * copied entries stay supervisor-only (no US bit).  The two user PDEs
     * are replaced below with this task's private page tables. */
    for (int i = 0; i < 1024; i++) d[i] = kdir[i];
    d[PDE_USER]     = s->pt  | 0x007u;                   /* P RW US */
    d[PDE_USER_EXT] = s->pt2 | 0x007u;                   /* P RW US */
    /* New directory, not loaded yet: mirror but do NOT flush TLB. */
    sync_lfb_pdes(d);
    return 0;
}

/* page-table entry for a user vaddr, or NULL if outside the two
 * user PDEs (8: legacy arena, 9: ELF/mmap extension) */
static uint32_t *pt_for(task_space_t *s, uint32_t vaddr) {
    uint32_t pde = vaddr >> 22;
    if (pde == PDE_USER)     return (uint32_t *)s->pt;
    if (pde == PDE_USER_EXT) return (uint32_t *)s->pt2;
    return 0;
}

/* map a user virtual page (PDE 8 or 9) to a physical frame */
int task_map_page(task_space_t *s, uint32_t vaddr, uint32_t pa, int user_writable) {
    uint32_t *t = pt_for(s, vaddr);
    if (!t) return -1;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    t[pte] = (pa & ~0xFFFu) | 0x005u | (user_writable ? 0x002u : 0);
    cpu_invlpg(vaddr);
    return 0;
}

/* unmap a user page and free its frame (mmap/munmap support) */
int task_unmap_page(task_space_t *s, uint32_t vaddr) {
    uint32_t *t = pt_for(s, vaddr);
    if (!t) return -1;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    if (!(t[pte] & 1)) return -1;
    frame_free(t[pte] & ~0xFFFu);
    t[pte] = 0;
    cpu_invlpg(vaddr);
    return 0;
}

uint32_t task_virt_to_phys(task_space_t *s, uint32_t vaddr) {
    uint32_t *t = pt_for(s, vaddr);
    if (!t) return 0;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    if (!(t[pte] & 1)) return 0;
    return (t[pte] & ~0xFFFu) + (vaddr & 0xFFF);
}

void task_space_destroy(task_space_t *s) {
    if (!s->pt) return;
    uint32_t *t = (uint32_t *)s->pt;
    for (uint32_t i = 0; i < 1024; i++)
        if (t[i] & 1) frame_free(t[i] & ~0xFFFu);
    frame_free(s->pt);
    if (s->pt2) {
        uint32_t *t2 = (uint32_t *)s->pt2;
        for (uint32_t i = 0; i < 1024; i++)
            if (t2[i] & 1) frame_free(t2[i] & ~0xFFFu);
        frame_free(s->pt2);
    }
    if (s->dir) frame_free(s->dir);
    s->pt = 0;
    s->pt2 = 0;
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
 * The installed RAM is mapped with PSE (4 MiB) pages.  A VBE LFB lives in
 * the PCI hole, above the RAM top (PCI BAR0 at 0xfd000000 on QEMU std
 * VGA), so it needs its own 4 KiB page tables.  A PSE PDE that already
 * backs RAM is never split.  Since v2.12 the high PDEs are mirrored into
 * every task directory as SUPERVISOR-ONLY entries so the kernel can draw
 * while running on a ring3 caller's CR3. */
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
            /* P | RW | PCD(=UC): PCI MMIO/framebuffer must be uncacheable.
             * QEMU ignores the cache bit, but real VMware SVGA/e1000 present
             * the framebuffer (and answer MMIO) only when the guest's writes
             * actually reach physical memory -- a write-back mapping makes
             * the device read stale/zero memory, i.e. a black screen. */
            kdir[pde] = pt | 0x013u;         /* P | RW | PCD, supervisor-only */
        }
    }
    for (; pa < end; pa += 4096) {
        uint32_t *t = (uint32_t *)(kdir[pa >> 22] & ~0xFFFu);
        t[(pa >> 12) & 0x3FFu] = pa | 0x013u;  /* UC, so device sees CPU writes */
    }
    /* v2.20: the VMware SVGA II driver maps TWO separate PCI BARs (the
     * framebuffer and the command FIFO).  Keep the union of all mapped
     * high PDEs so both survive in every ring3 directory. */
    if (!lfb_pdes_ready) {
        lfb_pde_first = pde0;
        lfb_pde_last  = pde1;
        lfb_pdes_ready = 1;
    } else {
        if (pde0 < lfb_pde_first) lfb_pde_first = pde0;
        if (pde1 > lfb_pde_last)  lfb_pde_last  = pde1;
    }
    /* Also fix up whichever directory is loaded RIGHT NOW.  A display
     * mode switch can come from a ring3 syscall, in which case this is
     * the caller's directory, not kdir -- and the very next instruction
     * (clear/redraw) writes VRAM.  Never switch CR3 to kdir here: that
     * would strand the syscall on the wrong address space. */
    uint32_t cur = cpu_cr3_read();
    sync_lfb_pdes((uint32_t *)cur);
    cpu_cr3_write(cur);                       /* flush the ACTIVE dir */
    return (void *)phys;
}
