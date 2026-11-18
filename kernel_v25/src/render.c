/*
 * render.c -- CatOS display FRONT-END (v2.9).
 *
 * Owns ALL console state: the logical cell grid, cursor, current style,
 * 256-entry CLUT, UTF-8 decoder, display-mode policy, consent gate,
 * software cursor and BLINK animation.  Back-ends are stateless painters.
 *
 * v2.9 changes (all the v2.8 "known limitations" except QEMU testing):
 *   - ONE cell = 8x16 px on every back-end.  A 16x16 hanzi is 2 columns
 *     x 1 row, not 2x2.  No even-row rule, no two-row scrolling, ASCII
 *     scrolls exactly one line at a time.
 *   - VGA text mode now paints hanzi through 128 dynamic 8x16 font slots.
 *   - Back-ends may implement scroll()/draw_cursor(): scrolling is a
 *     framebuffer move instead of a full repaint, and gfx/VBE get a
 *     software cursor.  redraw_all() skips cells already covered by clear.
 *   - con_tick() (100 Hz PIT) animates BLINK by repainting only the
 *     affected cells, guarded against re-entering a paint in progress.
 *   - con_write_n() streams arbitrary-length buffers, keeping an
 *     incomplete UTF-8 sequence alive across chunk boundaries.
 */
#include "render.h"
#include "render_be.h"
#include "keyboard.h"
#include "mouse.h"
#include "clip.h"
#include "task.h"

#define CON_MAX_COLS 100        /* 800 / 8, the widest (VBE) terminal */
#define CON_MAX_ROWS 80         /* 592 / 8  -> 75 rows, compact scale */
#define BLINK_TICKS  50         /* 100 Hz PIT -> 0.5 s half-period    */

static con_cell_t cells[CON_MAX_ROWS][CON_MAX_COLS];
static con_cell_t tmp_cells[CON_MAX_ROWS][CON_MAX_COLS];
static render_be_t *be;
static int cols, rows, cur_x, cur_y;

/* v2.21 T51: console scrollback.  Every row that scrolls off the top is
 * archived here.  sb_head is the next write slot, so sb[sb_head-1] is the
 * most recently archived row.  sb_live keeps a verbatim copy of the live
 * grid while the user is looking at history. */
#define CON_SB_ROWS 96
static con_cell_t sb[CON_SB_ROWS][CON_MAX_COLS];
static int sb_head, sb_count, sb_view;
static con_cell_t sb_live[CON_MAX_ROWS][CON_MAX_COLS];

/* v2.25 full-screen UI takeover backup (one level) */
static con_cell_t ui_backup[CON_MAX_ROWS][CON_MAX_COLS];
static int ui_backup_cur_x, ui_backup_cur_y, ui_backup_valid;

/* current style */
static uint32_t s_fg_rgb, s_bg_rgb, clear_bg;
static uint8_t  s_fg_idx, s_bg_idx, s_attrs;

/* 256-entry colour look-up table (owned here, read by the back-ends) */
static uint32_t clut[256];

/* UTF-8 incremental decoder */
static uint32_t u_cp;
static int      u_need;

/* animation / re-entrancy */
static volatile uint32_t blink_ticks, blink_phase, blink_pending;
static volatile int      render_busy;
static int pending_wide;        /* text-mode CJK needs a font-slot rebuild */
static int dcx = -1, dcy = -1;  /* last software-cursor position */

/* active text-mode font-slot map: gslot -> codepoint */
static uint32_t hz_map[CON_TEXT_HZ_SLOTS];

/* transient status bar row (pinyin IME / tab completer, legacy single) */
static con_cell_t saved_row[CON_MAX_COLS];
static int        saved_row_valid;
static int        saved_row_index = -1;   /* v2.25: WHICH row is barrowed */

/* v2.57 mouse cell selection.  The bottom two rows are reserved for the
 * IME candidate bar, so the selectable region is rows 0 .. rows-3. */
static int sel_valid;              /* selection has an anchor + endpoint */
static int sel_dragging;           /* left button still held             */
static int sel_ax, sel_ay;         /* anchor cell                        */
static int sel_cx, sel_cy;         /* current endpoint cell              */
static int paste_pending;          /* middle-click arms a paste          */
static con_paste_fn paste_hook;
static int sel_at(int x, int y);

/* v2.58: two-row fixed IME candidate area -- separator row (rows-2) and
 * candidate row (rows-1).  Both rows' original cells are saved verbatim
 * and restored on `con_status_bar_off()`, so leaving the IME never
 * scratches shell output or editor content (text slots rebuilt too). */
#define SB2_ROWS 2
static con_cell_t saved2[SB2_ROWS][CON_MAX_COLS];
static int        saved2_valid;

/* v2.25 dirty-CELL tracker (the CatOS dirty rectangle: our unit IS the
 * cell).  painted[] remembers the last PIXELS actually on screen for a
 * position; paint() skips the framebuffer write when the next cell to
 * draw is byte-identical.  Repeated identical output (progress bars,
 * blink overlays, self-redrawing apps) costs ZERO VRAM writes.
 * painted_valid is cleared whenever the grid and the framebuffer may
 * disagree (mode switch, clear, scrollback view, scale change). */
static con_cell_t painted[CON_MAX_ROWS][CON_MAX_COLS];
static uint8_t    painted_cur[CON_MAX_ROWS][CON_MAX_COLS]; /* cursor shown */
static uint8_t    painted_valid[CON_MAX_ROWS][CON_MAX_COLS];
static uint32_t   stat_drawn, stat_skipped;

static void painted_invalidate_all(void) {
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            painted_valid[y][x] = 0;
}

/* ---- colour helpers ----------------------------------------------------- */
uint32_t render_clut(uint8_t idx) { return clut[idx]; }

uint8_t render_quantize(uint32_t rgb) {
    int r = CON_RGB_R(rgb), g = CON_RGB_G(rgb), b = CON_RGB_B(rgb);
    uint8_t best = 0;
    long best_d = 0x7FFFFFFF;
    for (int i = 0; i < 256; i++) {
        int dr = r - CON_RGB_R(clut[i]);
        int dg = g - CON_RGB_G(clut[i]);
        int db = b - CON_RGB_B(clut[i]);
        long d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (d < best_d) { best_d = d; best = (uint8_t)i; }
    }
    return best;
}

/* VGA's 16-colour index is BGR + intensity, not RGB.  First try an exact
 * nearest match against the modern CLUT 0..15 so theme colours keep their
 * dedicated DAC slot; only unmatched arbitrary RGB falls back to the
 * legacy 3-bit threshold ramp. */
