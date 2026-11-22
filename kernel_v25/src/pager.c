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
#include "keyboard.h"

/* v2.76: "type anything to skip the flood".  v2.63 made every pager call a
 * pass-through, so a long listing (help/tree/cat) scrolled for as long as it
 * took to paint, and keystrokes sat in the scancode queue until the last line
 * was out.  Now pager_line() watches for a pending key ONCE the output has
 * passed a full screen, marks the stream aborted and returns; every caller
 * already bails out on pager_quit(), so the remaining lines are never printed
 * and the buffered keystroke reaches the line editor unchanged. */
static int p_lines, p_armed, p_aborted, p_noabort;

void pager_set_noninteractive(int on) { (void)on; }
void pager_reset(void) { p_lines = 1; p_armed = 0; p_aborted = 0; }
void pager_suppress_abort(int on) { p_noabort = on; if (on) p_aborted = 0; }
int  pager_quit(void)  { return p_aborted; }
int  pager_aborted(void) { return p_aborted; }

int pager_begin(int interactive) {
    (void)interactive;
    pager_reset();
    return 0;                       /* never engaged: always plain stream */
}

int pager_line(const char *line) {
    if (p_aborted) return 0;
    if (!p_armed) {
        int rows = con_rows();
        if (rows < 4) rows = 24;
        if (p_lines >= rows) p_armed = 1;    /* one full screen has gone by */
    }
    if (!p_noabort && p_armed && kb_key_pending()) {              /* any key: stop the flood */
        p_aborted = 1;
        return 0;
    }
    if (line) vga_write(line);
    vga_write("\n");
    p_lines++;
    return 1;
}

void pager_end(void) {
    /* Defensive: a bar left over from a previously running v2.59/62 image
     * can never survive a reboot, but clearing is free and keeps callers
     * safe if pager_end() is used out of order. */
    con_status_bar_off();
}
