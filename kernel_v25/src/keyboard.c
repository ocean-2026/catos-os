#include "keyboard.h"
#include "io.h"
#include "vga.h"
#include "ime.h"
#include "task.h"

/* US QWERTY scancode set 1 (make codes, unshifted / shifted) */
static const char sc_unshift[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};
static const char sc_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static char line[KB_BUF];
static int  line_pos = 0;
static int  shift = 0, caps = 0, ctrl = 0;
static int  got_e0 = 0;          /* an 0xE0 prefix was just seen */

/* ---- interrupt-driven scancode queue ---------------------------------
 * kb_irq() runs at IRQ1 and only enqueues bytes + wakes waiters; all
 * decoding and echo happens in kb_getkey() at normal kernel context. */

#define SCQ_SZ 256
static volatile uint8_t scq[SCQ_SZ];
static volatile uint32_t scq_head, scq_tail;

void kb_irq(void) {
    while (inb(0x64) & 1) {
        uint8_t sc = inb(0x60);
        uint32_t n = (scq_head + 1) % SCQ_SZ;
        if (n != scq_tail) { scq[scq_head] = sc; scq_head = n; }
    }
    console_wake_all();                 /* wake tasks blocked for a key */
}

static int scq_pop(void) {
    if (scq_tail == scq_head) return -1;
    uint8_t sc = scq[scq_tail];
    scq_tail = (scq_tail + 1) % SCQ_SZ;
    return sc;
}

/* non-blocking: 1 when at least one scancode is pending (for `top`) */
int kb_peek(void) { return scq_tail != scq_head; }

void kb_init(void) {
    line_pos = 0;
    shift = 0; caps = 0; ctrl = 0; got_e0 = 0;
    scq_head = scq_tail = 0;
    /* Flush any pending data */
    while (inb(0x64) & 1) inb(0x60);
}

/* Map scancode to a (possibly special) key value; called with modifiers
 * already handled. Returns 0 for keys to ignore. */
static int sc_to_key(uint8_t sc) {
    switch (sc) {
    case 0x48: return KB_UP;
    case 0x50: return KB_DOWN;
    case 0x4B: return KB_LEFT;
    case 0x4D: return KB_RIGHT;
    case 0x47: return KB_HOME;
    case 0x4F: return KB_END;
    case 0x53: return KB_DEL;
    case 0x52: return KB_INS;
    case 0x49: return KB_PGUP;
    case 0x51: return KB_PGDN;
    case 0x3B: return KB_F1;
    case 0x3C: return KB_F2;
    case 0x3D: return KB_F3;
    case 0x3E: return KB_F4;
    case 0x3F: return KB_F5;
    case 0x40: return KB_F6;
    case 0x41: return KB_F7;
    case 0x42: return KB_F8;
    case 0x43: return KB_F9;
    case 0x44: return KB_F10;
    case 0x57: return KB_F11;
    case 0x58: return KB_F12;
    default:   return 0;
    }
}

/* blocking: waits for the next scancode (yields the CPU while waiting) */
static int kb_next_scancode(void) {
    for (;;) {
        int sc = scq_pop();
        if (sc >= 0) return sc;
        task_block(T_WAIT_KB);
    }
}

/* timed scancode source: -1 when the deadline (timer ticks) passes */
extern volatile uint32_t timer_ticks;
static int kb_next_scancode_timed(uint32_t deadline) {
    for (;;) {
        int sc = scq_pop();
        if (sc >= 0) return sc;
        if ((int32_t)(timer_ticks - deadline) >= 0) return -1;
        task_sleep_ticks(1);
    }
}

/* Wait up to `ticks` timer ticks for a REAL key (modifier press/release
 * and break codes never count).  Returns the key (ASCII or KB_*) or -1
 * on timeout.  Used by `top` so a stale Enter-break never reads as a
 * quit key. */
int kb_getkey_timed(uint32_t ticks) {
    uint32_t deadline = timer_ticks + ticks;
    for (;;) {
        int s = kb_next_scancode_timed(deadline);
        if (s < 0) return -1;
        uint8_t sc = (uint8_t)s;

        if (got_e0) {
            got_e0 = 0;
            if (sc & 0x80) continue;
            if (sc == 0x1D) continue;
            int k = sc_to_key(sc);
            if (k) return k;
            continue;
        }
        if (sc == 0xE0) { got_e0 = 1; continue; }

        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc == 0x1D) { ctrl = 1; continue; }
        if (sc == 0x9D) { ctrl = 0; continue; }
        if (sc == 0x3A) { caps = !caps; continue; }
        if (sc & 0x80) continue;

        int k = sc_to_key(sc);
        if (k) return k;
        if (sc == 0x0E) return 8;           /* backspace */
        if (sc == 0x1C) return 10;          /* newline   */

        char c = (sc < 128) ? (shift ? sc_shift[sc] : sc_unshift[sc]) : 0;
        if (c == 0) continue;

        if (ctrl) {
            if (c >= 'a' && c <= 'z') return c - 'a' + 1;
            if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            int want_upper = shift ^ caps;
            if (want_upper) c -= 32;
        }
        return c;
    }
}