uint8_t render_rgb_to_vga(uint32_t rgb) {
    int r = CON_RGB_R(rgb), g = CON_RGB_G(rgb), b = CON_RGB_B(rgb);
    uint8_t best = 0;
    long best_d = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        int dr = r - CON_RGB_R(clut[i]);
        int dg = g - CON_RGB_G(clut[i]);
        int db = b - CON_RGB_B(clut[i]);
        long d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (d < best_d) { best_d = d; best = (uint8_t)i; }
    }
    if (best_d < 24 * 24 * 3) return best;
    uint8_t i = (uint8_t)((b >= 128 ? 1 : 0) | (g >= 128 ? 2 : 0) |
                          (r >= 128 ? 4 : 0));
    if (r > 192 || g > 192 || b > 192) i |= 8;
    return i;
}

static void clut_init(void) {
    /* v2.37 modern terminal theme (P0-2): single table drives both the text
     * DAC and the truecolour back-end.  bg #0C0C0C, body #DCDCDC, secondary
     * #8B949E; accent green #7EE787, path blue #60A5FA; semantic
     * red #F85149 / yellow #E3B341 / cyan #39C5CF / success green #56D364. */
    static const uint32_t vga16[16] = {
        0x0C0C0C,0x1F6FEB,0x56D364,0x39C5CF,
        0xF85149,0xD299DD,0xE3B341,0xDCDCDC,
        0x8B949E,0x60A5FA,0x7EE787,0x39C5CF,
        0xF85149,0xD299DD,0xE3B341,0xFFFFFF
    };
    for (int i = 0; i < 16; i++) clut[i] = vga16[i];
    for (int i = 16; i < 256; i++) {
        uint8_t g = (uint8_t)((i - 16) * 255 / 239);
        clut[i] = CON_RGB(g, g, g);
    }
}

/* ---- cell helpers ------------------------------------------------------- */
static void cell_blank(con_cell_t *c) {
    c->cp = 0;
    c->fg = s_fg_rgb;   c->bg = s_bg_rgb;
    c->fg_idx = s_fg_idx; c->bg_idx = s_bg_idx;
    c->attrs = s_attrs; c->flags = CELL_F_SINGLE;
    c->gslot = CELL_GSLOT_NONE;
}
static void cell_set(con_cell_t *c, uint32_t cp, uint8_t flags) {
    c->cp = cp;
    c->fg = s_fg_rgb;   c->bg = s_bg_rgb;
    c->fg_idx = s_fg_idx; c->bg_idx = s_bg_idx;
    c->attrs = s_attrs; c->flags = flags;
    c->gslot = CELL_GSLOT_NONE;
}

/* A WIDE_R is never painted on its own; its glyph comes from WIDE_L.
 * render_busy fences the PIT IRQ out of a half-drawn cell. */
static void paint(int x, int y) {
    if (x < 0 || x >= cols || y < 0 || y >= rows) return;
    con_cell_t *c = &cells[y][x];
    if (c->flags == CELL_F_WIDE_R) return;
    int is_cur = (be->draw_cursor && x == cur_x && y == cur_y);
    int show_cur = is_cur && !blink_phase;   /* dark half of the blink */
    /* v2.25 dirty-cell skip: compare against what is REALLY on screen. */
    if (painted_valid[y][x] && painted_cur[y][x] == (uint8_t)show_cur) {
        con_cell_t t = *c;
        if (be->sw_blink && (t.attrs & CON_ATTR_BLINK) && !blink_phase)
            t.attrs |= CON_ATTR_HIDDEN;
        con_cell_t p = painted[y][x];
        if (t.cp == p.cp && t.fg == p.fg && t.bg == p.bg &&
            t.attrs == p.attrs && t.flags == p.flags) {
            stat_skipped++;
            return;
        }
    }
    render_busy++;
    con_cell_t t = *c;
    if (be->sw_blink && (t.attrs & CON_ATTR_BLINK) && !blink_phase)
        t.attrs |= CON_ATTR_HIDDEN;      /* software blink: hide text */
    if (sel_at(x, y)) {                  /* v2.57 selection: inverse */
        uint32_t f = t.fg, b = t.bg;
        t.fg = b; t.bg = f;
    }
    if (show_cur) {
        /* Desired terminal cursor: a cell with no glyph shows an UNDERLINE;
         * a cell that already holds a glyph shows a BACKGROUND-INVERSE
         * (reverse-video) overlay.  Both blink with the BLINK phase so a
         * stale overlay never lingers (the old #58 block trail is gone). */
        if (t.cp == 0) {
            be->draw_cell(x, y, &t);
            be->draw_cursor(x, y, t.fg);          /* underline (empty cell) */
        } else {
            con_cell_t inv = t;                    /* swap fg/bg = inverse */
            uint32_t f = t.fg, b = t.bg;
            inv.fg = b; inv.bg = f;
            be->draw_cell(x, y, &inv);             /* reverse video (filled) */
        }
    } else {
        be->draw_cell(x, y, &t);
    }
    painted[y][x] = t;
    painted_cur[y][x] = (uint8_t)show_cur;
    painted_valid[y][x] = 1;
    stat_drawn++;
    render_busy--;
}

/* Move the software cursor by repainting the old cell (without overlay)
 * and the new one (with overlay).  Text mode uses the hardware cursor. */
static void sync_cursor(int force) {
    if (!be->draw_cursor) {
        if (be->set_cursor) be->set_cursor(cur_x, cur_y);
        dcx = dcy = -1;
        return;
    }
    if (force || dcx != cur_x || dcy != cur_y) {
        if (dcx >= 0) paint(dcx, dcy);
        paint(cur_x, cur_y);
        dcx = cur_x; dcy = cur_y;
    }
}

/* Text mode: assign the 128 spare 8x16 VGA font slots (0x80..0xFF) to
 * the distinct hanzi currently visible, in first-appearance order.
 * Rebuilt on every full redraw, so it needs no LRU and no persistent
 * state.  Overflow degrades to '?' (gfx/VBE have no such limit). */
static void hz_rebuild_map(void) {
    for (int i = 0; i < CON_TEXT_HZ_SLOTS; i++) hz_map[i] = 0;
    int n = 0;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            con_cell_t *c = &cells[y][x];
            if (c->flags != CELL_F_WIDE_L) continue;
            int slot = -1;
            for (int i = 0; i < n; i++)
                if (hz_map[i] == c->cp) { slot = i; break; }
            if (slot < 0 && n < CON_TEXT_HZ_SLOTS) {
                hz_map[n] = c->cp; slot = n++;
            }
            if (slot < 0) {
                c->cp = '?'; c->flags = CELL_F_SINGLE;
                c->gslot = CELL_GSLOT_NONE;
                if (x + 1 < cols && cells[y][x + 1].flags == CELL_F_WIDE_R)
                    cell_blank(&cells[y][x + 1]);
                continue;
            }
            c->gslot = (uint8_t)slot;
        }
}

