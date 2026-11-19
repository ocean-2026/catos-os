/*
 * tui.c -- CatOS Terminal User Interface primitives.
 *
 * Works across all display back-ends by choosing the drawing strategy
 * based on the current mode:
 *   - text / VBE32 : cell-level via con_write_at_utf8() (UTF-8 box chars)
 *   - VBE24        : pixel-level via gfx24_rect() / gfx24_pixel()
 *   - Mode 13h     : cell-level via con_write_* (no dedicated pixel API)
 *
 * The box-drawing Unicode block characters (U+2500..U+257F) are treated
 * as wide glyphs: each ─ / │ / ┌ ┘ etc. occupies one full cell width
 * because they're rendered as single byte-glyphs in the VGA font plane
 * (the right-half trick from hz_font). In practice they appear cleanly
 * because the font module already handles them like CJK.
 */
#include <stdint.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

#include "render.h"
#include "render_be.h"
#include "gfx24.h"
#include "tui.h"

/* ---- internal helpers -------------------------------------------------- */

/* Kernel-safe string length -- no libc dependency. */
static int k_strlen(const char *s) {
    int n = 0; while (s && s[n]) n++; return n;
}

/* Clip gx/gw to [0, cols).  Returns 0 if entirely out of bounds. */
static int clip_x(int *gx, int *gw) {
    int c = con_cols();
    if (*gx >= c || *gx + *gw <= 0) return 0;
    if (*gx < 0)          { *gw += *gx; *gx = 0; }
    if (*gx + *gw > c)    *gw = c - *gx;
    return 1;
}

/* Same for gy/gh against rows. */
static int clip_y(int *gy, int *gh) {
    int r = con_rows();
    if (*gy >= r || *gy + *gh <= 0) return 0;
    if (*gy < 0)          { *gh += *gy; *gy = 0; }
    if (*gy + *gh > r)    *gh = r - *gy;
    return 1;
}

/* Check whether the active back-end can use pixel drawing. */
static int gfx_active(void) {
    return gfx24_active() == 1;
}

/* ---- horizontal line --------------------------------------------------- */

static void do_hline_gfx(int x, int y, int len, uint32_t col) {
    gfx24_rect(x, y, len, 1, col);
}

static void do_hline_txt(int gx, int gy, int len, const char *ch) {
    for (int i = 0; i < len; i++)
        con_write_at_utf8((uint8_t)(gx + i), (uint8_t)gy, ch);
}

static void hline(int gx, int gy, int len, uint32_t col,
                  const char *ch) {
    if (len <= 0) return;
    int x = gx, w = len;
    if (!clip_x(&x, &w)) return;
    if (gy < 0 || gy >= con_rows()) return;
    if (gfx_active()) {
        int px = x * 8;              /* approx; exact = x * be->cell_w */
        int py = gy * 16;
        do_hline_gfx(px, py, w * 8, col);
    } else {
        do_hline_txt(x, gy, w, ch);
    }
}

/* ---- background fill (interior of a rectangle) ------------------------- */

/* ---- tui_box ------------------------------------------------------------ */

void tui_box(int gx, int gy, int w, int h, uint32_t fg_rgb,
             const char *title) {
    if (w < 2 || h < 2) return;

    int ow = w, oh = h;
    if (!clip_x(&gx, &w) || !clip_y(&gy, &h)) return;

    if (gfx_active()) {
        /* Pixel-perfect border with title bar inset */
        int px0 = gx * 8, py0 = gy * 16;
        /* Top edge */
        gfx24_rect(px0, py0, w * 8, 1, fg_rgb);
        /* Bottom edge */
        gfx24_rect(px0, py0 + (h - 1) * 16, w * 8, 1, fg_rgb);
        /* Left edge */
        gfx24_rect(px0, py0, 1, h * 16, fg_rgb);
        /* Right edge */
        gfx24_rect(px0 + (w - 1) * 8, py0, 1, h * 16, fg_rgb);
        /* Title bar (subtle highlight along top-inner row) */
        if (title && *title) {
            gfx24_rect(px0 + 8, py0 + 1, (int)k_strlen(title) * 8, 1, fg_rgb);
        }
    } else {
        /* Text-mode: Unicode box-drawing characters */
        if (h <= 0 || w <= 0) return;

        if (title && *title) {
            /* Top row with title */
            con_write_at_utf8((uint8_t)gx,           (uint8_t)gy,   TUI_UL);
            /* Horizontal dashes between corners */
            for (int c = gx + 1; c < gx + w - 1 && c < gx + (int)k_strlen(title) + 1; c++)
                con_write_at_utf8((uint8_t)c, (uint8_t)gy, TUI_H);
            /* Draw title text just after the dashes (overlap OK for short titles) */
            if ((int)k_strlen(title) + 2 < w) {
                int tx = gx + (w - 1 - (int)k_strlen(title));
                con_write_at((uint8_t)tx, (uint8_t)gy, title);
            }
            con_write_at_utf8((uint8_t)(gx + w - 1), (uint8_t)gy, TUI_UR);
        } else {
            /* Plain top border */
            con_write_at_utf8((uint8_t)gx,           (uint8_t)gy,   TUI_UL);
            for (int c = gx + 1; c < gx + w - 1; c++)
                con_write_at_utf8((uint8_t)c, (uint8_t)gy, TUI_H);
            con_write_at_utf8((uint8_t)(gx + w - 1), (uint8_t)gy, TUI_UR);
        }

        /* Middle rows */
        for (int r = 1; r < h - 1; r++) {
            con_write_at_utf8((uint8_t)gx,           (uint8_t)(gy + r), TUI_V);
            con_write_at_utf8((uint8_t)(gx + w - 1), (uint8_t)(gy + r), TUI_V);
        }

        /* Bottom row */
        con_write_at_utf8((uint8_t)gx,           (uint8_t)(gy + h - 1), TUI_BL);
        for (int c = gx + 1; c < gx + w - 1; c++)
            con_write_at_utf8((uint8_t)c, (uint8_t)(gy + h - 1), TUI_H);
        con_write_at_utf8((uint8_t)(gx + w - 1), (uint8_t)(gy + h - 1), TUI_BR);
    }

    (void)ow; (void)oh; /* suppress unused-variable warnings */
}

