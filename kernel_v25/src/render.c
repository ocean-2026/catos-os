/*
 * render.c -- CatOS display FRONT-END.
 *
 * Owns ALL console state: a logical cell grid, the cursor, the current
 * style, and the display-mode policy.  Back-ends (be_text.c / be_gfx.c)
 * are stateless painters reached only through render_be_t.
 *
 * Consequences:
 *   - shell / fs / net / exceptions / CE syscalls never see a VGA register
 *     or a framebuffer address, in either mode;
 *   - switching modes does not lose the screen: the logical grid is
 *     re-wrapped to the new column count and replayed to the new painter;
 *   - GFX -> TEXT is gated on a physical 'Y' keypress.  No program can
 *     supply that consent (there is no parameter or syscall for it).
 */
#include "render.h"
#include "render_be.h"
#include "keyboard.h"
#include "task.h"

#define CON_MAX_COLS 80
#define CON_MAX_ROWS 25

#define CELL(ch,fg,bg,at) ((uint32_t)(unsigned char)(ch) | \
                           ((uint32_t)(uint8_t)(fg) << 8)  | \
                           ((uint32_t)(uint8_t)(bg) << 16) | \
                           ((uint32_t)(uint8_t)(at) << 24))
#define CELL_CH(c)  ((unsigned char)((c) & 0xFFu))
#define CELL_FG(c)  ((uint8_t)(((c) >> 8)  & 0xFFu))
#define CELL_BG(c)  ((uint8_t)(((c) >> 16) & 0xFFu))
#define CELL_AT(c)  ((uint8_t)(((c) >> 24) & 0xFFu))

static render_be_t *be;
static uint32_t cells[CON_MAX_ROWS][CON_MAX_COLS];
static uint32_t tmp_cells[CON_MAX_ROWS][CON_MAX_COLS];   /* reflow scratch */
static int     cols, rows;
static int     cur_x, cur_y;
static uint8_t s_fg = CON_LIGHT_GREY, s_bg = CON_BLACK, s_attrs = 0;

static uint32_t blank_cell(void) { return CELL(0, s_fg, s_bg, s_attrs); }

static void redraw_all(void) {
    be->clear(s_bg);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            uint32_t c = cells[y][x];
            be->draw_cell(x, y, CELL_CH(c),
                          CELL_FG(c), CELL_BG(c), CELL_AT(c));
        }
}

static void scroll_one(void) {
    for (int y = 1; y < rows; y++)
        for (int x = 0; x < cols; x++) cells[y - 1][x] = cells[y][x];
    for (int x = 0; x < cols; x++) cells[rows - 1][x] = blank_cell();
    cur_y = rows - 1;
    redraw_all();
}

static void shift_tmp_up(void) {
    for (int y = 1; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            tmp_cells[y - 1][x] = tmp_cells[y][x];
    for (int x = 0; x < CON_MAX_COLS; x++)
        tmp_cells[CON_MAX_ROWS - 1][x] = blank_cell();
}

/* Re-wrap the logical grid when the back-end's column count changes
 * (80x25 text <-> 40x25 graphics).  Trailing blank cells are dropped;
 * every original row ends with an explicit line break. */
static void reflow(int oc, int orow, int nc, int nrow) {
    if (oc > CON_MAX_COLS) oc = CON_MAX_COLS;
    if (orow > CON_MAX_ROWS) orow = CON_MAX_ROWS;
    if (nc > CON_MAX_COLS) nc = CON_MAX_COLS;
    if (nrow > CON_MAX_ROWS) nrow = CON_MAX_ROWS;
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            tmp_cells[y][x] = blank_cell();

    int nx = 0, ny = 0;
    for (int y = 0; y < orow; y++) {
        int last = -1;
        for (int x = 0; x < oc; x++)
            if (CELL_CH(cells[y][x])) last = x;
        for (int x = 0; x <= last; x++) {
            if (nx >= nc) { nx = 0; ny++; }
            if (ny >= nrow) { shift_tmp_up(); ny = nrow - 1; }
            tmp_cells[ny][nx++] = cells[y][x];
        }
        nx = 0; ny++;
        if (ny >= nrow) { shift_tmp_up(); ny = nrow - 1; }
    }
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cells[y][x] = tmp_cells[y][x];
}

static void switch_backend(render_be_t *nb) {
    int oc = cols, orow = rows;
    be->leave();
    reflow(oc, orow, nb->cols, nb->rows);
    be = nb;
    cols = be->cols;
    rows = be->rows;
    be->enter();                 /* program hardware BEFORE painting */
    redraw_all();
    if (cur_x > cols) cur_x = cols;
    if (cur_y >= rows) cur_y = rows - 1;
    be->set_cursor(cur_x, cur_y);
}

/* The consent gate.  Reads ONE real key from the physical PS/2 queue.
 * Only the human at the console can produce it.  Ring3 has no way to
 * inject a key, set a flag, or skip this function. */
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
        con_write("\n[display] consent denied - staying in graphics mode.\n");
        return 0;
    }
}