static void redraw_all(void) {
    if (be->mode == CON_MODE_TEXT) hz_rebuild_map();
    clear_bg = s_bg_rgb;
    painted_invalidate_all();
    be->clear(clear_bg);
    /* Everything already covered by clear() needs no repaint. */
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            con_cell_t *c = &cells[y][x];
            if (c->flags == CELL_F_WIDE_R) continue;
            if (c->cp == 0 && c->bg == clear_bg) {
                /* be->clear() already put these pixels on screen. */
                painted[y][x] = *c;
                painted_cur[y][x] = 0;
                painted_valid[y][x] = 1;
                continue;
            }
            paint(x, y);
        }
    sync_cursor(1);
}

/* Repaint only cells whose appearance changes with the blink phase. */
static void blink_apply(void) {
    if (!be || !be->sw_blink) return;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            con_cell_t *c = &cells[y][x];
            if (c->flags != CELL_F_WIDE_R && (c->attrs & CON_ATTR_BLINK))
                paint(x, y);
        }
    if (be->draw_cursor) paint(cur_x, cur_y);
}

void con_tick(void) {
    if (!be) return;
    if (++blink_ticks < BLINK_TICKS) return;
    blink_ticks = 0;
    blink_phase ^= 1u;
    if (!render_busy) { blink_pending = 0; blink_apply(); }
    else               blink_pending = 1;
}

static void render_flush(void) {
    if (pending_wide) { pending_wide = 0; redraw_all(); }
    else if (blink_pending && !render_busy) {
        blink_pending = 0; blink_apply();
    }
}

/* ONE row at a time.  Because a hanzi is 2x1 (not 2x2), a scroll can
 * never cut a glyph in half -- no even-row invariant is needed. */
static void scroll_one(void) {
    /* Archive the row we are about to discard BEFORE shifting the grid. */
    if (sb_view == 0) {
        for (int x = 0; x < cols && x < CON_MAX_COLS; x++)
            sb[sb_head][x] = cells[0][x];
        sb_head = (sb_head + 1) % CON_SB_ROWS;
        if (sb_count < CON_SB_ROWS) sb_count++;
    }
    for (int y = 1; y < rows; y++)
        for (int x = 0; x < cols; x++) cells[y - 1][x] = cells[y][x];
    for (int x = 0; x < cols; x++) cell_blank(&cells[rows - 1][x]);
    cur_y--; if (cur_y < 0) cur_y = 0;
    /* The dirty tracker mirrors the grid: rows shift up with it, the new
     * bottom row is whatever be->scroll()/redraw_all() leaves there. */
    for (int y = 1; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            painted[y - 1][x]     = painted[y][x];
            painted_cur[y - 1][x] = painted_cur[y][x];
            painted_valid[y - 1][x] = painted_valid[y][x];
        }
    for (int x = 0; x < cols; x++) painted_valid[rows - 1][x] = 0;
    /* Erase a software cursor drawn into the LFB BEFORE the pan moves the
     * framebuffer; otherwise its pixels scroll with the viewport and leave
     * a ghost one line higher (QEMU -vga std with hardware panning, seen
     * as a stray cursor after a newline). */
    if (be->draw_cursor && dcx >= 0) paint(dcx, dcy);
    if (be->scroll && be->scroll(1, s_bg_rgb) == 0) {
        /* Backend moved the pixels itself (VBE hardware panning). */
        clear_bg = s_bg_rgb;
        sync_cursor(1);
    } else {
        /* Full re-render from the grid: write-only, never reads VRAM
         * (text mode also rebuilds its dynamic font-slot map here). */
        redraw_all();
    }
}

/* Overwriting either half of a hanzi must erase BOTH halves. */
static void break_block(int x, int y) {
    if (x < 0 || x >= cols || y < 0 || y >= rows) return;
    int bx = x, w = 1;
    if (cells[y][x].flags == CELL_F_WIDE_R && x > 0 &&
        cells[y][x - 1].flags == CELL_F_WIDE_L) { bx = x - 1; w = 2; }
    else if (cells[y][x].flags == CELL_F_WIDE_L) w = 2;
    for (int i = 0; i < w; i++) {
        if (bx + i < cols) {
            cell_blank(&cells[y][bx + i]);
            paint(bx + i, y);
        }
    }
}

static void ensure_room(int wide) {
    int w = wide ? 2 : 1;
    if (cur_x + w > cols) { cur_x = 0; cur_y++; }
    while (cur_y >= rows) scroll_one();
}

static void cell_put_cp(uint32_t cp) {
    switch (cp) {
    case '\n':
        cur_x = 0; cur_y++;
        while (cur_y >= rows) scroll_one();
        break;
    case '\r': cur_x = 0; break;
    case '\b':
        if (cur_x > 0) {
            cur_x--;
            if (cells[cur_y][cur_x].flags == CELL_F_WIDE_R && cur_x > 0)
                cur_x--;
            break_block(cur_x, cur_y);
            cell_blank(&cells[cur_y][cur_x]);
            paint(cur_x, cur_y);
        }
        break;
    case '\t': {
        int nx = (cur_x + 4) & ~3;
        while (cur_x < nx) cell_put_cp(' ');
        return;                         /* recursion already syncs cursor */
    }
    default: {
        int wide = 0;
        /* v2.16: "is there a 16x16 glyph for this codepoint" is the sole
         * criterion for a double-width cell.  The old hardcoded
         * cp >= 0x1100 excluded CJK punctuation/symbols outside that
         * range and degraded them all to '?'.  It also let 0x80..0xFF
         * fall through to render_font_glyph(), whose basic table covers
         * only 0x20..0x7E -- an out-of-bounds read. */
        if (cp >= 0x80 && be->cjk && render_font_hz(cp)) wide = 1;
        else if (cp > 0x7E || cp < 0x20) cp = '?';
        ensure_room(wide);
        break_block(cur_x, cur_y);
        if (wide && cur_x + 1 < cols) break_block(cur_x + 1, cur_y);
        if (wide) {
            cell_set(&cells[cur_y][cur_x], cp, CELL_F_WIDE_L);
            cell_set(&cells[cur_y][cur_x + 1], 0, CELL_F_WIDE_R);
            if (be->mode == CON_MODE_TEXT) pending_wide = 1;
            else paint(cur_x, cur_y);
            cur_x += 2;
        } else {
            cell_set(&cells[cur_y][cur_x], cp, CELL_F_SINGLE);
            paint(cur_x, cur_y);
            cur_x++;
        }
        break;
    }
    }
    if (cur_x >= cols) { cur_x = 0; cur_y++; }
    while (cur_y >= rows) scroll_one();
    sync_cursor(0);
}

