#ifndef CATOS_BSOD_H
#define CATOS_BSOD_H

/*
 * bsod.h -- v2.59 full-screen kernel crash report ("blue screen of death").
 *
 * The crash screen is deliberately SELF-CONTAINED: once bsod_enter() runs it
 * draws through the vga_* front-end facade (set_color/clear/write_at), polls
 * the 8042 keyboard directly and never touches the heap, the filesystem, the
 * shell, the IME or the font loader.  The only outside call it may make on
 * entry is con_force_text(): if the console happens to be in gfx24/VBE/
 * Mode13h, the kernel-only emergency switch back to 80x25 text mode is
 * requested first; if that fails the front-end writes still land in whatever
 * text surface is mapped.
 *
 * Panic behaviour (reboot vs halt) is read ONCE at boot from
 * /etc/panicmode and cached in a plain bss variable, so the crash path
 * itself never opens a file.
 */
#include <stdint.h>
#include "cpu.h"          /* int_frame_t */

#define BSOD_MODE_REBOOT  0
#define BSOD_MODE_HALT    1
#define BSOD_MODE_WAIT    2

/* P0-2 second-fault guard: 1 once the crash screen has been entered.
 * Exposed so the exception dispatcher can take the hard `cli; hlt`
 * shortcut instead of re-entering the BSOD. */
extern volatile int g_in_panic;

/* Read /etc/panicmode (reboot|halt|wait, optional `reboot:<seconds>`) into
 * the cached mode.  Safe to call before the FS is ready: stays on default. */
void bsod_init(void);

int  bsod_mode(void);
void bsod_set_mode(int mode);
/* P2-9: cache a `wait` mode from the shell builtin. */
void bsod_set_wait(void);

/*
 * Render the crash report.
 *   msg   : short panic line (may be NULL -> "(no message)").
 *   frame : exception frame, or NULL for a plain manual panic.
 *   soft_return: 1 -> after a keypress return to the caller (used by the
 *               `bsod --no-reboot` test hook); 0 -> act on panicmode
 *               (reboot / halt) after a keypress or the idle timeout.
 */
void bsod_enter(const char *msg, const int_frame_t *frame, int soft_return);

/* Idle timeout before the default action fires, in seconds. */
#define BSOD_TIMEOUT_SEC  30

/* ---- panic record (fixed bss area, no FS write on the crash path) ------ */
#define PANIC_RECORD_MAGIC 0x43504E31u   /* "CPN1" */
#define PANIC_FRAMES_MAX   5

typedef struct {
    uint32_t magic;
    uint32_t ticks;
    int      intno;
    uint32_t eip, cr2, err, eflags, esp_k;
    uint32_t frames[PANIC_FRAMES_MAX];
    int      nframes;
    char     version[24];
    char     msg[64];
} panic_record_t;

/* Read-back accessors for the shell `crashlog` builtin.  Returns NULL /
 * magic 0 when no panic record has been written this boot. */
const panic_record_t *panic_record_get(void);
uint32_t              panic_record_magic(void);

/* Resolve an address to nearest built-in symbol + offset.  Writes a
 * NUL-terminated string into `out` (max `n` chars).  Shared by the BSOD
 * and the crashlog summary; no allocation. */
void bsod_sym_fmt(char *out, int n, uint32_t addr);

/* P0-2: nested-fault depth cap.  A second fatal fault while already in the
 * crash screen burns one level; past this count we stop painting and just
 * cli; hlt in a loop. */
#define BSOD_PANIC_DEPTH_MAX 2

#endif /* CATOS_BSOD_H */
