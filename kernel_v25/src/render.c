/*
 * render.c -- CatOS display FRONT-END (v2.8).
 *
 * Owns ALL console state: logical cell grid, cursor, current style,
 * 256-entry colour look-up table, UTF-8 decoder, display-mode policy and
 * the user-consent gate.  Back-ends are stateless painters.
 *
 * New in v2.8:
 *   - 32-bit RGB colours + a user-definable 256-entry CLUT.  8bpp
 *     back-ends use the quantised index; the 32bpp VBE back-end uses RGB.
 *   - UTF-8 decoding; GB2312 hanzi occupy a 2x2 logical block, kept
 *     atomic across scrolling, re-wrapping and mode switches.
 *   - A third back-end (VBE 800x600x32 truecolour).
 */
#include "render.h"
#include "render_be.h"
#include "keyboard.h"
#include "task.h"

#define CON_MAX_COLS 100        /* 800 / 8, the widest (VBE) terminal */
#define CON_MAX_ROWS 75         /* 600 / 8                            */

static con_cell_t cells[CON_MAX_ROWS][CON_MAX_COLS];
static con_cell_t tmp_cells[CON_MAX_ROWS][CON_MAX_COLS];
static render_be_t *be;
static int cols, rows, cur_x, cur_y;

/* current style */
static uint32_t s_fg_rgb, s_bg_rgb;
static uint8_t  s_fg_idx, s_bg_idx, s_attrs;

/* 256-entry colour look-up table (owned here, read by the back-ends) */
static uint32_t clut[256];

/* UTF-8 incremental decoder */
static uint32_t u_cp;
static int      u_need;

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

/* VGA's 16-colour index is BGR + intensity, not RGB. */
uint8_t render_rgb_to_vga(uint32_t rgb) {
    int r = CON_RGB_R(rgb), g = CON_RGB_G(rgb), b = CON_RGB_B(rgb);
    uint8_t i = (uint8_t)((b >= 128 ? 1 : 0) | (g >= 128 ? 2 : 0) |
                          (r >= 128 ? 4 : 0));
    if (r > 192 || g > 192 || b > 192) i |= 8;
    return i;
}

static void clut_init(void) {
    static const uint32_t vga16[16] = {
        0x000000,0x0000AA,0x00AA00,0x00AAAA,
        0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
        0x555555,0x5555FF,0x55FF55,0x55FFFF,
        0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
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
}
static void cell_set(con_cell_t *c, uint32_t cp, uint8_t flags) {
    c->cp = cp;
    c->fg = s_fg_rgb;   c->bg = s_bg_rgb;
    c->fg_idx = s_fg_idx; c->bg_idx = s_bg_idx;
    c->attrs = s_attrs; c->flags = flags;
}

static void paint(int x, int y) {
    const con_cell_t *c = &cells[y][x];
    if (c->flags == CELL_F_CONT) return;      /* painted by its WIDE_TL */
    be->draw_cell(x, y, c);
}

static void redraw_all(void) {
    be->clear(s_bg_rgb);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) paint(x, y);
}

/* CJK glyphs always start on an even row and occupy rows y,y+1.  That
 * invariant lets us scroll two rows at a time and guarantees no 2x2
 * block is ever cut in half. */
static void scroll_two(void) {
    int n = rows >= 2 ? 2 : 1;
    for (int y = n; y < rows; y++)
        for (int x = 0; x < cols; x++) cells[y - n][x] = cells[y][x];
    for (int y = rows - n; y < rows; y++)
        for (int x = 0; x < cols; x++) cell_blank(&cells[y][x]);
    cur_y -= n; if (cur_y < 0) cur_y = 0;
    redraw_all();
}

/* Overwriting one cell of a 2x2 CJK block must erase the whole block,
 * otherwise its remaining continuation cells render as garbage. */