/* ---- UTF-8 -------------------------------------------------------------- */
static int utf8_feed(unsigned char b, uint32_t *out) {
    if (u_need == 0) {
        if (b < 0x80) { *out = b; return 1; }
        if ((b & 0xE0) == 0xC0) { u_cp = b & 0x1F; u_need = 1; return 0; }
        if ((b & 0xF0) == 0xE0) { u_cp = b & 0x0F; u_need = 2; return 0; }
        if ((b & 0xF8) == 0xF0) { u_cp = b & 0x07; u_need = 3; return 0; }
        *out = '?'; return 1;
    }
    if ((b & 0xC0) != 0x80) { u_need = 0; *out = '?'; return 1; }
    u_cp = (u_cp << 6) | (b & 0x3F);
    if (--u_need == 0) {
        *out = u_cp;
        if (u_cp > 0x10FFFFu || (u_cp >= 0xD800u && u_cp <= 0xDFFFu))
            *out = '?';
        return 1;
    }
    return 0;
}

void con_flush_utf8(void) {
    if (u_need) { u_need = 0; cell_put_cp('?'); }
}

/* Stateless single-codepoint decoder for positioned writes. */
static int utf8_one(const char *s, uint32_t *out) {
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp; int n;
    if (p[0] < 0x80)                    { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0)    { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0)    { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8) == 0xF0)    { cp = p[0] & 0x07; n = 4; }
    else { *out = '?'; return 1; }
    for (int i = 1; i < n; i++) {
        if (!p[i] || (p[i] & 0xC0) != 0x80) { *out = '?'; return i; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = '?';
    *out = cp;
    return n;
}

/* ---- reflow: re-wrap into a differently sized 8x16 grid ---------------- */
static void tmp_shift(int n) {
    if (n <= 0) return;
    if (n > CON_MAX_ROWS) n = CON_MAX_ROWS;
    for (int y = n; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            tmp_cells[y - n][x] = tmp_cells[y][x];
    for (int y = CON_MAX_ROWS - n; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cell_blank(&tmp_cells[y][x]);
}

static void reflow(int oc, int orow, int nc, int nrow) {
    if (oc > CON_MAX_COLS) oc = CON_MAX_COLS;
    if (orow > CON_MAX_ROWS) orow = CON_MAX_ROWS;
    if (nc > CON_MAX_COLS) nc = CON_MAX_COLS;
    if (nrow > CON_MAX_ROWS) nrow = CON_MAX_ROWS;
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cell_blank(&tmp_cells[y][x]);

    int nx = 0, ny = 0;
    for (int y = 0; y < orow; y++) {
        for (int x = 0; x < oc; x++) {
            con_cell_t *c = &cells[y][x];
            if (c->flags == CELL_F_WIDE_R) continue;
            int w = (c->flags == CELL_F_WIDE_L) ? 2 : 1;
            if (nx + w > nc) { nx = 0; ny++; }
            if (ny >= nrow) { tmp_shift(ny - nrow + 1); ny = nrow - 1; }
            tmp_cells[ny][nx] = *c;
            tmp_cells[ny][nx].gslot = CELL_GSLOT_NONE;
            if (w == 2 && nx + 1 < nc) {
                if (x + 1 < oc) tmp_cells[ny][nx + 1] = cells[y][x + 1];
                else cell_blank(&tmp_cells[ny][nx + 1]);
                tmp_cells[ny][nx + 1].flags = CELL_F_WIDE_R;
                tmp_cells[ny][nx + 1].gslot = CELL_GSLOT_NONE;
            }
            nx += w;
        }
        nx = 0; ny++;
        if (ny >= nrow) { tmp_shift(ny - nrow + 1); ny = nrow - 1; }
    }
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cells[y][x] = tmp_cells[y][x];
}

static int switch_backend(render_be_t *nb) {
    render_be_t *old = be;
    int oc = cols, orow = rows;
    /* Shut the old hardware down FIRST: VBE's dispi disable must precede
     * a Mode 13h / text-mode register sequence, not follow it. */
    old->leave();
    if (nb->enter() != 0) {
        old->enter();                   /* restore the old mode */
        return CON_ENODEV;
    }
    reflow(oc, orow, nb->cols, nb->rows);
    be = nb;
    cols = be->cols;
    rows = be->rows;
    mouse_clamp((int)be->pixel_w, (int)be->pixel_h);
    saved_row_valid = 0;           /* bottom row changed size/position */
    saved2_valid = 0;
    dcx = dcy = -1;
    sb_view = 0;                   /* archive rows never match the new grid */
    redraw_all();
    if (cur_x > cols) cur_x = cols;
    if (cur_y >= rows) cur_y = rows - 1;
    sync_cursor(1);
    return 0;
}

/* ---- consent gate ------------------------------------------------------- */
static int con_ask_consent(int origin) {
    con_write("\n[display] Leaving graphics mode requires your consent.\n");
    if (origin == CON_ORIGIN_TASK)
        con_write("[display] A user program requested TEXT mode.\n");
    con_write("[display] Press Y to allow, any other key to deny: ");
    for (;;) {
        while (!kb_peek()) task_block(T_WAIT_KB);
        int k = kb_getkey();
        if (k == 'y' || k == 'Y') {
            con_write("Y\n[display] consent granted.\n");
            return 1;
        }
        if (k < 0) continue;
        con_write("\n[display] consent denied - mode unchanged.\n");
        return 0;
    }
}

/* ---- public API --------------------------------------------------------- */
void render_init(void) {
    clut_init();
    be = &be_text;
    cols = be->cols; rows = be->rows;
    cur_x = cur_y = 0;
    u_cp = 0; u_need = 0;
    blink_ticks = blink_phase = blink_pending = 0;
    render_busy = 0;
    pending_wide = 0;
    dcx = dcy = -1;
    s_fg_idx = CON_LIGHT_GREY; s_bg_idx = CON_BLACK; s_attrs = 0;
    s_fg_rgb = clut[s_fg_idx]; s_bg_rgb = clut[s_bg_idx];
    clear_bg = s_bg_rgb;
    /* Do NOT call be->enter() at boot: GRUB already set text mode 03. */
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++) cell_blank(&cells[y][x]);
    be->clear(s_bg_rgb);
    sync_cursor(1);
}

void con_clear(void) {
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++) cell_blank(&cells[y][x]);
    cur_x = cur_y = 0; u_need = 0;
    for (int i = 0; i < CON_TEXT_HZ_SLOTS; i++) hz_map[i] = 0;
    saved_row_valid = 0;
    saved2_valid = 0;
    dcx = dcy = -1;
    redraw_all();
    sync_cursor(1);
}

void con_putc(char c) { con_write_n(&c, 1, 1); }

void con_write(const char *s) {
    if (!s) return;
    int n = 0;
    while (s[n]) n++;
    con_write_n(s, n, 1);
}
void con_write_utf8(const char *s) { con_write(s); }

void con_write_n(const char *s, int n, int final) {
    if (!s || n <= 0) {
        if (final) con_flush_utf8();
        render_flush();
        return;
    }
    for (int i = 0; i < n; i++) {
        uint32_t cp;
        if (utf8_feed((unsigned char)s[i], &cp)) cell_put_cp(cp);
    }
    if (final) con_flush_utf8();
    render_flush();
}

void con_set_style(uint8_t fi, uint8_t bi, uint8_t at) {
    s_fg_idx = fi; s_bg_idx = bi; s_attrs = at;
    s_fg_rgb = clut[fi]; s_bg_rgb = clut[bi];
}
void con_set_style_rgb(uint32_t fg, uint32_t bg, uint8_t at) {
    s_fg_rgb = fg & 0x00FFFFFFu; s_bg_rgb = bg & 0x00FFFFFFu;
    s_fg_idx = render_quantize(s_fg_rgb);
    s_bg_idx = render_quantize(s_bg_rgb);
    s_attrs = at;
}
void con_set_fg(uint8_t fi)     { s_fg_idx = fi; s_fg_rgb = clut[fi]; }
void con_set_fg_rgb(uint32_t c) { s_fg_rgb = c & 0x00FFFFFFu;
                                  s_fg_idx = render_quantize(s_fg_rgb); }
void con_set_attrs(uint8_t at)  { s_attrs = at; }
uint8_t con_get_fg(void)        { return s_fg_idx; }
uint8_t con_get_attrs(void)     { return s_attrs; }

int con_set_palette(uint8_t idx, uint32_t rgb) {
    clut[idx] = rgb & 0x00FFFFFFu;
    if (be->set_palette) be->set_palette(idx, clut[idx]);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            if (cells[y][x].fg_idx == idx) cells[y][x].fg = clut[idx];
            if (cells[y][x].bg_idx == idx) cells[y][x].bg = clut[idx];
        }
    redraw_all();
    return 0;
}
uint32_t con_palette(uint8_t idx) { return clut[idx]; }

