#include "keyboard.h"
#include "usb.h"
#include "io.h"
#include "vga.h"
#include "ime.h"
#include "mouse.h"
#include "clip.h"
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

/* ---- v2.62: double-tap Shift toggles the pinyin IME --------------------
 * A "tap" = Shift pressed and released WITHOUT any other key in between.
 * Two isolated taps within IME_SHIFT_DBL_TICKS emit KB_IME_TOGGLE.  Shift
 * held with a printable key (Shift+A forcaps, etc.) is a combo, not a tap,
 * so ordinary capital typing never toggles the IME. */
#define IME_SHIFT_DBL_TICKS 60   /* 100 Hz PIT -> 600 ms */
extern volatile uint32_t timer_ticks;
static uint32_t s_shift_down_tick, s_shift_last_tap;
static int      s_shift_combo, s_shift_tap_valid;

static void shift_down(void) {
    shift = 1;
    s_shift_down_tick = timer_ticks;
    s_shift_combo = 0;
}
/* v2.68: ONE isolated Shift tap toggles the pinyin IME.  Shift held
 * together with any other key is a combo (capital letters, shortcuts)
 * and never toggles. */
static int shift_up(void) {
    shift = 0;
    if (s_shift_combo) { s_shift_tap_valid = 0; return 0; }
    s_shift_last_tap = s_shift_down_tick;
    s_shift_tap_valid = 1;
    return 1;
}
#define SHIFT_MARK_COMBO() do { if (shift) s_shift_combo = 1; } while (0)

/* ---- interrupt-driven scancode queue ---------------------------------
 * kb_irq() runs at IRQ1 and only enqueues bytes + wakes waiters; all
 * decoding and echo happens in kb_getkey() at normal kernel context. */

#define SCQ_SZ 256
static volatile uint8_t scq[SCQ_SZ];
static volatile uint32_t scq_head, scq_tail;

/* v2.57: one scancode enqueue, shared by the PS/2 IRQ1 drain and the
 * PS/2-bit5 routing helper (ps2_drain) so an aux byte can never be
 * enqueued as a key and vice versa. */
void kbd_feed_byte(uint8_t sc) {
    uint32_t n = (scq_head + 1) % SCQ_SZ;
    if (n != scq_tail) { scq[scq_head] = sc; scq_head = n; }
}

