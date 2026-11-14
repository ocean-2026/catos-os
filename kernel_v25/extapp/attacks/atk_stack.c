/* atk_stack.c -- attack vector: ring3 stack exhaustion, no-NUL string
 * scans and self-modification of the vsyscall page.
 *
 * WHY IT MIGHT CRASH
 *   1) The user stack is 128 KiB (0x21E0000..0x2200000), fully mapped at
 *      spawn, and it is NOT a VMA with guard pages -- it simply ends.  A
 *      deep recursion or a large alloca walks below the VMA, faults, and
 *      must be killed as a ring3 fault.  If the kernel instead trusted the
 *      frame while handling the fault, or resumed at a bogus esp, the
 *      failure would escalate to ring0.
 *   2) The vsyscall page is mapped USER-WRITABLE ("RW: ls thunk scratch
 *      lives here", task.c:2153) and it holds the int 0x80 thunks AND the
 *      65-entry api pointer table.  A task can therefore rewrite the code
 *      it is about to execute; that must stay a ring3-only capability.
 *   3) An unterminated string at the last byte of a mapped page forces the
 *      kernel's u2k_str()/copy_from_user() to walk into the next page.
 *
 * Order: non-fatal probes first, the stack smash LAST (it kills this task).
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;

#define VS ((volatile uint8_t *)0x02080000u)

static char noptr[8192];

/* Frame of 512 bytes forces ~4 KB of stack per 8 levels. */
static int rec(int d) {
    volatile char pad[512];
    pad[0] = (char)d;
    pad[511] = (char)(d ^ 0x5A);
    if (d <= 0) return pad[0];
    return rec(d - 1) + pad[511];
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    int i;
    (void)argc; (void)argv;

    w("ATK-STACK: stack / string-scan / vsyscall abuse\n");

    step(1); mark("moderate recursion (depth 40, ~20 KiB) -- control\n");
    m("rec40", rec(40));
    nl();

    step(2); mark("unterminated string at the end of a mapped page\n");
    for (i = 0; i < 8192; i++) noptr[i] = 'A';        /* no NUL at all */
    m("strlen(8K no NUL)", api->strlen(noptr));
    m("strlen(last byte)", api->strlen(noptr + 8191));
    api->strcpy(noptr, noptr + 8000);   /* void in the ABI */
    w("strcpy done\n");
    nl();

    step(3); mark("vsyscall page is user-writable: peek at the thunks\n");
    {
        uint32_t api_tbl = *(volatile uint32_t *)(VS + 0x800);   /* slot 0 */
        mh("thunk[0]", api_tbl);
        mh("api ptr", (uint32_t)api);
        nl();
    }

    step(4); mark("self-modify a vsyscall thunk (ring3-only capability?)\n");
    /* Patch the exit thunk at +0xF00 with an infinite loop of our own, but
     * do not jump to it: if the write itself faults, the page is protected
     * (good); if it succeeds we restore it immediately. */
    {
        uint8_t save = VS[0xF00];
        VS[0xF00] = 0xEB;                                /* jmp $ (short) */
        VS[0xF01] = 0xFE;
        m("patched", VS[0xF00]);
        VS[0xF00] = save;
        w("restored\n");
    }

    step(5); mark("deep recursion: depth 600 (~300 KiB > 128 KiB stack)\n");
    if (argc >= 2) w("(skipped: vla pass)\n");
    else { m("rec600", rec(600)); nl(); }

    step(6); mark("huge VLA on the stack (0x7FFFFF bytes)\n");
    if (argc >= 2) {
        w("about to alloca\n");
        {
            volatile int n = 0x7FFFFF;
            volatile char vla[n];                 /* no room: must fault */
            vla[0] = 1;
            vla[n - 1] = 2;
            m("vla ok", vla[0] + vla[n - 1]);
        }
        nl();
    } else {
        w("(skipped: recursion pass)\n");
    }

    step(7); mark("if we are still alive the stack smash failed\n");
    w("\nATK-DONE\n");
    return 0;
}