void con_move_cursor(void) { sync_cursor(1); }

void con_set_cursor(uint8_t x, uint8_t y) {
    if ((int)x >= cols) x = (uint8_t)(cols - 1);
    if ((int)y >= rows) y = (uint8_t)(rows - 1);
    cur_x = x; cur_y = y;
    /* Keep dcx/dcy so sync_cursor() erases the OLD cursor position.
     * Resetting them to -1 left the previous cursor painted on screen --
     * the T51 line editor hit this as a stray cursor after every edit and
     * after each newline. */
    sync_cursor(1);
}

void con_get_cursor(int *x, int *y) {
    if (x) *x = cur_x;
    if (y) *y = cur_y;
}

void con_putc_at(uint8_t x, uint8_t y, char c) {
    if ((int)x >= cols || (int)y >= rows) return;
    break_block(x, y);
    cell_set(&cells[y][x], (unsigned char)c, CELL_F_SINGLE);
    paint(x, y);
}
void con_write_at(uint8_t x, uint8_t y, const char *s) {
    if (!s) return;
    while (*s && (int)x < cols) con_putc_at(x++, y, *s++);
}

void con_write_at_utf8(uint8_t x, uint8_t y, const char *s) {
    if (!s) return;
    int cx = x, cy = y;
    while (*s && cx < cols && cy < rows) {
        uint32_t cp;
        int used = utf8_one(s, &cp);
        s += used;
        int wide = 0;
        /* v2.16: glyph-table driven, see cell_put_cp() above. */
        if (cp >= 0x80 && be->cjk && render_font_hz(cp)) wide = 1;
        else if (cp > 0x7E || cp < 0x20) cp = '?';
        int w = wide ? 2 : 1;
        if (cx + w > cols) break;
        break_block(cx, cy);
        if (w == 2) break_block(cx + 1, cy);
        cell_set(&cells[cy][cx], cp,
                 wide ? CELL_F_WIDE_L : CELL_F_SINGLE);
        if (w == 2) cell_set(&cells[cy][cx + 1], 0, CELL_F_WIDE_R);
        if (be->mode == CON_MODE_TEXT && wide) pending_wide = 1;
        else paint(cx, cy);
        cx += w;
    }
    render_flush();
}

/* ---- transient status bar ------------------------------------------------
 * The IME needs a candidate line without permanently eating a console
 * row.  We keep a verbatim copy of the bottom row and put it back when
 * the composition ends.  Only the inverse attribute is changed, so a
 * text-mode font-slot rebuild (redraw_all) cannot repaint the screen in
 * the bar's colours. */
void con_status_bar(const char *s) { con_status_bar_at(rows - 1, s); }

void con_status_bar_at(int row, const char *s) {
    if (!s) return;
    if (row < 0 || row >= rows) return;
    /* Only one bar at a time: if the bar moved (IME following the input
     * line), put the previous row back before borrowing the new one. */
    if (saved_row_valid && row != saved_row_index) con_status_bar_off();
    if (!saved_row_valid) {
        for (int x = 0; x < cols; x++) saved_row[x] = cells[row][x];
        saved_row_valid = 1;
        saved_row_index = row;
    }
    uint8_t old_attrs = s_attrs;
    s_attrs |= CON_ATTR_INVERSE;
    for (int x = 0; x < cols; x++) cell_blank(&cells[row][x]);
    con_write_at_utf8(0, (uint8_t)row, s);
    s_attrs = old_attrs;
    for (int x = 0; x < cols; x++) paint(x, row);
    sync_cursor(1);
}

/* v2.58: fixed two-row IME area.  `sep` paints the separator row (rows-2)
 * -- an empty string fills the whole row with '-' (U+2500 has no glyph in
 * the built-in font; scheme A of ime-plan 3.1.2).  `cand` paints the
 * candidate row (rows-1).  Original cells of BOTH rows are saved once
 * and put back verbatim by con_status_bar_off(). */
static void invalidate_row(int y) {
    if (y < 0 || y >= rows) return;
    for (int x = 0; x < cols; x++) painted_valid[y][x] = 0;
}

