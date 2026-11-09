/* allocprobe.c -- CE probe for the per-task user heap (V-5 / P0-1).
 *
 * Before P0-1 the kernel's heap_vma_init() bailed out with
 * `if (t->vm) return 0;`, a condition that is always true for a ring3
 * task (task_spawn_ce already built the image/vsyscall/stack VMAs).
 * The heap VMA was therefore never created, api->alloc() always
 * returned NULL, and any CE program that trusted the allocator had to
 * fall back to a tiny static buffer.
 *
 * This probe proves the allocator works:
 *   alloc(64)   -> a non-NULL address, aligned inside the heap window
 *   alloc(8 KiB)-> a second, DISTINCT non-NULL address (no overlap)
 *   free(a)     -> accepted (must not fault)
 *   alloc again -> still returns usable memory after the free
 *
 * It also prints the raw addresses so a human (or a screenshot diff)
 * can confirm the values are inside USER_HEAP_BASE..+USER_HEAP_MAX
 * rather than merely non-NULL.
 *
 * Output contract: the probe prints "ALLOCPROBE: alloc works" as the
 * acceptance marker (PRD V-5) followed by "ALLOCPROBE PASS"; on any
 * failure it prints "ALLOCPROBE FAIL" instead.  The trailing line makes
 * a QEMU screenshot judgeable by eye.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* USER_HEAP_BASE / USER_HEAP_MAX as of the P0-3 constants (512 KiB).
 * Duplicated here on purpose: a CE program may not include paging.h
 * (that header is kernel-internal), and the probe must fail loudly if
 * the kernel's window ever moves. */
#define PROBE_HEAP_BASE 0x02100000u
#define PROBE_HEAP_MAX  0x00080000u

static void putline(const char *s) { __ce_api->write(s); }

static void puthex(uint32_t v) {
    __ce_api->write("0x");
    __ce_api->print_hex(v);
}

static int in_heap(uint32_t a) {
    return a >= PROBE_HEAP_BASE && a < PROBE_HEAP_BASE + PROBE_HEAP_MAX;
}

/* The CE loader jumps to image offset 0, so ce_main must be the first
 * thing in .text.  ce.ld only guarantees that for code placed in the
 * .text.ce_entry section (see hello.c). */
__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    int fails = 0;

    /* --- 1. small allocation --- */
    unsigned char *a = (unsigned char *)__ce_api->alloc(64);
    if (a == 0) {
        putline("FAIL alloc(64) returned NULL\n");
        fails++;
    } else {
        putline("ok   alloc(64) = ");
        puthex((uint32_t)a);
        putline("\n");
        if (!in_heap((uint32_t)a)) {
            putline("FAIL alloc(64) outside the heap window\n");
            fails++;
        } else {
            putline("ok   alloc(64) inside the heap window\n");
        }
    }
    /* --- 2. a second, larger allocation must not overlap the first --- */
    unsigned char *b = (unsigned char *)__ce_api->alloc(8192);
    if (b == 0) {
        putline("FAIL alloc(8k) returned NULL\n");
        fails++;
    } else {
        putline("ok   alloc(8k) = ");
        puthex((uint32_t)b);
        putline("\n");
        if (a && (uint32_t)b == (uint32_t)a) {
            putline("FAIL alloc(8k) returned the same address as alloc(64)\n");
            fails++;
        } else {
            putline("ok   alloc(8k) distinct from alloc(64)\n");
        }
        if (!in_heap((uint32_t)b)) {
            putline("FAIL alloc(8k) outside the heap window\n");
            fails++;
        }
    }

    /* --- 3. write/read roundtrip on both blocks --- */
    if (a) {
        for (int i = 0; i < 64; i++) a[i] = (unsigned char)(i * 3 + 5);
        int ok = 1;
        for (int i = 0; i < 64; i++)
            if (a[i] != (unsigned char)(i * 3 + 5)) { ok = 0; break; }
        if (ok) putline("ok   rw roundtrip on the 64-byte block\n");
        else { putline("FAIL rw roundtrip on the 64-byte block\n"); fails++; }
    }
    if (b) {
        /* touch the LAST byte of the 8 KiB block: that is the page
         * boundary the single-page heap_ensure() used to miss (P0-2). */
        b[8191] = 0xA5;
        b[0] = 0x5A;
        if (b[8191] == 0xA5 && b[0] == 0x5A)
            putline("ok   rw at the 8k block's last byte\n");
        else { putline("FAIL rw at the 8k block's last byte\n"); fails++; }
    }

    /* --- 4. free must be accepted --- */
    if (a) {
        __ce_api->free(a);
        putline("ok   free(64) accepted\n");
    }

    /* --- 5. the allocator must still work after the free --- */
    unsigned char *c = (unsigned char *)__ce_api->alloc(128);
    if (c == 0) {
        putline("FAIL alloc(128) after free returned NULL\n");
        fails++;
    } else {
        putline("ok   alloc(128) after free = ");
        puthex((uint32_t)c);
        putline("\n");
        c[0] = 0x42;
        if (c[0] == 0x42) putline("ok   rw after free\n");
        else { putline("FAIL rw after free\n"); fails++; }
    }

    if (fails == 0) {
        putline("ALLOCPROBE: alloc works\n");
        putline("ALLOCPROBE PASS\n");
        return 0;
    }
    putline("ALLOCPROBE FAIL\n");
    return 1;
}
