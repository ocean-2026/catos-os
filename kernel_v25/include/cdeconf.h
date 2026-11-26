#ifndef CDECONF_H
#define CDECONF_H

/*
 * cdeconf.h -- v2.82 A1.4: /etc/cde.conf appearance configuration mirror.
 *
 * Persistence paradigm copied from /etc/style (gui/settings.c) and /etc/lang
 * (lang.c): a one-key-per-line text file under /etc, read through
 * fs_read_text() on boot / desktop entry, written back on a user-facing
 * switch via fs_write_text().  No new CE ABI, no new syscall.
 *
 * File format (INI, one KEY=VALUE per line; '#' starts a comment):
 *
 *   theme    = dark|light|solarized|mono   shell palette scheme
 *   accent   = #RRGGBB                     PAL_ACCENT role mirror
 *   fontsize = <n>                         RESERVED -- see notes: the block
 *                                          desktop has no runtime font-size
 *                                          state yet; parsed but not applied.
 *   taskbar  = bottom|...                  RESERVED -- taskbar position is
 *                                          fixed to the bottom edge in the
 *                                          block desktop; parsed but not applied.
 *
 * Missing file, absent volume, unknown or malformed keys fall back to the
 * built-in defaults already in memory; boot never writes the file on its own.
 * The hive (HKLM\Software\CatOS\Terminal\Theme) stays the primary source;
 * this file is the legacy mirror, adopted only when the hive has no label.
 */

/* Read /etc/cde.conf and adopt any valid fallback values. */
void cdeconf_load(void);

/* Persist the active scheme name (and the current accent mirror) to
 * /etc/cde.conf.  Called by the shell `theme` command after a switch. */
void cdeconf_save_theme(const char *scheme);

/* Last scheme label read from /etc/cde.conf ("" when the file had none).
 * Used by the desktop entry to pick the matching gui token. */
const char *cdeconf_scheme(void);

#endif /* CDECONF_H */