void con_status_bar2(const char *sep, const char *cand) {
    if (!be || rows < 2) return;
    con_status_bar_off();                 /* release a legacy single bar */
    int r1 = rows - 2, r2 = rows - 1;
    if (!saved2_valid) {
        for (int x = 0; x < cols; x++) {
            saved2[0][x] = cells[r1][x];
            saved2[1][x] = cells[r2][x];
        }
        saved2_valid = 1;
    }
    /* Drop the dirty-cell cache for both borrowed rows: a repaint that
     * shrinks the previous wide CJK block to a short ASCII line must not
     * leave stale halves on screen. */
    invalidate_row(r1);
    invalidate_row(r2);
    uint8_t old_attrs = s_attrs;
    s_attrs |= CON_ATTR_INVERSE;
    for (int x = 0; x < cols; x++) cell_blank(&cells[r1][x]);
    if (sep && *sep) {
        con_write_at_utf8(0, (uint8_t)r1, sep);
    } else {
        char dash[CON_MAX_COLS + 1];
        for (int x = 0; x < cols; x++) dash[x] = '-';
        dash[cols] = 0;
        con_write_at((uint8_t)0, (uint8_t)r1, dash);
    }
    for (int x = 0; x < cols; x++) cell_blank(&cells[r2][x]);
    if (cand && *cand) con_write_at_utf8(0, (uint8_t)r2, cand);
    s_attrs = old_attrs;
    for (int x = 0; x < cols; x++) { paint(x, r1); paint(x, r2); }
    render_flush();
    sync_cursor(1);
}

void con_status_bar_off(void) {
    if (saved2_valid) {
        int r1 = rows - 2, r2 = rows - 1;
        if (r1 < 0) r1 = 0;
        for (int x = 0; x < cols; x++) {
            cells[r1][x] = saved2[0][x];
            cells[r2][x] = saved2[1][x];
        }
        saved2_valid = 0;
        if (be->mode == CON_MODE_TEXT) redraw_all();  /* rebuild CJK slots */
        else for (int x = 0; x < cols; x++) { paint(x, r1); paint(x, r2); }
    }
    if (!saved_row_valid) { sync_cursor(1); return; }
    int row = saved_row_index >= 0 ? saved_row_index : rows - 1;
    for (int x = 0; x < cols; x++) cells[row][x] = saved_row[x];
    saved_row_valid = 0;
    saved_row_index = -1;
    if (be->mode == CON_MODE_TEXT) redraw_all();  /* reassign font slots */
    else for (int x = 0; x < cols; x++) paint(x, row);
    sync_cursor(1);
}

/* ---- v2.21 T51: scrollback viewer --------------------------------------
 * The archived rows are only ever PAINTED; the live grid is restored from
 * sb_live when the offset returns to 0.  No output is produced while the
 * viewer is active, so scroll_one() cannot corrupt the archive. */
static void sb_repaint(void) {
    if (be->mode == CON_MODE_TEXT) redraw_all();   /* rebuild CJK slots */
    else for (int y = 0; y < rows; y++)
             for (int x = 0; x < cols; x++) paint(x, y);
    sync_cursor(1);
}

/* Archive index visible at grid row y.  The block holds the newest `show`
 * archived rows (top = oldest of the visible window, bottom = newest):
 * top row j=sb_view-1, bottom row j=sb_view-show. */
static int sb_j_for_row(int y) {
    int show = sb_view < (rows - 1) ? sb_view : (rows - 1);
    int top = rows - show;
    return sb_view - 1 - (y - top);
}

/* Copy archive row j (0 = newest archived) into cell row y. */
static void sb_row_to_cell(int y, int j) {
    int k = (sb_head - 1 - j + CON_SB_ROWS * 2) % CON_SB_ROWS;
    for (int x = 0; x < cols; x++) cells[y][x] = sb[k][x];
}

/* Move one grid row src -> dst.  The destination's dirty tracker is
 * invalidated so paint() actually redraws the moved-in content onto the
 * framebuffer (copying the tracker would make paint skip a row whose
 * pixels on screen are still the old destination row). */
static void sb_move_row(int dst, int src) {
    for (int x = 0; x < cols; x++) {
        cells[dst][x]         = cells[src][x];
        painted_valid[dst][x] = 0;
    }
}

static void sb_paint_row(int y) {
    for (int x = 0; x < cols; x++) paint(x, y);
}

static void sb_draw_hint_row(void) {
    uint8_t old_attrs = s_attrs;
    s_attrs |= CON_ATTR_INVERSE;
    for (int x = 0; x < cols; x++) cell_blank(&cells[0][x]);
    con_write_at_utf8(0, 0, "-- scrollback: Ctrl+Up/Down, any other key returns --");
    s_attrs = old_attrs;
}

/* First entry into history: blank the live grid's body, draw the hint row
 * and lay the newest `show` archived rows along the bottom.  This one
 * transition repaints the block; every later step moves row by row. */
static void sb_enter_history(int show) {
    for (int y = 1; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            cell_blank(&cells[y][x]);
            painted_valid[y][x] = 0;
        }
    sb_draw_hint_row();
    for (int i = 0; i < show; i++) {
        int y = rows - show + i;
        sb_row_to_cell(y, sb_j_for_row(y));
    }
    sb_repaint();
}

/* One single-row viewer transition.  dir +1 = older, -1 = newer.
 * Returns 0 when the boundary is reached (no step taken).
 *
 * The history block maps row y to archive j = rows-1-y.  While the block
 * GROWS (older, not yet full) the existing rows keep their content and only
 * the new top row is filled; once the block is full an older step shifts the
 * whole block down one row and drops the oldest.  The newer direction mirrors
 * this: shrinking keeps the lower rows and blanks the vacated top slot; a
 * full block shifts up one row and refreshes the bottom from archive j=0. */
static int sb_step(int dir) {
    int show = sb_view < (rows - 1) ? sb_view : (rows - 1);

    if (dir > 0) {
        if (sb_view >= sb_count) return 0;
        int old_show = show;
        sb_view++;
        int new_show = sb_view < (rows - 1) ? sb_view : (rows - 1);
        int top = rows - new_show;
        if (new_show > old_show) {
            /* Grow: insert one older row at the top, keep the rest. */
            sb_row_to_cell(top, sb_j_for_row(top));
            sb_paint_row(top);
        } else {
            /* Full block: shift down, drop the oldest, fill the top. */
            for (int y = rows - 1; y > top; y--) sb_move_row(y, y - 1);
            sb_row_to_cell(top, sb_j_for_row(top));
            for (int y = top; y < rows; y++) sb_paint_row(y);
        }
    } else {
        if (sb_view <= 0) return 0;
        int old_show = show;
        sb_view--;
        int new_show = sb_view < (rows - 1) ? sb_view : (rows - 1);
        int top = rows - new_show;
        if (new_show < old_show) {
            /* Shrink: drop the oldest top row, keep the lower block. */
            for (int x = 0; x < cols; x++) {
                cell_blank(&cells[top - 1][x]);
                painted_valid[top - 1][x] = 0;
            }
            sb_paint_row(top - 1);
        } else {
            /* Full block: shift up, refresh the bottom row from its archive. */
            for (int y = top; y < rows - 1; y++) sb_move_row(y, y + 1);
            sb_row_to_cell(rows - 1, sb_j_for_row(rows - 1));
            for (int y = top; y < rows; y++) sb_paint_row(y);
        }
    }
    return 1;
}

