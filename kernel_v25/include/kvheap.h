#ifndef KVHEAP_H
#define KVHEAP_H

#include <stdint.h>

/* kvheap.c - Stage 3A-1: page-backed virtually-contiguous kernel heap.
 *
 * Physical frames are pulled from the 4 KiB frame allocator and mapped into
 * a reserved high virtual window (see KVHEAP_BASE/SIZE below).  The allocator
 * hands out VIRTUAL addresses only; backing physical frames may be scattered.
 * Metadata (one free-list head per size class) is a small static table; no
 * large .bss array is added.
 *
 * Window layout (documented in docs/threading.md):
 *   KVHEAP_BASE .. KVHEAP_BASE+SIZE  supervisor-only, write-back
 *   above the 256 MiB identity RAM map, clear of the CE image
 *   (0x02000000) and the user arena (0x2000000..0x2800000).
 */

#define KVHEAP_BASE   0x18000000u
#define KVHEAP_SIZE   0x02000000u      /* 32 MiB window, 8192 frames */

void     kvheap_init(void);
void    *kvmalloc(uint32_t size, uint32_t align);   /* 16-aligned payload */
void     kvmfree(void *va);
uint32_t kvheap_inuse_bytes(void);
uint32_t kvheap_inuse_objs(void);
/* boot-time self test: 8 KiB + 32 KiB fill/read-back, free, realloc same
 * address.  Prints `kvheap selftest: PASS ...` to COM1. */
void     kvheap_selftest(void);

#endif