/* ---- tui_panel ---------------------------------------------------------- */

void tui_panel(int gx, int gy, int w, int h,
               uint32_t fg_rgb, uint32_t bg_rgb,
               const char *title) {
    if (w < 2 || h < 2) return;

    int ow = w, oh = h;
    if (!clip_x(&gx, &w) || !clip_y(&gy, &h)) return;

    if (gfx_active()) {
        /* Fill interior first */
        int px = gx * 8, py = gy * 16;
        gfx24_rect(px + 1, py + 1, w * 8 - 2, h * 16 - 2, bg_rgb);
        /* Border on top of fill */
        tui_box(gx, gy, ow, oh, fg_rgb, title);
    } else {
        /* Text mode: clear interior with background colour, then draw border */
        con_set_bg_rgb(bg_rgb);
        for (int r = 1; r < h - 1; r++) {
            for (int c = 1; c < w - 1; c++)
                con_write_at((uint8_t)(gx + c), (uint8_t)(gy + r), " ");
        }
        tui_box(gx, gy, ow, oh, fg_rgb, title);
    }
    (void)ow; (void)oh;
}

/* ---- tui_progress ------------------------------------------------------- */

void tui_progress(int gx, int gy, int w, int pct,
                  uint32_t fg_rgb, uint32_t bg_rgb,
                  const char *label) {
    if (w < 2) return;
    pct = (pct < 0) ? 0 : (pct > 100) ? 100 : pct;
    int fill_cells = (pct * (w - 2)) / 100;
    int empty_cells = (w - 2) - fill_cells;

    if (gfx_active()) {
        int px = gx * 8, py = gy * 16;
        /* Background track */
        gfx24_rect(px + 1, py + 2, (w - 2) * 8, 12, bg_rgb);
        /* Filled portion */
        if (fill_cells > 0)
            gfx24_rect(px + 1, py + 2, fill_cells * 8, 12, fg_rgb);
    } else {
        con_set_bg_rgb(bg_rgb);
        con_write_at((uint8_t)gx,         (uint8_t)gy, "[");
        for (int i = 0; i < fill_cells; i++)
            con_write_at((uint8_t)(gx + 1 + i), (uint8_t)gy, "█");
        for (int i = 0; i < empty_cells; i++)
            con_write_at((uint8_t)(gx + 1 + fill_cells + i), (uint8_t)gy, " ");
        con_write_at((uint8_t)(gx + w - 1), (uint8_t)gy, "]");
    }

    if (label && *label) {
        con_set_fg_rgb(fg_rgb);
        int lx = gx + w + 2;
        if (lx < con_cols())
            con_write_at((uint8_t)lx, (uint8_t)gy, label);
    }
}

/* ---- tui_button -------------------------------------------------------- */

void tui_button(int gx, int gy, int w, int h,
                uint32_t fg_rgb, const char *text) {
    if (w < 4 || h < 1) return;
    int tw = (int)k_strlen(text);
    int tx = gx + (w - tw) / 2;
    int ty = gy + (h - 1) / 2;

    if (tx < 0) tx = 0;
    if (tx + tw >= gx + w) tx = gx + w - tw - 1;

    tui_box(gx, gy, w, h, fg_rgb, NULL);
    if (ty >= 0 && ty < con_rows() && tx >= 0 && tx + tw < con_cols()) {
        con_set_fg_rgb(fg_rgb);
        con_write_at((uint8_t)tx, (uint8_t)ty, text);
    }
}

/* ---- tui_checkbox ------------------------------------------------------ */

void tui_checkbox(int gx, int gy, int checked,
                  uint32_t fg_rgb, const char *label) {
    if (gx < 0 || gx >= con_cols()) return;
    if (gy < 0 || gy >= con_rows()) return;
    con_set_fg_rgb(fg_rgb);
    con_write_at((uint8_t)gx,         (uint8_t)gy, "[");
    con_write_at((uint8_t)(gx + 1),   (uint8_t)gy, checked ? "X" : " ");
    con_write_at((uint8_t)(gx + 2),   (uint8_t)gy, "]");
    if (label && *label) {
        int lx = gx + 4;
        if (lx < con_cols())
            con_write_at((uint8_t)lx, (uint8_t)gy, label);
    }
}

/* ---- tui_label --------------------------------------------------------- */

void tui_label(int gx, int gy, uint32_t fg_rgb, const char *text) {
    if (gx < 0 || gy < 0) return;
    if (gx >= con_cols() || gy >= con_rows()) return;
    con_set_fg_rgb(fg_rgb);
    con_write_at((uint8_t)gx, (uint8_t)gy, text ?: "");
}

/* ---- tui_hline_sep ------------------------------------------------------ */

void tui_hline_sep(int gx, int gy, int w, uint32_t fg_rgb) {
    if (w <= 0 || gy < 0 || gy >= con_rows()) return;
    con_set_fg_rgb(fg_rgb);
    hline(gx, gy, w, fg_rgb, TUI_H);
}
