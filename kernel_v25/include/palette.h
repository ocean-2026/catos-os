#ifndef PALETTE_H
#define PALETTE_H

/*
 * palette.h -- semantic colour roles backed by the hive (v2.67).
 *
 * Before v2.67 every colour the shell used was a compile-time constant
 * (VGA_LIGHT_GREEN for the prompt, VGA_LIGHT_RED for errors, ...).  That
 * meant a user could re-colour the console only by rebuilding the kernel,
 * and the crash screen kept its own hard-coded palette.
 *
 * Now each *role* is a named hive value:
 *
 *   HKLM\Software\CatOS\Terminal\Colors\<Role>     console roles
 *   HKLM\System\Crash\Colors\<Role>                crash-screen roles
 *
 * pal_init() reads them once at shell boot and reserves CLUT slots
 * 32..32+PAL_COUNT so a custom RGB survives on every backend (text mode
 * quantises down to the nearest of the 16 hardware colours -- the stored
 * value is still the truth, and the next VBE session uses it exactly).
 *
 * A missing or malformed value falls back to the built-in default and is
 * reported, never silently accepted: "trust nothing" applies to our own
 * configuration file too.
 */

#include <stdint.h>

enum pal_role {
    PAL_BG = 0,
    PAL_FG,
    PAL_PROMPT_USER,
    PAL_PROMPT_PATH,
    PAL_PROMPT_SYMBOL,
    PAL_COMMAND,
    PAL_ARGUMENT,
    PAL_ERROR,
    PAL_WARNING,
    PAL_SUCCESS,
    PAL_INFO,
    PAL_DIM,
    PAL_ACCENT,
    PAL_SELECTION,
    PAL_STATUS_BG,
    PAL_STATUS_FG,
    PAL_BSOD_BG,
    PAL_BSOD_FG,
    PAL_BSOD_TITLE,
    PAL_BSOD_HINT,
    PAL_BSOD_CODE,
    PAL_COUNT
};

/* Load every role from the hive (defaults where absent/invalid).
 * Returns the number of roles that had to fall back. */
int  pal_init(void);

/* Re-read the hive after a registry edit; returns fallback count. */
int  pal_reload(void);
/* v2.72: re-seed every role with the built-in defaults and persist them.
 * Backs `colors reset`; the hive stays the single source of truth. */
int  pal_reset(void);

/* Resolved colour of a role: 0x00RRGGBB. */
uint32_t pal_rgb(int role);

/* CLUT index to hand to vga_set_fg()/con_set_fg().  The slot is already
 * programmed with the role's RGB by pal_init()/pal_reload(). */
uint8_t  pal_vga(int role);

/* Value text as stored ("#RRGGBB"), for `theme` / `hive ls`. */
const char *pal_hex(int role);

/* Hive path of a role (e.g. "...\\Colors\\PromptUser"). */
const char *pal_path(int role);
const char *pal_name(int role);

/* Persist a role.  Validates the input first; an invalid colour string
 * is REFUSED (returns -1) instead of being written and forgotten. */
int  pal_set(int role, const char *spec);
int  pal_set_rgb(int role, uint32_t rgb);

/* The shell's `theme` command: rewrite the console roles from a named
 * built-in scheme (dark / light / solarized / mono).  Crash roles are
 * left alone unless `with_crash` is set. */
int  pal_apply_scheme(const char *name, int with_crash);
const char *pal_scheme_name(void);

#endif /* PALETTE_H */
