#include "ce_app.h"

/* gotoprobe.c -- v2.69 regression for CE ABI slot 65 (gotoxy).
 * Before the fix nargs[65] was implicitly 0, so the vsyscall thunk did
 * NOT forward ebx/ecx and gotoxy(x,y) saw stale arguments.  This program
 * clears the screen, moves to a known cell, and writes a unique marker.
 * The host probe reads VGA text (10,3) and asserts the marker landed
 * exactly there -- proving both arguments crossed the thunk intact.
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;

    api->cls();
    int rc = api->gotoxy(10, 3);
    api->write("[GOTO-WORD]");
    /* report the return code as a single hex digit at (0,0) */
    api->gotoxy(0, 0);
    char h = "0123456789abcdef"[((unsigned)rc) & 15];
    api->putc(h);
    return 0;
}
