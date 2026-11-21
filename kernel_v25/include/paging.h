#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* paging.c - frame allocator + per-task address spaces (see cpu.h for the
 * descriptor/interrupt side).
 *
 * v2.6 P0: VMA (virtual memory area) infrastructure replaces the old
 * fixed layout (image/heap/stack at hardcoded offsets).  Each ring3
 * task now carries a sorted linked list of VMA descriptors describing
 * every mapped region in its address space.  The CE loader still lays
 * out image/vsyscall/stack at the legacy offsets so all existing .ce
 * binaries keep working unchanged; new ELF loaders will build VMA lists
 * from PT_LOAD entries instead.
 */

void     paging_init(uint32_t mem_lower_kb, uint32_t mem_upper_kb);
uint32_t paging_kdir(void);
uint32_t paging_phys_top(void);   /* v2.67: end of the identity-mapped window */

uint32_t frame_alloc(void);
void     frame_stats(uint32_t *total, uint32_t *used);   /* 4 KiB frames */
void     frame_free(uint32_t pa);
void    *kzalloc_frame(void);          /* one zeroed frame, kernel-virtual */

typedef struct task_space {
    uint32_t dir;
    uint32_t pt;        /* user page table for PDE 8  (0x2000000..0x2400000) */
    uint32_t pt2;       /* user page table for PDE 9  (0x2400000..0x2800000),
                         * ELF images and anonymous mmap live here */
} task_space_t;

int      task_space_create(task_space_t *s);
int      task_map_page(task_space_t *s, uint32_t vaddr, uint32_t pa, int user_writable);
int      task_unmap_page(task_space_t *s, uint32_t vaddr);   /* frees the frame */
uint32_t task_virt_to_phys(task_space_t *s, uint32_t vaddr);
void     task_space_destroy(task_space_t *s);

/* ---- VMA type and management ----------------------------------------- */
/* vma_kind values for vma_new(). */
#define VMA_IMAGE  0        /* program code/data (read-write or read-only) */
#define VMA_VSYSCALL 1      /* per-task syscall thunk page (read-only) */
#define VMA_ANON   2        /* anonymous: heap, stack, mmap'd pages */
#define VMA_MMAP   3        /* file-backed mmap (future ELF loader) */

typedef struct vma {
    uint32_t  start;     /* inclusive */
    uint32_t  end;       /* exclusive */
    uint8_t   kind;
    uint8_t   writable;  /* 0 = read-only (e.g. code page, vsyscall) */
    uint8_t   guard_pages; /* leading pages kept unmapped (mmap only) */
    uint8_t   ref_count;    /* for future COW / shared maps */
    struct vma *next;    /* sorted by start ascending */
} vma_t;

/* allocate an empty VMA node (kernel heap; never fails under normal load) */
vma_t *vma_new(uint32_t start, uint32_t end, uint8_t kind, uint8_t writable);
/* destroy a VMA node and free its (optional) frame(s) if the VMA is empty */
void   vma_free(vma_t *v);
/* insert `v` into `list` in address-order; asserts no overlap with
 * existing VMAs (caller must check before calling).  Returns the new
 * head of the list. */
vma_t *vma_insert(vma_t *list, vma_t *v);
/* remove `v` from `list`; returns the new head. */
vma_t *vma_remove(vma_t *list, vma_t *v);
/* find the VMA containing `addr`; returns NULL if not in any region. */
vma_t *vma_find(const vma_t *list, uint32_t addr);
/* walk `list` and call `fn(user_data, vma)` for every entry. */
void   vma_foreach(const vma_t *list,
                   void (*fn)(void *ud, const vma_t *v), void *ud);

/* check whether `addr` lies in any VMA in the list. */
int    vma_contains(const vma_t *list, uint32_t addr);

/* ---- legacy layout constants (kept for CE compat only; new loaders
 *      must not depend on them) ---------------------------------------- */
#define USER_IMAGE_BASE 0x2000000u
#define USER_VSYSCALL   0x2080000u
#define USER_HEAP_BASE  0x2100000u
/* P0-3: restored to the original project's 512 KiB.  The v2.6 VMA
 * refactor had shrunk it to 256 KiB, which starved any CE program
 * that keeps a large working buffer (e.g. the Go interpreter's arena
 * or the cc self-compiler's source/token tables).  The heap lives in
 * PDE 8 (0x2100000..0x2400000) and must stay below USER_ARENA_END so
 * it never reaches the mmap extension arena in PDE 9. */
#define USER_HEAP_MAX   0x80000u       /* 512 KiB */
#define USER_STACK_TOP  0x2200000u
/* P0-3: restored to the original project's 128 KiB (was 64 KiB).
 * 0x21e0000..0x2200000 -- still inside PDE 8, and still clear of the
 * heap (which ends at 0x2180000). */
#define USER_STACK_SZ   0x20000u       /* 128 KiB */
#define USER_ARENA_END  0x2400000u

/* ---- extension arena: PDE 9 (36..40 MiB, one 4 KiB page table) --------
 * Home of ELF images (PIE, ASLR-slid base) and anonymous mmap regions.
 * All of it is lazy-mapped: pages appear on first touch via the VMA
 * page-fault path. */
#define USER_EXT_BASE   0x2400000u
#define USER_EXT_END    0x2800000u
#define USER_EXT_SIZE   (USER_EXT_END - USER_EXT_BASE)
#define PDE_USER_EXT    9u

/* ---- VBE linear framebuffer -------------------------------------------
 * Map a PCI MMIO framebuffer into the KERNEL page directory (identity,
 * supervisor-only, 4 KiB pages).  Returns the usable virtual address
 * (== phys) or 0 on failure.  The high PDEs are ALSO mirrored into
 * every ring3 directory as supervisor-only entries, so the kernel can
 * draw while running on a caller's CR3; user code still cannot reach
 * VRAM (no US bit), which keeps the framebuffer supervisor-only. */
void *vbe_lfb_map(uint32_t phys, uint32_t size);
/* Keep one ring3 directory in sync with the LFB PDEs (called at the top
 * of isr_syscall).  No-op once synchronised; flushes TLB on a change. */
void paging_sync_lfb_dir(uint32_t dir_phys);

#endif