int con_scrollback(int delta) {
    int want = sb_view + delta;
    if (want < 0) want = 0;
    if (want > sb_count) want = sb_count;
    if (want == sb_view) return sb_view;

    if (sb_view == 0 && want > 0)
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) sb_live[y][x] = cells[y][x];

    if (want == 0) {
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < cols; x++) cells[y][x] = sb_live[y][x];
        sb_view = 0;
        sb_repaint();
        return 0;
    }

    if (sb_view == 0) {
        int show = want < (rows - 1) ? want : (rows - 1);
        sb_view = want;
        sb_enter_history(show);
    }

    int dir = want > sb_view ? 1 : -1;
    while (sb_view != want) {
        if (!sb_step(dir)) break;
    }
    sync_cursor(1);
    return sb_view;
}

int  con_scrollback_view(void) { return sb_view; }
void con_scrollback_reset(void) {
    if (sb_view) con_scrollback(-sb_view);
}

/* ---- v2.25: input-line anchoring + adjustable glyph scale ---------------- */

/* Scroll (with archive) until the cursor row is the middle row.  The
 * shell calls this before printing each prompt: output pushes the input
 * line down, and the next prompt re-centres it -- like a modern full-
 * screen terminal keeping the active line mid-viewport. */
void con_scroll_anchor(void) {
    if (!be) return;
    int mid = rows / 2;
    int guard = rows;               /* scroll_one() always decrements cur_y */
    while (cur_y > mid && guard-- > 0) scroll_one();
    sync_cursor(1);
}

static int vbe24_scale(void) {
    /* Mirrors be_vbe24's current glyph scale via its cell height. */
    if (be_vbe24.cell_h == 8)  return 0;
    if (be_vbe24.cell_h == 32) return 2;
    return 1;
}

int con_get_cell_scale(void) {
    return vbe24_scale();
}

int con_set_cell_size(int s) {
    if (!be) return CON_ENOSYS;
    if (be != &be_vbe24 || !be->set_scale) return CON_ENOSYS;
    if (s < 0 || s > 2) return CON_EINVAL;
    if (vbe24_scale() == s && cols == be->cols && rows == be->rows)
        return 0;
    int oc = cols, orow = rows;
    if (be->set_scale(s) != 0) return CON_ENODEV;
    reflow(oc, orow, be->cols, be->rows);
    cols = be->cols;
    rows = be->rows;
    saved_row_valid = 0;
    saved2_valid = 0;
    dcx = dcy = -1;
    sb_view = 0;
    redraw_all();
    if (cur_x > cols) cur_x = cols;
    if (cur_y >= rows) cur_y = rows - 1;
    sync_cursor(1);
    return 0;
}

int con_screen_push(void) {
    if (ui_backup_valid) return -1;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) ui_backup[y][x] = cells[y][x];
    ui_backup_cur_x = cur_x; ui_backup_cur_y = cur_y;
    ui_backup_valid = 1;
    return 0;
}

int con_screen_pop(void) {
    if (!ui_backup_valid) return -1;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) cells[y][x] = ui_backup[y][x];
    cur_x = ui_backup_cur_x; cur_y = ui_backup_cur_y;
    if (cur_x >= cols) cur_x = cols - 1;
    if (cur_y >= rows) cur_y = rows - 1;
    ui_backup_valid = 0;
    saved_row_valid = 0; saved_row_index = -1;
    saved2_valid = 0;
    dcx = dcy = -1;
    redraw_all();
    sync_cursor(1);
    return 0;
}

int con_get_mode(void) { return be->mode; }
int con_cols(void)     { return cols; }
int con_rows(void)     { return rows; }
const char *con_backend_name(void) { return be->name; }
const char *con_mode_name(int m) {
    if (m == CON_MODE_TEXT) return "text";
    if (m == CON_MODE_GFX)  return "graphics (8bpp indexed)";
    if (m == CON_MODE_VBE)  return "vbe (32bpp truecolour)";
    if (m == CON_MODE_VBE24) return "vbe24 (24bpp truecolour, drawable)";
    return "unknown";
}

int con_info(int what) {
    switch (what) {
    case CON_INFO_MODE:        return be->mode;
    case CON_INFO_COLS:        return cols;
    case CON_INFO_ROWS:        return rows;
    case CON_INFO_COLORS:      return (int)be->colors;
    case CON_INFO_ATTRS:       return be->attr_mask;
    case CON_INFO_PIXEL_W:     return be->pixel_w;
    case CON_INFO_PIXEL_H:     return be->pixel_h;
    case CON_INFO_BPP:         return be->bpp;
    case CON_INFO_CJK:         return be->cjk;
    case CON_INFO_HZ_COUNT:    return (int)render_font_hz_count();
    case CON_INFO_CELL_W:      return be->cell_w ? (int)be->cell_w : CON_CELL_W;
    case CON_INFO_CELL_H:      return be->cell_h ? (int)be->cell_h : CON_CELL_H;
    case CON_INFO_BLINK:       return 1;
    case CON_INFO_SCROLL_FAST: return be->scroll ? 1 : 0;
    case CON_INFO_WC:          return be->wc_mode;
    case CON_INFO_HW_SCROLL:   return be->hw_scroll;
    case CON_INFO_CELLS_DRAWN: return (int)(stat_drawn & 0x7FFFFFFFu);
    case CON_INFO_CELLS_SKIPPED: return (int)(stat_skipped & 0x7FFFFFFFu);
    case CON_INFO_CELL_SCALE:  return vbe24_scale();
    default:                   return CON_ENOSYS;
    }
}

int con_request_mode(int mode, int origin) {
    (void)origin;                       /* consent is mandatory for all */
    if (mode != CON_MODE_TEXT && mode != CON_MODE_GFX &&
        mode != CON_MODE_VBE && mode != CON_MODE_VBE24) return CON_EINVAL;
    if (mode == be->mode) return 0;
    if (mode == CON_MODE_TEXT) {
        if (!con_ask_consent(origin)) return CON_EACCES;
        return switch_backend(&be_text);
    }
    if (mode == CON_MODE_GFX)   return switch_backend(&be_gfx);
    if (mode == CON_MODE_VBE24) return switch_backend(&be_vbe24);
    return switch_backend(&be_vbe);
}