/* ---- public front-end API ---------------------------------------------- */

void render_init(void) {
    be = &be_text;
    cols = be->cols;
    rows = be->rows;
    cur_x = cur_y = 0;
    s_fg = CON_LIGHT_GREY; s_bg = CON_BLACK; s_attrs = 0;
    /* Do NOT call be->enter() at boot: GRUB already programmed mode 03 and
     * re-seeding plane 2 would replace the BIOS 8x16 glyph set.  enter()
     * is only needed when coming BACK from Mode 13h. */
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cells[y][x] = blank_cell();
    be->clear(s_bg);
    be->set_cursor(0, 0);
}

void con_clear(void) {
    for (int y = 0; y < CON_MAX_ROWS; y++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cells[y][x] = blank_cell();
    cur_x = cur_y = 0;
    redraw_all();
    be->set_cursor(0, 0);
}

void con_putc(char c) {
    switch (c) {
    case '\n':
        cur_x = 0; cur_y++;
        break;
    case '\r':
        cur_x = 0;
        break;
    case '\b':
        if (cur_x > 0) cur_x--;
        cells[cur_y][cur_x] = blank_cell();
        be->draw_cell(cur_x, cur_y, 0, s_fg, s_bg, s_attrs);
        break;
    case '\t':
        cur_x = (cur_x + 4) & ~3;
        break;
    default:
        if (cur_x >= cols) { cur_x = 0; cur_y++; }
        if (cur_y >= rows) scroll_one();
        cells[cur_y][cur_x] = CELL((unsigned char)c, s_fg, s_bg, s_attrs);
        be->draw_cell(cur_x, cur_y, (unsigned char)c, s_fg, s_bg, s_attrs);
        cur_x++;
        break;
    }
    if (cur_x >= cols) { cur_x = 0; cur_y++; }
    if (cur_y >= rows) scroll_one();
    be->set_cursor(cur_x, cur_y);
}

void con_write(const char *s) {
    if (!s) return;
    while (*s) con_putc(*s++);
}

void con_set_style(uint8_t fg, uint8_t bg, uint8_t attrs) {
    /* Store the full intent, not the back-end's mask: switching back-ends
     * should not silently throw away styles the new back-end can draw. */
    s_fg = fg; s_bg = bg; s_attrs = attrs;
}
void con_set_fg(uint8_t fg)     { s_fg = fg; }
void con_set_attrs(uint8_t at)  { s_attrs = at; }
uint8_t con_get_fg(void)        { return s_fg; }
uint8_t con_get_attrs(void)     { return s_attrs; }
void con_move_cursor(void)      { be->set_cursor(cur_x, cur_y); }

void con_set_cursor(uint8_t x, uint8_t y) {
    if ((int)x >= cols) x = (uint8_t)(cols - 1);
    if ((int)y >= rows) y = (uint8_t)(rows - 1);
    cur_x = x; cur_y = y;
    be->set_cursor(cur_x, cur_y);
}

void con_putc_at(uint8_t x, uint8_t y, char c) {
    if ((int)x >= cols || (int)y >= rows) return;
    cells[y][x] = CELL((unsigned char)c, s_fg, s_bg, s_attrs);
    be->draw_cell(x, y, (unsigned char)c, s_fg, s_bg, s_attrs);
}

void con_write_at(uint8_t x, uint8_t y, const char *s) {
    if (!s) return;
    while (*s && (int)x < cols) con_putc_at(x++, y, *s++);
}

int con_get_mode(void)        { return be->mode; }
int con_cols(void)            { return cols; }
int con_rows(void)            { return rows; }
const char *con_backend_name(void) { return be->name; }

int con_info(int what) {
    switch (what) {
    case CON_INFO_MODE:     return be->mode;
    case CON_INFO_COLS:     return cols;
    case CON_INFO_ROWS:     return rows;
    case CON_INFO_COLORS:   return be->colors;
    case CON_INFO_ATTRS:    return be->attr_mask;
    case CON_INFO_PIXEL_W:  return be->pixel_w;
    case CON_INFO_PIXEL_H:  return be->pixel_h;
    default:                return CON_ENOSYS;
    }
}

int con_request_mode(int mode, int origin) {
    (void)origin;               /* consent is mandatory for ALL origins */
    if (mode != CON_MODE_TEXT && mode != CON_MODE_GFX) return CON_EINVAL;
    if (mode == be->mode) return 0;

    /* TEXT -> GFX: open to everyone, including ring3 CE programs. */
    if (mode == CON_MODE_GFX) { switch_backend(&be_gfx); return 0; }

    /* GFX -> TEXT: the only way in is a physical human 'Y'. */
    if (!con_ask_consent(origin)) return CON_EACCES;
    switch_backend(&be_text);
    return 0;
}

int con_force_text(void) {
    if (be->mode == CON_MODE_GFX) switch_backend(&be_text);
    return 0;
}

void con_diag(void) {
    if (be->diag) be->diag();
}
