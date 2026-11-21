/*
 * tui.h -- CatOS Terminal User Interface toolkit (v2.67 rewrite).
 *
 * Lessons borrowed from ratatui / bubbletea / btop:
 *   - widgets draw into the terminal CELL GRID, never straight into
 *     pixels, so the front-end dirty tracker stays authoritative (no
 *     ghosts, no full-screen flash);
 *   - only single-cell ASCII glyphs are used, so a widget cannot depend on
 *     the dynamic CJK font-slot table;
 *   - colour comes from the registry palette (palette.c), so a theme change
 *     restyles every TUI screen without touching widget code.
 */
#ifndef CATOS_TUI_H
#define CATOS_TUI_H

#include <stdint.h>

/* Empty colour role = "take it from the palette". */
#define TUI_C_AUTO   0u

/* ---- primitives -------------------------------------------------------- */
void tui_fill(int gx, int gy, int w, int h, uint32_t bg_rgb);
void tui_box(int gx, int gy, int w, int h, uint32_t fg_rgb, const char *title);
void tui_panel(int gx, int gy, int w, int h, uint32_t fg_rgb, uint32_t bg_rgb,
               const char *title);
void tui_label(int gx, int gy, uint32_t fg_rgb, const char *text);
void tui_label_bg(int gx, int gy, uint32_t fg_rgb, uint32_t bg_rgb, const char *text);
void tui_hline_sep(int gx, int gy, int w, uint32_t fg_rgb);

/* ---- widgets ---------------------------------------------------------- */
/* Ratio bar + percentage: the btop/gitui "gauge". */
void tui_gauge(int gx, int gy, int w, int pct, uint32_t fg_rgb,
               uint32_t bg_rgb, const char *label);

/* key/value row, value right-aligned inside `w`. */
void tui_kv(int gx, int gy, int w, uint32_t kfg, uint32_t vfg,
            const char *key, const char *val);

/* tab strip; the selected tab is inverted. */
void tui_tabs(int gx, int gy, int w, const char *const *names, int count,
              int selected, uint32_t accent, uint32_t dim);

/* section header: " title " with a rule to the right. */
void tui_section(int gx, int gy, int w, uint32_t fg, const char *title);

/* footer key hints. */
void tui_hintbar(int gy, int w, uint32_t fg, uint32_t bg, const char *hints);

/* table row: fixed column widths, last column takes the remainder.
 * vals[] may contain NULL for an empty cell. */
void tui_row(int gx, int gy, int w, const int *colw, int ncol,
             uint32_t fg, uint32_t bg, const char *const *vals, int header);

/* horizontal sparkline built from 0..100 samples. */
void tui_spark(int gx, int gy, int w, const uint8_t *v, int n,
               uint32_t fg, uint32_t bg);

#endif /* CATOS_TUI_H */