static void break_block(int x, int y) {
    int bx = -1, by = -1;
    for (int dy = 0; dy <= 1 && bx < 0; dy++)
        for (int dx = 0; dx <= 1 && bx < 0; dx++) {
            int tx = x - dx, ty = y - dy;
            if (tx >= 0 && ty >= 0 && tx < cols && ty < rows &&
                cells[ty][tx].flags == CELL_F_WIDE_TL) { bx = tx; by = ty; }
        }
    if (bx < 0) return;
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int tx = bx + dx, ty = by + dy;
            if (tx < cols && ty < rows) {
                cell_blank(&cells[ty][tx]);
                paint(tx, ty);
            }
        }
}

static void ensure_room(int wide) {
    int w = wide ? 2 : 1;
    if (wide && (cur_y & 1)) { cur_x = 0; cur_y++; }
    if (cur_x + w > cols) {
        cur_x = 0; cur_y += w;
        if (wide && (cur_y & 1)) cur_y++;
    }
    while (cur_y + w > rows) scroll_two();
}

static void cell_put_cp(uint32_t cp) {
    switch (cp) {
    case '\n': cur_x = 0; cur_y++; while (cur_y >= rows) scroll_two(); break;
    case '\r': cur_x = 0; break;
    case '\b':
        if (cur_x > 0) {
            cur_x--;
            if (cur_y >= 0 && cur_y < rows && cur_x < cols &&
                cells[cur_y][cur_x].flags == CELL_F_CONT && cur_x > 0)
                cur_x--;
            break_block(cur_x, cur_y);
            cell_blank(&cells[cur_y][cur_x]);
            paint(cur_x, cur_y);
        }
        break;
    case '\t': {
        int nx = (cur_x + 4) & ~3;
        while (cur_x < nx) cell_put_cp(' ');
        break;
    }
    default: {
        int wide = 0;
        if (cp >= 0x1100) {
            if (be->cjk && render_font_hz(cp)) wide = 1;
            else cp = '?';                 /* text mode / uncovered glyph */
        }
        if (cp > 0xFF) cp = '?';
        ensure_room(wide);
        if (wide) {
            cell_set(&cells[cur_y][cur_x], cp, CELL_F_WIDE_TL);
            paint(cur_x, cur_y);
            cell_set(&cells[cur_y][cur_x + 1], 0, CELL_F_CONT);
            cell_set(&cells[cur_y + 1][cur_x], 0, CELL_F_CONT);
            cell_set(&cells[cur_y + 1][cur_x + 1], 0, CELL_F_CONT);
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
    while (cur_y >= rows) scroll_two();
    be->set_cursor(cur_x, cur_y);
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
    if (--u_need == 0) { *out = u_cp; return 1; }
    return 0;
}

void con_flush_utf8(void) {
    if (u_need) { u_need = 0; cell_put_cp('?'); }
}

/* ---- reflow: re-wrap the grid, keeping 2x2 CJK blocks atomic ------------ */
static void tmp_shift(int n) {
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
            if (c->flags == CELL_F_CONT) continue;
            int w = (c->flags == CELL_F_WIDE_TL) ? 2 : 1;
            if (w == 2 && (ny & 1)) ny++;
            if (nx + w > nc) {
                nx = 0; ny += w;
                if (w == 2 && (ny & 1)) ny++;
            }
            while (ny + w > nrow) {
                tmp_shift(2);
                ny -= 2; if (ny < 0) ny = 0;
            }
            for (int dy = 0; dy < w; dy++)
                for (int dx = 0; dx < w; dx++)
                    if (y + dy < orow && x + dx < oc &&
                        ny + dy < nrow && nx + dx < nc)
                        tmp_cells[ny + dy][nx + dx] = cells[y + dy][x + dx];
            nx += w;
        }
        nx = 0; ny++;
    }
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cells[y][x] = tmp_cells[y][x];
}

static int switch_backend(render_be_t *nb) {
    render_be_t *old = be;
    int oc = cols, orow = rows;
    /* Shut the old hardware down FIRST.  VBE's dispi disable must precede
     * a Mode 13h / text-mode register sequence, not follow it. */
    old->leave();
    if (nb->enter() != 0) {          /* e.g. no VBE device */
        old->enter();                /* restore the old mode */
        return CON_ENODEV;
    }
    reflow(oc, orow, nb->cols, nb->rows);
    be = nb;
    cols = be->cols;
    rows = be->rows;
    redraw_all();
    if (cur_x > cols) cur_x = cols;
    if (cur_y >= rows) cur_y = rows - 1;
    be->set_cursor(cur_x, cur_y);
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
    s_fg_idx = CON_LIGHT_GREY; s_bg_idx = CON_BLACK; s_attrs = 0;
    s_fg_rgb = clut[s_fg_idx]; s_bg_rgb = clut[s_bg_idx];
    /* Do NOT call be->enter() at boot: GRUB already set text mode 03. */
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++) cell_blank(&cells[y][x]);
    be->clear(s_bg_rgb);
    be->set_cursor(0, 0);
}

