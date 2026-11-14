/* atk_sysnr.c -- attack vector: illegal / out-of-range syscall numbers and
 * illegal interrupt vectors issued straight from ring3.
 *
 * WHY IT MIGHT CRASH
 *   The dispatcher is a `switch (nr)` over an enum of 66 slots, so the
 *   classic "table[NR] -> wild jump" bug should be impossible.  But
 *   SYS_LS_NEXT is a magic 1000, and build_vsyscall_page() builds a thunk
 *   table of exactly SYS_COUNT entries; anything that indexes that table
 *   with a user value would walk off it.  This program proves either way:
 *     - eax = SYS_COUNT.. , 1000/1001, 0xFFFF, 0x7FFFFFFF, 0x80000000,
 *       0xFFFFFFFF with all argument registers poisoned;
 *     - a direct `int $N` for vectors a ring3 task must never raise
 *       (3/4/13/14/32/33/48/0x41) -- the IDT gates for 0..47 are DPL0,
 *       so the correct answer is a #GP that kills only this task.
 * Expected (per src/task.c:1798 `default: ret = -8`): every number is
 * rejected with -8/-1 and the kernel keeps running.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static const uint32_t NRS[] = {
    65u, 66u, 67u, 70u, 99u, 255u, 1000u, 1001u, 1024u,
    0xFFFFu, 0x10000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
};

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    int i, j;
    (void)argc; (void)argv;

    w("ATK-SYSNR: illegal syscall numbers + illegal int vectors\n");

    step(1); mark("eax=65..0xFFFFFFFF, ebx/ecx/edx/esi=poison\n");
    for (i = 0; i < (int)(sizeof(NRS) / sizeof(NRS[0])); i++) {
        mh("nr", NRS[i]); w("r=");
        wd(sc5(NRS[i], 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu));
        w(" / r0="); wd(sc0(NRS[i])); nl();
    }

    step(2); mark("same numbers, args = task-internal addresses\n");
    for (i = 0; i < (int)(sizeof(NRS) / sizeof(NRS[0])); i++) {
        for (j = 8; j < NPOISON; j += 4) {
            wd(sc5(NRS[i], POISON[j], POISON[j], POISON[j], POISON[j]));
            w(" ");
        }
    }
    nl();

    step(3); mark("SYS_LS_NEXT(1000) with poison handles\n");
    for (j = 0; j < NPOISON; j++) {
        wd(sc5(1000u, POISON[j], POISON[j], 0, 0)); w(" ");
    }
    nl();

    step(4); mark("illegal int vectors from ring3 (expect task kill)\n");
    /* Vectors 0..47 are DPL0 exceptions/IRQs, 0x80 is DPL3.  Raising one
     * from ring3 must give #GP -> ring3 fault -> task killed, NOT a
     * kernel fault.  The driver treats "task killed + prompt back" as OK. */
    {
        static const int V[] = { 3, 4, 13, 14, 32, 33, 48, 0x41 };
        for (i = 0; i < (int)(sizeof(V) / sizeof(V[0])); i++) {
            w("int 0x"); wh((uint32_t)V[i]); w(": ");
            rawint(V[i]);
            w("returned\n");     /* <- only for a vector that is DPL3 */
        }
    }

    w("\nATK-DONE\n");
    return 0;
}
