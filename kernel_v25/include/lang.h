#ifndef LANG_H
#define LANG_H

/* lang.h -- v2.21 user-visible language setting (T50).
 *
 * English is the DEFAULT.  `lang zh` switches the handful of shell
 * messages that are user-facing to Chinese and persists the choice in
 * /etc/lang; `lang en` switches back.  Command names, help text, kernel
 * diagnostics and CE applications stay in English on purpose -- they are
 * part of the scripting surface and must not change spelling.
 *
 * The module is tiny and has no libc dependency: tr() returns a static
 * string, so callers can pass it straight to vga_write(). */

#include <stdint.h>

#define LANG_EN 0
#define LANG_ZH 1

/* Message identifiers.  Append-only: never renumber, old ids are used by
 * translation tables in the shell. */
enum {
    L_NO_FAT32 = 0,
    L_FORMAT_CONFIRM,
    L_FORMAT_CANCEL,
    L_FORMAT_DONE,
    L_ERR_NOFILE,
    L_ERR_PERM,
    L_ERR_BADARG,
    L_NET_DOWN,
    L_SND_NONE,
    L_GFX_NONE,
    L_TEXT_CONFIRM,
    L_LANG_USAGE,
    L_LANG_SET,
    L_LANG_PERSIST_FAIL,
    L_PASSWD_PROMPT,
    L_PASSWD_MISMATCH,
    L_JOBS_NONE,
    L_LOG_NOJOB,
    L_COUNT
};

/* Load /etc/lang.  Safe to call before fs_init(): falls back to English.
 * Call once after fs_init() in shell_run(). */
void        lang_init(void);
int         lang_get(void);
/* Persist + switch.  0 = ok, -1 = bad id, -2 = /etc/lang write failed
 * (the in-memory language still changes so the session stays usable). */
int         lang_set(int l);
const char *lang_name(void);
/* Translated message, always NUL-terminated, never NULL. */
const char *tr(int id);

#endif /* LANG_H */
