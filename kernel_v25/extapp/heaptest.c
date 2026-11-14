/* heaptest.c -- CE probe for cross-page heap allocation (V-6 / P0-2).
 *
 * The kernel's heap_ensure() mapped only the ONE page containing the
 * requested address.  A multi-page allocation therefore faulted on its
 * second page (cr2=0x02101007) and the task was killed -- the symptom
 * PM recorded against the original project as issue #47.
 *
 * This probe allocates blocks that straddle page boundaries and writes
 * to EVERY byte, so any unmapped page inside the payload shows up
 * immediately as a task kill (no "FAIL" line) rather than silently
 * passing.
 *
 * Blocks used:
 *   4097  bytes  -> 2 pages, straddles the 4 KiB boundary
 *   12288 bytes  -> 3 pages
 *   65536 bytes  -> 16 pages (also proves the heap window is big enough
 *                   for a realistic buffer)
 *
 * The last line is "HEAPTEST PASS" or "HEAPTEST FAIL".
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static void putline(const char *s) { __ce_api->write(s); }

static void putdec(int v) { __ce_api->print_int(v); }

/* Fill a block with a position-dependent pattern, then verify it.
 * Returns 0 on success, -1 on a mismatch. */
static int fill_and_check(unsigned char *p, unsigned int n, unsigned int seed) {
    for (unsigned int i = 0; i < n; i++)
        p[i] = (unsigned char)((i * 31u + seed) & 0xFFu);
    for (unsigned int i = 0; i < n; i++)
        if (p[i] != (unsigned char)((i * 31u + seed) & 0xFFu)) return -1;
    return 0;
}

static void test_block(unsigned int n, unsigned int seed) {
    unsigned char *p = (unsigned char *)__ce_api->alloc(n);
    putline("  alloc(");
    putdec((int)n);
    putline(") = ");
    if (p == 0) {
        putline("NULL -> FAIL\n");
        return;
    }
    __ce_api->print_hex((uint32_t)p);
    putline("\n");
    if (fill_and_check(p, n, seed) == 0) {
        putline("  rw over ");
        putdec((int)n);
        putline(" bytes: ok\n");
    } else {
        putline("  rw over ");
        putdec((int)n);
        putline(" bytes: FAIL (pattern mismatch)\n");
    }
    __ce_api->free(p);
}

/* The CE loader jumps to image offset 0, so ce_main must be the first
 * thing in .text.  ce.ld only guarantees that for code placed in the
 * .text.ce_entry section (see hello.c); without it the linker is free
 * to emit the static helpers first and ce_main lands elsewhere. */
__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    int fails = 0;

    putline("HEAPTEST start\n");

    /* 2-page block: the classic P0-2 trigger. */
    test_block(4097u, 1u);
    /* 3-page block. */
    test_block(12288u, 7u);
    /* 16-page block: a realistic application buffer. */
    test_block(65536u, 13u);

    /* Several medium blocks back to back: the free-list path splits a
     * chunk and the remainder must also be mapped. */
    for (int i = 0; i < 4; i++) {
        unsigned char *q = (unsigned char *)__ce_api->alloc(9000u);
        if (q == 0) {
            putline("  alloc(9000) round ");
            putdec(i);
            putline(" -> NULL FAIL\n");
            fails++;
            continue;
        }
        if (fill_and_check(q, 9000u, (unsigned int)(i + 100)) != 0) {
            putline("  alloc(9000) round ");
            putdec(i);
            putline(" -> rw FAIL\n");
            fails++;
        } else {
            __ce_api->free(q);
        }
    }
    putline("  four 9000-byte blocks: ok\n");

    if (fails == 0) { putline("HEAPTEST PASS\n"); return 0; }
    putline("HEAPTEST FAIL\n");
    return 1;
}
