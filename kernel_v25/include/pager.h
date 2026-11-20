#ifndef PAGER_H
#define PAGER_H

/*
 * pager.h -- v2.63: paging REMOVED.  This header is kept only as a
 * pass-through API so the ~40 shell.c call sites (help/cat/tree/...) do
 * not need rewriting.  Semantics now:
 *
 *   pager_begin()  -> 0 (never engages)
 *   pager_line()   -> prints line + "\n" immediately, returns 1
 *   pager_end()    -> clears any stale status bar
 *   pager_quit()   -> always 0
 *   pager_set_noninteractive() -> no-op
 *
 * Long listings are viewed with the normal scrollback (PgUp/PgDn/wheel),
 * not a "-- More --" bar.  Scrollback archiving is independent of pager.
 */

/* Begin a paged output block.  `active` is the caller's non-interactive
 * decision: pass 0 to force full-throughput mode (redirect / pipe / bg /
 * script), 1 to enable paging on an interactive console.  Returns 1 when
 * the pager is engaged, 0 when it fell back to plain streaming. */
int  pager_begin(int interactive);

/* Feed one already-formed output line (without trailing '\n').  The
 * pager prints it through vga_write() and appends its own newline.  When
 * the pager was disabled at pager_begin(), this is a plain vga_write().
 * Returns 1 when output should continue, 0 when the user quit ('q') and
 * the caller must drop the remaining output. */
int  pager_line(const char *line);

/* End the block: remove the transient status bar if it is still up and
 * reset per-block state.  Safe to call after a quit. */
void pager_end(void);

/* Mark the current command line as non-interactive (redirect / pipe /
 * background / script).  The shell sets this around the matching dispatch
 * path; the pager consults it when `interactive` is not explicitly passed. */
void pager_set_noninteractive(int on);

/* Current block was quit with 'q' (1) or not (0). */
int  pager_quit(void);

#endif
