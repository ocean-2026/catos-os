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

/* Read /etc/panicmode (reboot|halt, default reboot) into the cached mode.
 * Safe to call before the FS is ready: stays on the default. */
void bsod_init(void);

int  bsod_mode(void);
void bsod_set_mode(int mode);

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

#endif /* CATOS_BSOD_H */
