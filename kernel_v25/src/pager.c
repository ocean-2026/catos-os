/*
 * pager.c -- v2.63: paging REMOVED, this is now a pass-through shim.
 *
 * The v2.59 pager parked a "-- More --" bar and blocked on kb_getkey()
 * every screenful.  Users asked for multi-page output to go away.  Rather
 * than rewrite the ~40 call sites in shell.c (help/cat/tree/...), the API
 * is kept intact and every entry point now streams straight through:
 *
 *   pager_begin()  -> returns 0 (never engaged)
 *   pager_line()   -> vga_write(line) + "\n", always returns 1
 *   pager_end()    -> clears any stale status bar, resets state
 *   pager_quit()   -> always 0 (no block can be aborted by 'q')
 *   pager_set_noninteractive() -> no-op (nothing is interactive anymore)
 *
 * Scrollback (PgUp/PgDn / wheel) still works: the console front-end
 * archives every emitted line regardless of pager state.  To see a long
 * `help`/`cat`/`tree` listing, use the normal scrollback keys instead of
 * the old More bar.
 */
#include "pager.h"
#include "vga.h"
#include "render.h"

void pager_set_noninteractive(int on) { (void)on; }
int  pager_quit(void)                 { return 0; }

int pager_begin(int interactive) {
    (void)interactive;
    return 0;                       /* never engaged: always plain stream */
}

int pager_line(const char *line) {
    if (line) vga_write(line);
    vga_write("\n");
    return 1;
}

void pager_end(void) {
    /* Defensive: a bar left over from a previously running v2.59/62 image
     * can never survive a reboot, but clearing is free and keeps callers
     * safe if pager_end() is used out of order. */
    con_status_bar_off();
}