int con_force_text(void) {
    if (be->mode != CON_MODE_TEXT) switch_backend(&be_text);
    return 0;
}

void con_diag(void) { if (be->diag) be->diag(); }

/* ---- v2.57: mouse glue (selection / wheel / paste) ---------------------- */

static void sel_box(int *x0, int *y0, int *x1, int *y1) {
    *x0 = sel_ax < sel_cx ? sel_ax : sel_cx;
    *x1 = sel_ax < sel_cx ? sel_cx : sel_ax;
    *y0 = sel_ay < sel_cy ? sel_ay : sel_cy;
    *y1 = sel_ay < sel_cy ? sel_cy : sel_cy;
}

static int sel_at(int x, int y) {
    if (!sel_valid) return 0;
    int x0, y0, x1, y1;
    sel_box(&x0, &y0, &x1, &y1);
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static void sel_repaint_box(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (x >= 0 && x < cols && y >= 0 && y < rows) {
                /* Force the dirty tracker to drop its cached appearance: the
                 * selection inverse swaps fg/bg at draw time, so a plain
                 * cell-content comparison would wrongly skip the repaint. */
                painted_valid[y][x] = 0;
                paint(x, y);
            }
}

static void sel_clear(void) {
    if (!sel_valid) return;
    int x0, y0, x1, y1;
    sel_box(&x0, &y0, &x1, &y1);
    sel_valid = 0; sel_dragging = 0;
    sel_repaint_box(x0, y0, x1, y1);
}

static void cell_of_pixel(int px, int py, int *cx, int *cy) {
    int cw = be->cell_w ? (int)be->cell_w : 8;
    int ch = be->cell_h ? (int)be->cell_h : 16;
    int x = px / cw, y = py / ch;
    if (x < 0) x = 0;
    if (x >= cols) x = cols - 1;
    if (y < 0) y = 0;
    int ymax = rows - 3;                 /* bottom two rows: IME bar */
    if (ymax < 0) ymax = 0;
    if (y > ymax) y = ymax;
    *cx = x; *cy = y;
}

static int utf8_encode_cp(uint32_t cp, uint8_t *out) {
    if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F)); return 4;
}

static void sel_serialize(void) {
    if (!sel_valid) return;
    int x0, y0, x1, y1;
    sel_box(&x0, &y0, &x1, &y1);
    uint8_t buf[CLIP_CAP];
    int n = 0;
    for (int y = y0; y <= y1; y++) {
        if (y > y0 && n < CLIP_CAP - 1) buf[n++] = '\n';
        int col0 = (y == y0) ? x0 : 0;
        int col1 = (y == y1) ? x1 : cols - 1;
        uint8_t rowtext[CON_MAX_COLS * 4];
        int rn = 0;
        for (int x = col0; x <= col1; x++) {
            con_cell_t *c = &cells[y][x];
            if (c->flags == CELL_F_WIDE_R) continue;
            uint32_t cp = c->cp;
            if (cp == 0) { rowtext[rn++] = ' '; continue; }
            uint8_t tmp[4];
            int k = utf8_encode_cp(cp, tmp);
            for (int i = 0; i < k && rn < (int)sizeof(rowtext); i++)
                rowtext[rn++] = tmp[i];
        }
        while (rn > 0 && rowtext[rn - 1] == ' ') rn--;
        for (int i = 0; i < rn && n < CLIP_CAP - 1; i++) buf[n++] = rowtext[i];
    }
    clip_set(buf, n);
}

void con_mouse_poll(void) {
    mouse_event_t e;
    while (mouse_poll_event(&e)) {
        /* Software pixel cursor: only the drawable gfx24 back-end hooks
         * this; text mode draws no mouse cursor (wheel still scrolls). */
        if (be->mouse_cursor) be->mouse_cursor(e.x, e.y, 1);

        /* Button edges first, so a wheel packet that also carries a button
         * change is not swallowed by the wheel branch. */
        if (e.changed & MOUSE_BTN_MID && (e.down & MOUSE_BTN_MID)) {
            if (paste_hook && !clip_empty()) paste_hook(clip_get(), clip_len());
            else                              paste_pending = 1;
        }
        if (e.changed & MOUSE_BTN_RIGHT && (e.down & MOUSE_BTN_RIGHT)) {
            sel_clear();
            continue;
        }
        if (e.changed & MOUSE_BTN_LEFT) {
            int cx, cy;
            cell_of_pixel(e.x, e.y, &cx, &cy);
            if (e.down & MOUSE_BTN_LEFT) {
                int ox0, oy0, ox1, oy1;
                if (sel_valid) sel_box(&ox0, &oy0, &ox1, &oy1);
                else { ox0 = oy0 = ox1 = oy1 = -1; }
                sel_ax = sel_cx = cx;
                sel_ay = sel_cy = cy;
                sel_valid = 1; sel_dragging = 1;
                if (ox0 >= 0) sel_repaint_box(ox0, oy0, ox1, oy1);
                sel_repaint_box(cx, cy, cx, cy);
            } else {
                sel_dragging = 0;
                if (sel_valid) sel_serialize();
            }
            continue;
        }

        if (e.dz) {
            /* 3 lines per notch; delta > 0 = older history. */
            con_scrollback(e.dz > 0 ? 3 : -3);
            continue;
        }

        if (sel_dragging) {
            int cx, cy;
            cell_of_pixel(e.x, e.y, &cx, &cy);
            if (cx != sel_cx || cy != sel_cy) {
                int ox0, oy0, ox1, oy1;
                sel_box(&ox0, &oy0, &ox1, &oy1);
                sel_cx = cx; sel_cy = cy;
                sel_repaint_box(ox0, oy0, ox1, oy1);
                sel_repaint_box(cx, cy, cx, cy);
            }
        }
    }
}

int con_mouse_paste_taken(void) {
    int v = paste_pending;
    paste_pending = 0;
    return v;
}

void con_set_paste_hook(con_paste_fn fn) {
    paste_hook = fn;
    if (!fn) { paste_pending = 0; return; }
    if (paste_pending && !clip_empty()) {
        paste_pending = 0;
        paste_hook(clip_get(), clip_len());
    }
}

void con_mouse_state(int *x, int *y, uint8_t *buttons) {
    mouse_get_state(x, y, buttons);
}

int con_selection_nonempty(void) { return sel_valid && !clip_empty(); }
