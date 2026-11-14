/* atk_args.c -- attack vector: systemic argument fuzzing of all 65 ABI
 * slots with poison pointers and poison values.
 *
 * WHY IT MIGHT CRASH
 *   Every user pointer must be validated with validate_uaddr() and copied
 *   through the task's page table (the #78/#86 lesson).  One slot that
 *   forgot the check, or that computes addr+len with 32-bit wraparound,
 *   makes ring0 read or write an arbitrary physical page.  This program
 *   calls each slot with 5 poison tuples, so a missing check shows up as
 *   a ring0 page fault ("ring0 kernel fault - system halted").
 *
 * Deliberately SKIPPED slots (each is attacked by its own program, and
 * calling it here with a poison value would end the whole run early):
 *   2  cls           - wipes the evidence screen
 *   3  readline      - blocks on the keyboard when the buffer is valid
 *   13 exit          - would terminate this task mid-fuzz
 *   17 delay         - delay(0xFFFFFFFF) sleeps ~forever (task-level hang)
 *   22 getch         - blocks on the keyboard
 *   35 kill          - kill(0) is the shell -> atk_proc.c owns that
 *   47 wait          - wait(shell) deadlocks -> atk_proc.c owns that
 *   49 display_mode  - mode 0 asks the human for consent and BLOCKS
 *   64 pollkey       - harmless, but takes no arguments
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* Call a slot through its raw api pointer with four 32-bit arguments.
 * cdecl makes this safe for every arity: the thunk only reads the slots
 * it wants and ignores the rest. */
typedef int (*f4_t)(uint32_t, uint32_t, uint32_t, uint32_t);

static int skip_slot(int i) {
    switch (i) {
    case 2: case 3: case 13: case 17: case 22:
    case 35: case 47: case 49: case 64:
        return 1;
    default:
        return 0;
    }
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    f4_t *tab = (f4_t *)A();      /* catos_api_t is a table of pointers */
    int slot, p;
    (void)argc; (void)argv;

    w("ATK-ARGS: 65-slot argument poison fuzz\n");

    step(1); mark("tuple (0,0,0,0)\n");
    for (slot = 0; slot < 65; slot++) {
        if (skip_slot(slot)) continue;
        wd(tab[slot](0, 0, 0, 0)); w(" ");
    }
    nl();

    step(2); mark("tuple (0xFFFFFFFF x4)\n");
    for (slot = 0; slot < 65; slot++) {
        if (skip_slot(slot)) continue;
        wd(tab[slot](0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu));
        w(" ");
    }
    nl();

    step(3); mark("tuple (0x7FFFFFFF x4)\n");
    for (slot = 0; slot < 65; slot++) {
        if (skip_slot(slot)) continue;
        wd(tab[slot](0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu));
        w(" ");
    }
    nl();

    step(4); mark("poison-pointer sweeps, one value per line\n");
    for (p = 0; p < NPOISON; p++) {
        uint32_t v = POISON[p];
        mh("p", v);
        for (slot = 0; slot < 65; slot++) {
            if (skip_slot(slot)) continue;
            wd(tab[slot](v, v, v, v)); w(" ");
        }
        nl();
    }

    step(5); mark("mixed: valid small len + poison ptr, and vice versa\n");
    for (slot = 0; slot < 65; slot++) {
        if (skip_slot(slot)) continue;
        wd(tab[slot](0x02100000u, 0x400u, 0xFFFFFFFFu, 1));
        w("|");
        wd(tab[slot](0xFFFFFFFFu, 0x0u, 0x02100000u, 0));
        w(" ");
    }
    nl();

    /* Negative control: the slots we skipped must still answer sanely. */
    step(6); mark("skipped-slot negative control\n");
    m("exit? no", 0);
    m("kill(self)", A()->kill(A()->gettid()));   /* t == cur_task -> -1 */
    m("kill(-1)", A()->kill(-1));
    m("kill(999)", A()->kill(999));
    m("wait(999)", A()->wait(999));
    m("wait(-1)", A()->wait(-1));
    nl();

    w("\nATK-DONE\n");
    return 0;
}
