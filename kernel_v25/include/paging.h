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

uint32_t frame_alloc(void);
void     frame_stats(uint32_t *total, uint32_t *used);   /* 4 KiB frames */
void     frame_free(uint32_t pa);
void    *kzalloc_frame(void);          /* one zeroed frame, kernel-virtual */

typedef struct task_space {
    uint32_t dir;
    uint32_t pt;
} task_space_t;

int      task_space_create(task_space_t *s);
int      task_map_page(task_space_t *s, uint32_t vaddr, uint32_t pa, int user_writable);
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
    uint8_t   reserved;
    uint8_t   ref_count; /* for future COW / shared maps */
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
#define USER_HEAP_MAX   0x40000u       /* 256 KiB */
#define USER_STACK_TOP  0x2200000u
#define USER_STACK_SZ   0x10000u       /* 64 KiB */
#define USER_ARENA_END  0x2400000u

/* ---- VBE linear framebuffer -------------------------------------------
 * Map a PCI MMIO framebuffer into the KERNEL page directory (identity,
 * supervisor-only, 4 KiB pages).  Returns the usable virtual address
 * (== phys) or 0 on failure.  Ring3 page directories copy only PDEs
 * 0..15, so a high LFB is never reachable from user mode -- this is what
 * keeps the "no user drawing API" guarantee in truecolour mode too. */
void *vbe_lfb_map(uint32_t phys, uint32_t size);

#endif
