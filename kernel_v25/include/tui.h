/*
 * tui.h -- CatOS Terminal User Interface primitives.
 *
 * Cell-coordinate based widgets that work uniformly across all display
 * back-ends (text / Mode 13h / VBE32).  Uses con_write_utf8_at() when
 * the current back-end is text-mode oriented; falls back to gfx24_*
 * pixel drawing when in VBE24 graphics mode.
 *
 * Box-drawing glyphs: ┌ ┐ └ ┘ ├ ┤ ─ │ ┌─┐│└─┘
 * In pure-text back-ends these arrive as 3-byte UTF-8 sequences each;
 * VGA plane-2 font slots handle them the same way they handle CJK
 * glyphs (right-half glyph appended at cp & ~0x0F).
 */
#ifndef CATOS_TUI_H
#define CATOS_TUI_H

#include <stdint.h>

/* ---- colour palette --------------------------------------------------- */
#define TUI_CLR_BLACK   0x000000u
#define TUI_CLR_WHITE   0xFFFFFFu
#define TUI_CLR_RED     0xCC0000u
#define TUI_CLR_GREEN   0x00AA00u
#define TUI_CLR_BLUE    0x0066CCu
#define TUI_CLR_YELLOW  0xFFCC00u
#define TUI_CLR_CYAN    0x00CCCCu
#define TUI_CLR_MAGENTA 0xAA00AAu
#define TUI_CLR_GRAY    0xAAAAAAlu
#define TUI_CLR_DGRAY   0x555555ul
#define TUI_CLR_BG      0x1A1A2Eul  /* dark navy panel background */
#define TUI_CLR_FG      0xE0E0E0ul  /* off-white text */
#define TUI_CLR_ACCENT  0x4FC3F7ul  /* light cyan accent */

/* ---- box / frame characters ------------------------------------------- */
#define TUI_UL "┌"   /* top-left  corner */
#define TUI_UR "┐"   /* top-right corner */
#define TUI_BL "└"   /* bottom-left corner */
#define TUI_BR "┘"   /* bottom-right corner */
#define TUI_H  "─"   /* horizontal edge  */
#define TUI_V  "│"   /* vertical   edge  */
#define TUI_TL "┬"   /* T-junction up    */
#define TUI_TB "┴"   /* T-junction down  */
#define TUI_LT "├"   /* T-junction left  */
#define TUI_RT "┤"   /* T-junction right */
#define TUI_CR "╋"   /* cross            */

/* ---- widget types ----------------------------------------------------- */
enum {
    TUI_BOX,          /* bordered rectangle                          */
    TUI_PANEL,        /* bordered + filled background                */
    TUI_PROGRESS,     /* horizontal progress bar                     */
    TUI_BUTTON,       /* interactive button (label-only, no event)   */
    TUI_CHECKBOX,     /* checkbox label [x] / [ ]                    */
    TUI_LABEL,        /* plain label                                 */
    TUI_SEPARATOR,    /* thin separator line                         */
};

/* ---- public API ------------------------------------------------------- */
/* Draw a rectangular border at (gx,gy)..(gx+w-1,gy+h-1).
 * title: non-empty string → header row inside the top border.           */
void tui_box(int gx, int gy, int w, int h, uint32_t fg_rgb,
             const char *title);

/* Same as tui_box but fills the interior with bg_rgb.                 */
void tui_panel(int gx, int gy, int w, int h,
               uint32_t fg_rgb, uint32_t bg_rgb,
               const char *title);

/* Horizontal progress bar from pct (0..100). Label drawn above if len>0.*/
void tui_progress(int gx, int gy, int w, int pct,
                  uint32_t fg_rgb, uint32_t bg_rgb,
                  const char *label);

/* Simple button: framed label, visually inactive until handled elsewhere*/
void tui_button(int gx, int gy, int w, int h,
                uint32_t fg_rgb, const char *text);

/* Checkbox: "[ ] label" or "[x] label", aligned at gx/gy             */
void tui_checkbox(int gx, int gy, int checked,
                  uint32_t fg_rgb, const char *label);

/* Plain text label at cell position                                */
void tui_label(int gx, int gy, uint32_t fg_rgb,
               const char *text);

/* Thin separator line across cols (y fixed), colour fg_rgb            */
void tui_hline_sep(int gx, int gy, int w, uint32_t fg_rgb);

#endif /* CATOS_TUI_H */
