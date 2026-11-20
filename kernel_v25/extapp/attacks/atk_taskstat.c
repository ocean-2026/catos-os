/* atk_taskstat.c -- attack vector: the v2.48 task_get() snapshot slot.
 *
 * WHY IT MIGHT CRASH
 *   task_get(idx, out) is a NEW kernel code path that copies a kernel
 *   struct INTO user memory.  The two failure classes that matter:
 *     1. out pointing at a poison address (NULL, low page, VRAM,
 *        kernel image, the task's own image edges, unaligned, top-of-
 *        space) -- the kernel must validate_uaddr() + k2u_copy() through
 *        the task's own page tables, never dereference `out` from ring0
 *        directly.  One missing check = ring0 #PF -> "ring0 kernel
 *        fault - system halted" (C1).
 *     2. idx out of range (negative / >= MAX_TASKS / 0xFFFFFFFF) -- must
 *        be rejected with -22 before it indexes the tasks[] array.
 *   This program hammers both classes, then proves the slot still works
 *   sanely afterwards.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;

typedef int (*tg_t)(int, void *);

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    tg_t f = (tg_t)A()->task_get;   /* slot 69: may be NULL on old kernels */
    int i;

    (void)argc; (void)argv;

    w("ATK-TASKSTAT: v2.48 task_get snapshot surface\n");
    if (f == 0) {
        /* Pre-v2.48 kernel: the slot was appended; the table entry is
         * zero, so the attack has no target.  That is still a PASS
         * (nothing to hit) but we say so explicitly in the log. */
        mark("kernel predates task_get (slot 69 NULL); nothing to hit");
        w("\nATK-DONE\n");
        return 0;
    }

    step(1); mark("idx sweep: -1 .. 0x7FFFFFFF, 0xFFFFFFFF\n");
    {
        int idxvals[6] = { -1, 0, 7, 15, 16, 0x7FFFFFFF };
        for (i = 0; i < 6; i++) {
            ce_task_info_t info;
            m("idx", idxvals[i]);
            wd(f(idxvals[i], &info));
            w(" ");
            if (i == 5) {
                /* 0xFFFFFFFF as int == -1: same class as -1 */
            }
        }
        /* the unsigned edge: pass a value that is negative-as-int but
         * the caller API takes int, so -1 covers it */
        nl();
    }

    step(2); mark("poison out pointers (valid idx 0)\n");
    for (i = 0; i < NPOISON; i++) {
        ce_task_info_t info;
        uint32_t v = POISON[i];
        mh("out", v);
        wd(f(0, (void *)v));
        w(" ");
    }
    nl();

    step(3); mark("out at the task's own image/bss/vsyscall edges\n");
    {
        /* the caller's image is 0x2000000+ (CE_LOAD_BASE); bss follows
         * the image; the vsyscall page at 0x2080000 is mapped but NOT
         * writable by the CE task, so an `out` there must fail the VMA
         * check, not write.  Stack is 0x22xxxxx. */
        uint32_t edges[] = {
            0x2000000u,            /* image start (read-only VMA)       */
            0x207FFFFFu,           /* just below the vsyscall page      */
            0x2080000u,           /* the vsyscall page itself          */
            0x2080800u,           /* inside the vsyscall page          */
            0x2080FFFu,           /* vsyscall page end                 */
        };
        for (i = 0; i < 5; i++) {
            ce_task_info_t info;
            mh("edge", edges[i]);
            wd(f(0, (void *)edges[i]));
            w(" ");
        }
        nl();
    }

    step(4); mark("cross-page poison: out at page edge +/- 1\n");
    {
        /* alloc a live page, aim `out` at its very last byte: the
         * 32-byte struct spans two pages and the second page is not
         * the task's.  The kernel must refuse (or handle the split),
         * never fault. */
        void *p = A()->alloc(4096);
        uint32_t base = (uint32_t)p & ~0xFFFu;
        ce_task_info_t info;
        int rc[4];
        rc[0] = f(0, (void *)(base + 4096 - 32)); /* exact fit        */
        rc[1] = f(0, (void *)(base + 4096 - 31));  /* +1 over the edge */
        rc[2] = f(0, (void *)(base + 4096 - 64));  /* 2 pages, both in */
        rc[3] = f(0, (void *)(base + 4095));       /* last byte        */
        m("exact", rc[0]); m("over", rc[1]);
        m("two-pg", rc[2]); m("last-b", rc[3]);
        nl();
    }

    step(5); mark("negative control: the slot still works\n");
    {
        ce_task_info_t info;
        int rc = f(0, &info);
        m("rc", rc);
        m("tid", info.tid);
        m("st", info.state);
        nl();
    }

    w("\nATK-DONE\n");
    return 0;
}
