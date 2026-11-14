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
static int  line_len = 0, line_cur = 0;   /* bytes: total length / cursor */
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

/* v2.26: USB keyboard path -- HID reports are translated to scancode
 * set 1 by usb_uhci.c and pushed through the SAME queue as PS/2, so the
 * whole decoder stack (shift/caps/ctrl, IME, scrollback) works unchanged. */
void kb_usb_feed(uint8_t sc) {
    uint32_t n = (scq_head + 1) % SCQ_SZ;
    if (n != scq_tail) { scq[scq_head] = sc; scq_head = n; }
    console_wake_all();
}

void kb_init(void) {
    line_len = line_cur = 0;
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
    case 0x0F: return KB_TAB;         /* v2.28: TAB for completion */
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

/* v2.21 T51: fold the Ctrl modifier into E0-prefixed special keys.
 * Without this, Ctrl+Up is indistinguishable from plain Up, so the shell
 * cannot offer both history and scrollback on the arrow cluster. */
static int e0_map(int k, int ctrl_held) {
    if (!ctrl_held) return k;
    switch (k) {
    case KB_UP:    return KB_CTRL_UP;
    case KB_DOWN:  return KB_CTRL_DOWN;
    case KB_LEFT:  return KB_CTRL_LEFT;
    case KB_RIGHT: return KB_CTRL_RIGHT;
    case KB_PGUP:  return KB_CTRL_PGUP;
    case KB_PGDN:  return KB_CTRL_PGDN;
    default:       return k;
    }
}

/* blocking: waits for the next scancode (yields the CPU while waiting) */
static int kb_next_scancode(void) {
    for (;;) {
        usb_service();                     /* v2.29: latch hot-plug attach */
        int sc = scq_pop();
        if (sc >= 0) return sc;
        /* poll, don't block: usb_service() must run even with no key
         * producer alive (hot-plugged USB keyboard before attach). */
        task_sleep_ticks(2);
    }
}

/* timed scancode source: -1 when the deadline (timer ticks) passes */
extern volatile uint32_t timer_ticks;
static int kb_next_scancode_timed(uint32_t deadline) {
    for (;;) {
        usb_service();                     /* v2.29 */
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
            if (sc == 0x1D) { ctrl = 1; continue; }   /* right ctrl down */
            if (sc == 0x9D) { ctrl = 0; continue; }   /* right ctrl up   */
            if (sc & 0x80) continue;                   /* other E0 breaks */
            int k = sc_to_key(sc);
            if (k) return e0_map(k, ctrl);
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
        if (k) return e0_map(k, ctrl);
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
            if (sc == 0x1D) { ctrl = 1; continue; }   /* right ctrl down */
            if (sc == 0x9D) { ctrl = 0; continue; }   /* right ctrl up   */
            if (sc & 0x80) continue;           /* E0 break codes: ignore */
            int k = sc_to_key(sc);
            if (k) return e0_map(k, ctrl);
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
        if (k) return e0_map(k, ctrl);
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

/* ---- v2.21 T51: interactive line editor --------------------------------
 * Up/Down      : command history (16 entries, consecutive duplicates folded)
 * Left/Right   : move the insertion point, UTF-8 aware
 * Home/End     : start / end of line
 * Backspace/Del: delete a whole UTF-8 sequence, not one byte
 * Ctrl+Up/Down : console scrollback (Ctrl+PgUp/PgDn = half a screen)
 * Typing       : syntax highlighting (command / option / number / string)
 *
 * All redraw is done with absolute cell writes from the anchor position
 * captured at entry, so the prompt is never re-printed and the cursor can
 * sit in the middle of the line.  con_write_at_utf8() keeps CJK at exactly
 * 2xASCII, which is the invariant the graphics back-ends rely on. */
#define RL_HIST 16
static char rl_hist[RL_HIST][KB_BUF];
static int  rl_hist_head, rl_hist_count;

static void rl_hist_push(const char *s) {
    if (!s || !*s) return;
    if (rl_hist_count) {
        int last = (rl_hist_head - 1 + RL_HIST) % RL_HIST;
        int same = 1, i = 0;
        for (; s[i] && i < KB_BUF - 1; i++)
            if (rl_hist[last][i] != s[i]) { same = 0; break; }
        if (same && !rl_hist[last][i]) return;
    }
    int j = 0;
    for (; s[j] && j < KB_BUF - 1; j++) rl_hist[rl_hist_head][j] = s[j];
    rl_hist[rl_hist_head][j] = 0;
    rl_hist_head = (rl_hist_head + 1) % RL_HIST;
    if (rl_hist_count < RL_HIST) rl_hist_count++;
}
/* pos 0 = newest, pos 1 = older ... */
static const char *rl_hist_get(int pos) {
    if (pos < 0 || pos >= rl_hist_count) return 0;
    int k = (rl_hist_head - 1 - pos + RL_HIST * 2) % RL_HIST;
    return rl_hist[k];
}

static int rl_is_cont(unsigned char c) { return (c & 0xC0u) == 0x80u; }

/* Display columns consumed by the first n bytes (CJK = 2 cells). */
static int rl_width(const char *s, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (rl_is_cont(c)) continue;
        w += (c >= 0x80u) ? 2 : 1;
    }
    return w;
}
static int rl_prev(int p) {
    if (p <= 0) return 0;
    int q = p - 1;
    while (q > 0 && rl_is_cont((unsigned char)line[q])) q--;
    return q;
}
static int rl_next(int p) {
    if (p >= line_len) return line_len;
    int q = p + 1;
    while (q < line_len && rl_is_cont((unsigned char)line[q])) q++;
    return q;
}
static void rl_insert(const char *s, int n) {
    if (n <= 0 || line_len + n >= KB_BUF - 1) return;
    for (int i = line_len; i > line_cur; i--) line[i + n - 1] = line[i - 1];
    for (int i = 0; i < n; i++) line[line_cur + i] = s[i];
    line_len += n; line_cur += n; line[line_len] = 0;
}
static void rl_erase(int from, int to) {
    if (from < 0 || to > line_len || from >= to) return;
    int n = to - from;
    for (int i = to; i < line_len; i++) line[i - n] = line[i];
    line_len -= n;
    if (line_cur > to) line_cur -= n;
    else if (line_cur > from) line_cur = from;
    line[line_len] = 0;
}

/* Colour for one whitespace-delimited token. */
static uint8_t rl_tok_color(const char *tok, int n, int index, uint8_t base) {
    if (n <= 0) return base;
    if (tok[0] == '"' || tok[0] == '\'') return CON_YELLOW;
    if (tok[0] == '-')                   return CON_LIGHT_MAGENTA;
    if (index == 0)                      return CON_LIGHT_CYAN;
    int digits = 1;
    for (int i = 0; i < n; i++)
        if (tok[i] < '0' || tok[i] > '9') { digits = 0; break; }
    if (digits) return CON_LIGHT_GREEN;
    return base;
}

/* v2.28: completer hook registered by the shell. */
static rl_complete_fn rl_completer;
void kb_set_completer(rl_complete_fn fn) { rl_completer = fn; }

static void rl_redraw(int x0, int y0, uint8_t base) {
    int cols = con_cols();
    if (x0 >= cols) x0 = cols - 1;
    /* wipe the rest of the input row with the original foreground */
    con_set_fg(base);
    char pad[128];
    int room = cols - x0;
    if (room > 120) room = 120;
    for (int i = 0; i < room; i++) pad[i] = ' ';
    pad[room] = 0;
    con_write_at((uint8_t)x0, (uint8_t)y0, pad);

    char seg[KB_BUF];
    int i = 0, cx = x0, tok = 0;
    while (i < line_len && cx < cols) {
        if (line[i] == ' ') {                 /* plain separator */
            seg[0] = ' '; seg[1] = 0;
            con_set_fg(base);
            con_write_at_utf8((uint8_t)cx, (uint8_t)y0, seg);
            cx++; i++;
            continue;
        }
        int e = i;
        if (line[i] == '"' || line[i] == '\'') {
            char q = line[i]; e++;
            while (e < line_len && line[e] != q) e++;
            if (e < line_len) e++;
        } else {
            while (e < line_len && line[e] != ' ') e++;
        }
        int n = e - i;
        if (n > KB_BUF - 1) n = KB_BUF - 1;
        for (int j = 0; j < n; j++) seg[j] = line[i + j];
        seg[n] = 0;
        con_set_fg(rl_tok_color(seg, n, tok, base));
        con_write_at_utf8((uint8_t)cx, (uint8_t)y0, seg);
        cx += rl_width(seg, n);
        tok++;
        i = e;
    }
    con_set_fg(base);
    int cur_col = x0 + rl_width(line, line_cur);
    if (cur_col >= cols) cur_col = cols - 1;
    con_set_cursor((uint8_t)cur_col, (uint8_t)y0);
}

int kb_readline(char *buf, int len) {
    if (len <= 0) return 0;
    int x0 = 0, y0 = 0;
    con_get_cursor(&x0, &y0);
    ime_set_anchor(y0);             /* v2.25: candidate bar under the line */
    uint8_t base = con_get_fg();
    int cols = con_cols();
    int hist_pos = -1;                  /* -1 = the draft line being typed */
    char draft[KB_BUF];
    line_len = line_cur = 0; line[0] = 0;

    while (1) {
        int k = kb_getkey();

        /* Leaving scrollback: any non-scroll key restores the live grid
         * (including the partly typed line) and is then handled normally. */
        if (con_scrollback_view() &&
            k != KB_CTRL_UP && k != KB_CTRL_DOWN &&
            k != KB_CTRL_PGUP && k != KB_CTRL_PGDN)
            con_scrollback_reset();

        if (k == KB_CTRL_UP   || k == KB_CTRL_DOWN ||
            k == KB_CTRL_PGUP || k == KB_CTRL_PGDN) {
            int d = (k == KB_CTRL_UP || k == KB_CTRL_PGUP) ? 1 : -1;
            if (k == KB_CTRL_PGUP || k == KB_CTRL_PGDN)
                d *= (con_rows() > 2 ? (con_rows() - 1) / 2 : 1);
            con_scrollback(d);
            continue;
        }

        /* v2.9 pinyin IME: F2 toggles; a committed candidate is a 3-byte
         * UTF-8 hanzi, inserted at the cursor (not blindly appended). */
        {
            char han[5];
            int r = ime_feed(k, han);
            if (r == IME_CONSUME) continue;
            if (r == IME_COMMIT) {
                int n = 0;
                while (han[n]) n++;
                if (rl_width(line, line_len) + rl_width(han, n) < cols - x0) {
                    rl_insert(han, n);
                    rl_redraw(x0, y0, base);
                }
                continue;
            }
        }

        if (k == KB_CTRL_C) {          /* v2.28: cancel the line */
            line_len = line_cur = 0; line[0] = 0;
            rl_redraw(x0, y0, base);
            buf[0] = 0;
            con_set_fg(base);
            return 2;
        }
        if (k == KB_TAB && rl_completer) {
            char out[KB_BUF], bar[160];
            out[0] = 0; bar[0] = 0;
            if (rl_completer(line, line_cur, out, bar) == 1 && out[0]) {
                int j = 0;
                for (; out[j] && j < KB_BUF - 1; j++) line[j] = out[j];
                line[j] = 0; line_len = line_cur = j;
                rl_redraw(x0, y0, base);
            }
            if (bar[0]) {
                int br = y0 + 1;
                if (br >= con_rows()) br = y0 - 1;
                con_status_bar_at(br, bar);
            }
            continue;
        }
        if (k == '\n') {
            int n = line_len;
            if (n >= len) n = len - 1;
            for (int i = 0; i < n; i++) buf[i] = line[i];
            buf[n] = 0;
            rl_hist_push(line);
            con_set_fg(base);
            int end_col = x0 + rl_width(line, line_len);
            if (end_col >= cols) end_col = cols - 1;
            con_set_cursor((uint8_t)end_col, (uint8_t)y0);
            con_putc('\n');
            line_len = line_cur = 0; line[0] = 0;
            return 1;
        }
        if (k == KB_UP || k == KB_DOWN) {
            if (!rl_hist_count) continue;
            if (hist_pos < 0) {
                int j = 0;
                for (; j < line_len && j < KB_BUF - 1; j++) draft[j] = line[j];
                draft[j] = 0;
            }
            int np;
            if (k == KB_UP) np = (hist_pos < 0) ? 0 : hist_pos + 1;
            else            np = (hist_pos <= 0) ? -1 : hist_pos - 1;
            if (np >= rl_hist_count) np = rl_hist_count - 1;
            hist_pos = np;
            const char *h = (np < 0) ? draft : rl_hist_get(np);
            int j = 0;
            for (; h && h[j] && j < KB_BUF - 1; j++) line[j] = h[j];
            line_len = line_cur = j; line[j] = 0;
            rl_redraw(x0, y0, base);
            continue;
        }
        if (k == KB_LEFT)  { line_cur = rl_prev(line_cur); rl_redraw(x0, y0, base); continue; }
        if (k == KB_RIGHT) { line_cur = rl_next(line_cur); rl_redraw(x0, y0, base); continue; }
        if (k == KB_HOME)  { line_cur = 0;        rl_redraw(x0, y0, base); continue; }
        if (k == KB_END)   { line_cur = line_len; rl_redraw(x0, y0, base); continue; }
        if (k == KB_CTRL_LEFT || k == KB_CTRL_RIGHT) {
            if (k == KB_CTRL_LEFT) {
                while (line_cur > 0 && line[line_cur - 1] == ' ') line_cur--;
                while (line_cur > 0 && line[line_cur - 1] != ' ') line_cur--;
            } else {
                while (line_cur < line_len && line[line_cur] != ' ') line_cur++;
                while (line_cur < line_len && line[line_cur] == ' ') line_cur++;
            }
            rl_redraw(x0, y0, base);
            continue;
        }
        if (k == '\b') {                    /* delete BEFORE the cursor */
            if (line_cur > 0) {
                int s = rl_prev(line_cur);
                rl_erase(s, line_cur);
                rl_redraw(x0, y0, base);
            }
            continue;
        }
        if (k == KB_DEL) {                  /* delete AT the cursor */
            if (line_cur < line_len) {
                int e = rl_next(line_cur);
                rl_erase(line_cur, e);
                rl_redraw(x0, y0, base);
            }
            continue;
        }
        if (k >= 32 && k < 256) {           /* printable, insert anywhere */
            char c = (char)k;
            if (rl_width(line, line_len) + 1 < cols - x0) {
                rl_insert(&c, 1);
                rl_redraw(x0, y0, base);
            }
        }
        /* other function keys are ignored in line mode */
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
