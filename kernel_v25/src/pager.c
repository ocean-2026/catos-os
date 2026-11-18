/*
 * pager.c -- v2.59 generic console pager (minimal deliverable).
 *
 * Design notes:
 *   - Layered on the existing vga_write()/con_write() front-end.  There is
 *     no second screen driver and no full-screen repaint: each fed line is
 *     printed through the normal output path, and the "More" prompt is a
 *     transient status bar on the bottom row (same mechanism as the IME
 *     candidate bar), saved/restored verbatim by render.c.
 *   - Line counting tracks newlines produced by this block.  A screenful
 *     holds (con_rows() - 1) lines: one row is reserved for the prompt, so
 *     the More bar can live on the last row without pushing real content
 *     off the top of the viewport.
 *   - Non-interactive sessions never block: redirect to a file, a
 *     background job, a piped command, or script / non-tty execution all
 *     stream the full output through immediately.
 *
 * Boundary rules (defensive against a tiny / mode-switched console):
 *   - Every screenful is read fresh from con_rows() on each park, never
 *     cached across a mode switch (`size`/`text`/`gfx` can change rows at
 *     runtime).
 *   - If the console has fewer than 2 rows, or a computed screenful would
 *     be 0/negative, the block falls back to plain streaming: no counter,
 *     no divide, no park.
 *   - A block that produces 0 or 1 line never enters the wait loop.
 */
#include "pager.h"
#include "vga.h"
#include "render.h"
#include "keyboard.h"

#define PAGER_BAR_TEXT "-- More (Enter=1 line, Space=1 page, q=quit) --"

static int g_enabled;        /* block pager is engaged (1) vs plain stream */
static int g_quit;           /* user pressed q: drop the rest of output    */
static int g_rows_left;      /* lines still printable before the next park */
static int g_pending_park;   /* screenful is full; park BEFORE the next   */
                             /* line -- so "exactly one screen" never bars */
static int g_free_lines;     /* lines still printable after a Space page  */
static int g_noninter;       /* process-wide non-interactive latch         */

void pager_set_noninteractive(int on) {
    g_noninter = on ? 1 : 0;
}
int  pager_quit(void)                 { return g_quit; }

/* How many lines fit in one screenful of this block.  Con_rows() is read
 * per park so a runtime `size`/mode switch is honoured.  Returns 0 when the
 * console is too small to reserve a prompt row: callers must treat 0 as
 * "stream straight through, do not park". */
static int screenful(void) {
    int r = con_rows();
    if (r < 2) return 0;          /* no room for a prompt row: stream      */
    int sf = r - 1;
    if (sf < 1) sf = 1;
    return sf;
}

/* Park the More bar on the bottom row and wait for a pager key.
 * Returns 1 = keep paging (screen), 2 = one line, 0 = quit. */
static int pager_wait_key(void) {
    con_status_bar(PAGER_BAR_TEXT);
    for (;;) {
        int k = kb_getkey();
        if (k == ' ' || k == KB_PGDN) {          /* Space: one screen */
            con_status_bar_off();
            return 1;
        }
        if (k == '\n' || k == '\r' || k == KB_DOWN || k == KB_PGUP) {
            /* Enter: advance a single line.  Up / PgUp is also treated as
             * one line for convenience; the forward controls are the spec. */
            con_status_bar_off();
            return 2;                           /* 2 = one line */
        }
        if (k == 'q' || k == 'Q') {
            con_status_bar_off();
            return 0;
        }
        /* Any other key is ignored; the bar stays parked. */
    }
}

int pager_begin(int interactive) {
    g_quit = 0;
    g_pending_park = 0;
    g_enabled = interactive && !g_noninter;
    g_rows_left = 0;
    if (g_enabled) {
        int sf = screenful();
        if (sf <= 0) {
            /* Console too small to page: stream the whole block through. */
            g_enabled = 0;
        } else {
            g_rows_left = sf;
        }
    }
    return g_enabled;
}

int pager_line(const char *line) {
    if (!g_enabled) {
        vga_write(line);
        vga_write("\n");
        return 1;
    }
    if (g_quit) return 0;

    /* Park BEFORE printing, not after: a block that is exactly one screen
     * tall must finish silently, and a block with one extra line parks
     * only when that extra line actually arrives. */
    if (g_pending_park) {
        int act = pager_wait_key();
        if (act == 0) { g_quit = 1; return 0; }
        /* Space keeps a whole page flowing (rows-1 lines) before the next
         * park; Enter leaves g_free_lines at 0 so the very next line parks
         * again (one line at a time). */
        g_free_lines = (act == 1) ? (screenful() - 1) : 0;
        if (g_free_lines < 0) g_free_lines = 0;
        g_pending_park = 0;
    }

    vga_write(line);
    vga_write("\n");

    /* Geometry, not a cached count: the console can be resized / switched
     * between text and gfx24 at runtime, so ask the front-end where the
     * cursor actually landed.  Sitting on the last row means the bar row
     * is needed next: arm the park so the NEXT line parks first. */
    if (g_free_lines > 0) {
        g_free_lines--;                 /* still inside the granted page */
        if (g_free_lines > 0) return 1;
    }
    int cx, cy, rows = con_rows();
    con_get_cursor(&cx, &cy);
    if (rows >= 2 && cy >= rows - 1) g_pending_park = 1;
    return 1;
}

void pager_end(void) {
    /* Always drop any parked bar, even when the block fell back to plain
     * streaming: a prior block's bar must not survive onto the next prompt. */
    con_status_bar_off();
    g_enabled = 0;
    g_quit = 0;
    g_rows_left = 0;
    g_pending_park = 0;
}
