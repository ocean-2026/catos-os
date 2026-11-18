#ifndef PAGER_H
#define PAGER_H

/*
 * pager.h -- v2.59 generic console pager (minimal deliverable).
 *
 * A line-oriented pager layered on top of the EXISTING console output
 * path (vga_write / con_write).  It does NOT own a second screen driver
 * and does not repaint the whole framebuffer: callers feed it whole output
 * lines one at a time; the pager prints each line through the normal
 * vga_write() path and, once a screenful has been shown, parks a transient
 * status bar on the bottom row and waits for a key:
 *
 *   Enter  : advance one line
 *   Space  : advance one screenful
 *   q / Q  : quit the pager; the rest of the output is discarded and the
 *            caller returns straight to the prompt.
 *
 * Non-interactive contexts (redirected to a file, background job, piped
 * command, script / non-tty execution) disable the pager: every line is
 * printed straight through without ever waiting for a key.
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