int kb_getkey(void) {
    for (;;) {
        int s = kb_next_scancode();
        uint8_t sc = (uint8_t)s;

        if (got_e0) {
            got_e0 = 0;
            if (sc & 0x80) continue;           /* E0 break codes: ignore */
            if (sc == 0x1D) continue;          /* right ctrl: ignore */
            int k = sc_to_key(sc);
            if (k) return k;
            continue;
        }
        if (sc == 0xE0) { got_e0 = 1; continue; }

        /* modifier keys */
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc == 0x1D) { ctrl = 1; continue; }
        if (sc == 0x9D) { ctrl = 0; continue; }
        if (sc == 0x3A) { caps = !caps; continue; }
        if (sc & 0x80) continue;               /* plain break codes: ignore */

        /* cursor / function keys (rare on PS/2 without E0, keep both) */
        int k = sc_to_key(sc);
        if (k) return k;
        if (sc == 0x0E) return '\b';
        if (sc == 0x1C) return '\n';

        char c = (sc < 128) ? (shift ? sc_shift[sc] : sc_unshift[sc]) : 0;
        if (c == 0) continue;

        if (ctrl) {
            if (c >= 'a' && c <= 'z') return c - 'a' + 1;  /* ^A..^Z */
            if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
            continue;                                      /* other ctrl: ignore */
        }
        if (c >= 'a' && c <= 'z') {
            int want_upper = shift ^ caps;
            if (want_upper) c -= 32;
        }
        return c;
    }
}

int kb_readline(char *buf, int len) {
    while (1) {
        int k = kb_getkey();

        /* v2.9 pinyin IME: F2 toggles; letters/digits may be intercepted.
         * A committed candidate is a 3-byte UTF-8 hanzi. */
        {
            char han[5];
            int r = ime_feed(k, han);
            if (r == IME_CONSUME) continue;
            if (r == IME_COMMIT) {
                for (int i = 0; han[i]; i++) {
                    if (line_pos >= KB_BUF - 1 || line_pos >= len - 1) break;
                    line[line_pos++] = han[i];
                }
                vga_write(han);
                continue;
            }
        }

        if (k == '\n') {                        /* Enter */
            int n = line_pos;
            if (n >= len) n = len - 1;
            for (int i = 0; i < n; i++) buf[i] = line[i];
            buf[n] = 0;
            vga_putc('\n');
            line_pos = 0;
            line[0] = 0;
            return 1;
        }
        if (k == '\b' || k == KB_DEL) {         /* erase one character */
            if (line_pos > 0) {
                /* UTF-8 aware: drop the whole sequence, erase once --
                 * a hanzi occupies two cells and one '\b' clears both. */
                int nb = 1;
                while (nb < 4 && line_pos - nb > 0 &&
                       ((unsigned char)line[line_pos - nb] & 0xC0) == 0x80)
                    nb++;
                line_pos -= nb;
                vga_putc('\b');
            }
            continue;
        }
        if (k >= 32 && k < 256) {               /* printable char */
            if (line_pos < KB_BUF - 1 && line_pos < len - 1) {
                line[line_pos++] = (char)k;
                vga_putc((char)k);
            }
        }
        /* arrow / function keys: ignored in line mode */
    }
}

/* T52: password entry.  Identical key handling to kb_readline, except that
 * a printable key appends to buf without echoing it -- a '*' is printed
 * instead, and backspace erases one star.  IME / UTF-8 input is deliberately
 * not routed here: passwords are ASCII. */
int kb_readline_masked(char *buf, int len) {
    int n = 0;
    if (len <= 0) return 0;
    while (1) {
        int k = kb_getkey();
        if (k == '\n') {
            buf[n] = 0;
            vga_putc('\n');
            return 1;
        }
        if (k == '\b' || k == KB_DEL) {
            if (n > 0) {
                n--;
                vga_putc('\b'); vga_putc(' '); vga_putc('\b');
            }
            continue;
        }
        if (k >= 32 && k < 127) {
            if (n < len - 1) {
                buf[n++] = (char)k;
                vga_putc('*');
            }
        }
        /* arrows / function keys: ignored */
    }
}
