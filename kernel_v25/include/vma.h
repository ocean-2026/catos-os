/* vma.h -- anonymous mmap entry points (declared out of paging.h because
 * they take a task_t, which cannot be forward-declared: task.h typedefs
 * an anonymous struct; paging.h must stay includable from task.h). */
#ifndef VMA_H
#define VMA_H

#include "task.h"

/* anonymous mmap in the extension arena (PDE 9, 36..40 MiB).
 * len is rounded up to whole pages; the region is backed lazily by the
 * page-fault path.  Returns the user address (8 KiB past the VMA start,
 * so an underrun hits a guard page) or 0 on failure. */
uint32_t vma_mmap_anon(task_t *t, uint32_t len, int writable);

/* undo an mmap: unmap + free the pages in [addr, addr+len).  The range
 * must sit inside one anonymous mapping in the extension arena.
 * Returns 0, or a negative errno (-22 EINVAL). */
int      vma_munmap(task_t *t, uint32_t addr, uint32_t len);

#endif /* VMA_H */