void kb_irq(void) {
    ps2_drain();                        /* routes 0x60 by status bit5 */
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
 * Plain arrows and plain PgUp/PgDn are kept as-is so callers can distinguish
 * them from their Ctrl variants; only Up/Down map to KB_CTRL_UP/DOWN since
 * those were used for scrollback (v2.61: PgUp/PgDn now do it directly). */
static int e0_map(int k, int ctrl_held) {
    if (!ctrl_held) return k;
    switch (k) {
    case KB_UP:    return KB_CTRL_UP;
    case KB_DOWN:  return KB_CTRL_DOWN;
    case KB_LEFT:  return KB_CTRL_LEFT;
    case KB_RIGHT: return KB_CTRL_RIGHT;
    /* v2.61: PgUp/PgDn no longer folded -- handled directly below. */
    default:       return k;
    }
}

/* blocking: waits for the next scancode (yields the CPU while waiting) */
static int kb_next_scancode(void) {
    for (;;) {
        usb_service();                     /* v2.29: latch hot-plug attach */
        con_mouse_poll();                  /* v2.57: drain mouse events    */
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
        con_mouse_poll();                  /* v2.57 */
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

        if (sc == 0x2A || sc == 0x36) { shift_down(); continue; }
        if (sc == 0xAA || sc == 0xB6) {
            if (shift_up()) return KB_IME_TOGGLE;   /* v2.62: dbl-tap Shift */
            continue;
        }
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
        SHIFT_MARK_COMBO();          /* a real key while Shift held -> combo */

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
        if (sc == 0x2A || sc == 0x36) { shift_down(); continue; }
        if (sc == 0xAA || sc == 0xB6) {
            if (shift_up()) return KB_IME_TOGGLE;   /* v2.62: dbl-tap Shift */
            continue;
        }
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
        SHIFT_MARK_COMBO();          /* a real key while Shift held -> combo */

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
 * PgUp/PgDn    : console scrollback (one page per press)
 * Typing       : syntax highlighting (command / option / number / string)
 *
 * All redraw is done with absolute cell writes from the anchor position
 * captured at entry, so the prompt is never re-printed and the cursor can
 * sit in the middle of the line.  con_write_at_utf8() keeps CJK at exactly
 * 2xASCII, which is the invariant the graphics back-ends rely on. */
#define RL_HIST 16
static char rl_hist[RL_HIST][KB_BUF];
static int  rl_hist_head, rl_hist_count;

int kb_hist_count(void) { return rl_hist_count; }
const char *kb_hist_get(int i) {           /* 0 = oldest */
    if (i < 0 || i >= rl_hist_count) return "";
    int idx = (rl_hist_head - rl_hist_count + i + RL_HIST * 4) % RL_HIST;
    return rl_hist[idx];
}

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
static int rl_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

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

/* v2.57: middle-click paste closure.  Registered only while kb_readline
 * owns the input line; the console glue calls it with the clipboard UTF-8. */
static int      rl_paste_x0, rl_paste_y0;
static uint8_t  rl_paste_base;
static void rl_redraw(int x0, int y0, uint8_t base);
static void rl_paste_apply(const char *s, int n) {
    if (n <= 0) return;
    rl_insert(s, n);            /* v2.66: paste wraps instead of truncating */
    rl_redraw(rl_paste_x0, rl_paste_y0, rl_paste_base);
}

/* v2.66: soft-wrapped line editor state.  A long command now continues on
 * the following rows instead of being refused at the right edge; when the
 * wrapped line outgrows the screen the console scrolls up (rows archived
 * to scrollback).  rl_y0_adj remembers the (possibly scrolled) start row
 * between redraws because the callers keep passing the original anchor;
 * rl_last_rows remembers how many rows the line occupied so the next
 * redraw can wipe the full old footprint. */
static int rl_y0_adj    = -1;
static int rl_last_rows = 1;

/* Write one display char (ASCII byte or one whole UTF-8 sequence) at
 * (cx,cy), wrapping to column 0 of the next row when it would overflow. */
static void rl_putc_wrapped(const char *s, int n, int *cx, int *cy, int cols) {
    int cl = 1;
    unsigned char c0 = (unsigned char)s[0];
    if (c0 >= 0x80u)
        while (cl < n && rl_is_cont((unsigned char)s[cl])) cl++;
    int w = (c0 >= 0x80u) ? 2 : 1;
    if (*cx + w > cols) { *cx = 0; (*cy)++; }
    char tmp[8];
    if (cl > 7) cl = 7;
    for (int j = 0; j < cl; j++) tmp[j] = s[j];
    tmp[cl] = 0;
    con_write_at_utf8((uint8_t)*cx, (uint8_t)*cy, tmp);
    *cx += w;
}

/* Screen position (after wrapping) of byte offset `off` within `line`,
 * starting from anchor (x0,y0).  Must mirror rl_putc_wrapped exactly. */
static void rl_screen_pos(int off, int x0, int y0, int cols, int *pcx, int *pcy) {
    *pcx = x0; *pcy = y0;
    int i = 0;
    while (i < off && i < line_len) {
        int cl = 1;
        unsigned char c0 = (unsigned char)line[i];
        if (c0 >= 0x80u)
            while (i + cl < line_len && rl_is_cont((unsigned char)line[i + cl])) cl++;
        int w = (c0 >= 0x80u) ? 2 : 1;
        if (*pcx + w > cols) { *pcx = 0; (*pcy)++; }
        *pcx += w;
        i += cl;
    }
}

static void rl_redraw(int x0, int y0, uint8_t base) {
    int cols = con_cols();
    int rows = con_rows();
    if (x0 >= cols) x0 = cols - 1;
    if (rl_y0_adj >= 0 && rl_y0_adj < rows) y0 = rl_y0_adj;

    /* rows the wrapped line needs (including the active IME composition) */
    const char *comp = ime_composition();
    int comp_w = (comp && *comp) ? rl_width(comp, rl_strlen(comp)) : 0;
    int total = x0 + rl_width(line, line_len) + comp_w;
    int need = (total + cols - 1) / cols;
    if (need < 1) need = 1;

    /* grow downward: scroll the console so the whole wrapped line fits
     * between the prompt row and the bottom edge */
    if (y0 + need > rows) {
        int k = y0 + need - rows;
        con_scroll_up(k);
        y0 -= k;
        if (y0 < 0) y0 = 0;
        need = rows - y0;
        if (need < 1) need = 1;
    }
    rl_y0_adj = y0;

    /* wipe the previous footprint plus any rows the shorter line no
     * longer occupies (con_write_at clips at the right edge) */
    {
        char pad[160];
        int room = cols < 159 ? cols : 159;
        for (int i = 0; i < room; i++) pad[i] = ' ';
        pad[room] = 0;
        con_set_fg(base);
        int wipe = rl_last_rows > need ? rl_last_rows : need;
        for (int r = 0; r < wipe && y0 + r < rows; r++)
            con_write_at((uint8_t)(r ? 0 : x0), (uint8_t)(y0 + r), pad);
    }

    /* draw the wrapped, syntax-coloured line */
    int cx = x0, cy = y0, tok = 0, i = 0;
    while (i < line_len) {
        if (line[i] == ' ') {                 /* plain separator */
            con_set_fg(base);
            rl_putc_wrapped(line + i, 1, &cx, &cy, cols);
            i++;
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
        con_set_fg(rl_tok_color(line + i, n, tok, base));
        for (int j = 0; j < n; ) {
            int cl = 1;
            unsigned char c0 = (unsigned char)line[i + j];
            if (c0 >= 0x80u)
                while (j + cl < n && rl_is_cont((unsigned char)line[i + j + cl])) cl++;
            rl_putc_wrapped(line + i + j, cl, &cx, &cy, cols);
            j += cl;
        }
        tok++;
        i = e;
    }

    /* screen position of the text cursor */
    int curx, cury;
    rl_screen_pos(line_cur, x0, y0, cols, &curx, &cury);

    /* v2.58 stage-1: the active IME composition string is drawn inline at
     * the cursor (it lives outside the fixed bottom candidate area). */
    if (comp && *comp) {
        con_set_fg(base);
        for (const char *p = comp; *p; ) {
            int cl = 1;
            unsigned char c0 = (unsigned char)*p;
            if (c0 >= 0x80u) while (p[cl] && rl_is_cont((unsigned char)p[cl])) cl++;
            rl_putc_wrapped(p, cl, &curx, &cury, cols);
            p += cl;
        }
        int cur_col = curx;
        if (cur_col >= cols) { cur_col = 0; cury++; }
        /* v2.59 fix: blank the cell the cursor lands on so a stale glyph
         * from an earlier frame never peeks out after the pinyin string. */
        con_write_at_utf8((uint8_t)cur_col, (uint8_t)cury, " ");
    }
    if (curx >= cols) { curx = 0; cury++; }
    rl_last_rows = cury - y0 + 1;
    con_set_fg(base);
    con_set_cursor((uint8_t)curx, (uint8_t)cury);
}

int kb_readline(char *buf, int len) {
    if (len <= 0) return 0;
    int x0 = 0, y0 = 0;
    con_get_cursor(&x0, &y0);
    rl_y0_adj = -1; rl_last_rows = 1;   /* v2.66: fresh anchor per line */
    ime_set_anchor(y0);             /* v2.25: candidate bar under the line */
    uint8_t base = con_get_fg();
    int cols = con_cols();
    int hist_pos = -1;                  /* -1 = the draft line being typed */
    char draft[KB_BUF];
    line_len = line_cur = 0; line[0] = 0;
    rl_paste_x0 = x0; rl_paste_y0 = y0; rl_paste_base = base;
    con_set_paste_hook(rl_paste_apply);

    while (1) {
        int k = kb_getkey();

        /* Leaving scrollback: any non-scroll key restores the live grid
         * (including the partly typed line) and is then handled normally. */
        if (con_scrollback_view() &&
            k != KB_PGUP && k != KB_PGDN)
            con_scrollback_reset();

        if (k == KB_PGUP || k == KB_PGDN) {
            /* PgUp/PgDn = one full page (visible text rows - 1) per step.
             * text and gfx24 both go through the same con_scrollback() API. */
            int d = k == KB_PGUP ? 1 : -1;
            d *= (con_rows() > 2 ? con_rows() - 1 : 1);
            con_scrollback(d);
            continue;
        }

        /* v2.57: drain PS/2 mouse events (selection / wheel / middle-click
         * paste) even while the line editor is waiting on a key.  Paste is
         * applied through the registered rl_paste_apply hook. */
        con_mouse_poll();

        /* v2.9 pinyin IME: F2 toggles; a committed candidate is a 3-byte
         * UTF-8 hanzi, inserted at the cursor (not blindly appended). */
        {
            char han[IME_OUT_MAX];
            int r = ime_feed(k, han);
            if (r == IME_CONSUME) { rl_redraw(x0, y0, base); continue; }
            if (r == IME_COMMIT) {
                int n = 0;
                while (han[n]) n++;
                rl_insert(han, n);      /* v2.66: wraps, never refused */
                rl_redraw(x0, y0, base);
                continue;
            }
        }

        if (k == KB_CTRL_C) {          /* v2.28: cancel the line */
            line_len = line_cur = 0; line[0] = 0;
            rl_redraw(x0, y0, base);
            buf[0] = 0;
            con_set_fg(base);
            con_set_paste_hook(0);
            return 2;
        }
        if (k == KB_CTRL_Z) {          /* v2.60: suspend foreground job */
            line_len = line_cur = 0; line[0] = 0;
            rl_redraw(x0, y0, base);
            buf[0] = 0;
            con_set_fg(base);
            con_set_paste_hook(0);
            return 3;                  /* ^Z: signal caller to stop fg task */
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
                /* v2.66: put the bar under the LAST row of the (possibly
                 * wrapped) input line */
                int last = (rl_y0_adj >= 0 ? rl_y0_adj : y0) + rl_last_rows - 1;
                int br = last + 1;
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
            /* v2.66: park the cursor after the last wrapped row, then the
             * newline scrolls naturally from there */
            int ex, ey;
            {
                int ay0 = (rl_y0_adj >= 0) ? rl_y0_adj : y0;
                rl_screen_pos(line_len, x0, ay0, cols, &ex, &ey);
            }
            if (ex >= cols) { ex = 0; ey++; }
            con_set_cursor((uint8_t)ex, (uint8_t)ey);
            con_putc('\n');
            rl_y0_adj = -1; rl_last_rows = 1;
            line_len = line_cur = 0; line[0] = 0;
            con_set_paste_hook(0);
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
            rl_insert(&c, 1);               /* v2.66: wraps, never refused */
            rl_redraw(x0, y0, base);
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
