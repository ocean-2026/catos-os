#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KB_BUF 256

/* Special keys returned by kb_getkey (all > 0xFF so they never collide
 * with ASCII).  Used by the nano-style editor. */
#define KB_UP      0x100
#define KB_DOWN    0x101
#define KB_LEFT    0x102
#define KB_RIGHT   0x103
#define KB_HOME    0x104
#define KB_END     0x105
#define KB_DEL     0x106
#define KB_INS     0x107
#define KB_PGUP    0x108
#define KB_PGDN    0x109
#define KB_F1      0x110
#define KB_F2      0x111
#define KB_F3      0x112
#define KB_F4      0x113
#define KB_F5      0x114
#define KB_F6      0x115
#define KB_F7      0x116
#define KB_F8      0x117
#define KB_F9      0x118
#define KB_F10     0x119
#define KB_F11     0x11A
#define KB_F12     0x11B
/* v2.21 T51: Ctrl+E0-arrow combinations.  Plain arrows drive history and
 * cursor movement in the line editor; Ctrl+Up/Down drive console
 * scrollback.  They are distinct codes so no modifier state leaks out of
 * kb_getkey() into callers. */
#define KB_CTRL_UP    0x120
#define KB_CTRL_DOWN  0x121
#define KB_CTRL_LEFT  0x122
#define KB_CTRL_RIGHT 0x123
#define KB_CTRL_PGUP  0x124
#define KB_CTRL_PGDN  0x125

void kb_init(void);
void kb_irq(void);            /* PS/2 IRQ1: enqueue scancodes, wake waiters */
void kb_usb_feed(uint8_t sc); /* USB HID: set-1 scancode from usb_uhci.c   */
int  kb_peek(void);           /* non-blocking: a scancode is pending */
int  kb_getkey_timed(uint32_t ticks);  /* real key within N ticks, else -1 */
/* Blocking: returns one key as either an ASCII code (0..255, Ctrl+letter
 * comes back as ^A..^Z i.e. 0x01..0x1A) or one of the KB_* special codes. */
int  kb_getkey(void);
/* Line mode used by the shell: echoes input, returns 1 on Enter with the
 * line copied into buf (NUL-terminated, max len-1 chars). */
int  kb_readline(char *buf, int len);
/* T52: same line reader but every printable key echoes '*' instead of the
 * character -- for password entry.  Backspace erases one star. */
int  kb_readline_masked(char *buf, int len);

#endif