void con_clear(void) {
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++) cell_blank(&cells[y][x]);
    cur_x = cur_y = 0; u_need = 0;
    redraw_all();
    be->set_cursor(0, 0);
}

void con_putc(char c) { cell_put_cp((unsigned char)c); }

void con_write(const char *s) {
    if (!s) return;
    while (*s) {
        uint32_t cp;
        if (utf8_feed((unsigned char)*s++, &cp)) cell_put_cp(cp);
    }
    con_flush_utf8();   /* syscall/write boundary: never leak decoder state */
}
void con_write_utf8(const char *s) { con_write(s); }

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

void con_move_cursor(void) { be->set_cursor(cur_x, cur_y); }

void con_set_cursor(uint8_t x, uint8_t y) {
    if ((int)x >= cols) x = (uint8_t)(cols - 1);
    if ((int)y >= rows) y = (uint8_t)(rows - 1);
    cur_x = x; cur_y = y;
    be->set_cursor(cur_x, cur_y);
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

int con_get_mode(void) { return be->mode; }
int con_cols(void)     { return cols; }
int con_rows(void)     { return rows; }
const char *con_backend_name(void) { return be->name; }
const char *con_mode_name(int m) {
    if (m == CON_MODE_TEXT) return "text";
    if (m == CON_MODE_GFX)  return "graphics (8bpp indexed)";
    if (m == CON_MODE_VBE)  return "vbe (32bpp truecolour)";
    return "unknown";
}

int con_info(int what) {
    switch (what) {
    case CON_INFO_MODE:     return be->mode;
    case CON_INFO_COLS:     return cols;
    case CON_INFO_ROWS:     return rows;
    case CON_INFO_COLORS:   return (int)be->colors;
    case CON_INFO_ATTRS:    return be->attr_mask;
    case CON_INFO_PIXEL_W:  return be->pixel_w;
    case CON_INFO_PIXEL_H:  return be->pixel_h;
    case CON_INFO_BPP:      return be->bpp;
    case CON_INFO_CJK:      return be->cjk;
    case CON_INFO_HZ_COUNT: return (int)render_font_hz_count();
    default:                return CON_ENOSYS;
    }
}

int con_request_mode(int mode, int origin) {
    (void)origin;                       /* consent is mandatory for all */
    if (mode != CON_MODE_TEXT && mode != CON_MODE_GFX &&
        mode != CON_MODE_VBE) return CON_EINVAL;
    if (mode == be->mode) return 0;
    if (mode == CON_MODE_TEXT) {
        if (!con_ask_consent(origin)) return CON_EACCES;
        return switch_backend(&be_text);
    }
    if (mode == CON_MODE_GFX) return switch_backend(&be_gfx);
    return switch_backend(&be_vbe);
}

int con_force_text(void) {
    if (be->mode != CON_MODE_TEXT) switch_backend(&be_text);
    return 0;
}

void con_diag(void) { if (be->diag) be->diag(); }
