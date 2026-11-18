/*
 * vga.c -- thin LEGACY shim: vga_*() -> con_*().
 *
 * There is intentionally NO hardware access here.  Every pixel and every
 * VGA register is owned by src/be_text.c or src/be_gfx.c.
 */
#include "vga.h"

/* v2.59: optional stdout tee hook.  The shell installs this while a ring0
 * builtin is running under a plain file redirect; each printed character is
 * mirrored into the redirect buffer.  Left NULL otherwise (zero cost on the
 * normal console path).  This only mirrors the shell's line output --
 * positioned editor/TUI writes and the ring3 task SYS_WRITE path do not go
 * through it. */
static void (*g_tee_char)(char c);

void vga_set_tee(void (*hook)(char c)) { g_tee_char = hook; }

static void tee_str(const char *s) {
    if (!g_tee_char || !s) return;
    for (; *s; s++) g_tee_char(*s);
}

void vga_init(void)                       { render_init(); }
void vga_clear(void)                      { con_clear(); }
void vga_putc(char c)                     { if (g_tee_char) g_tee_char(c); con_putc(c); }
void vga_write(const char *s)             { tee_str(s); con_write(s); }
void vga_set_color(uint8_t f, uint8_t b)  { con_set_style(f, b, con_get_attrs()); }
void vga_set_fg(uint8_t f)                { con_set_fg(f); }
uint8_t vga_get_fg(void)                  { return con_get_fg(); }
void vga_move_cursor(void)                { con_move_cursor(); }
void vga_set_cursor(uint8_t x, uint8_t y) { con_set_cursor(x, y); }
void vga_putc_at(uint8_t x, uint8_t y, char c) { con_putc_at(x, y, c); }
void vga_write_at(uint8_t x, uint8_t y, const char *s) {
    con_write_at(x, y, s);
}
void vga_write_n(const char *s, int n, int final) {
    if (g_tee_char && s) {
        for (int i = 0; i < n; i++) g_tee_char(s[i]);
    }
    con_write_n(s, n, final);
}
void vga_write_at_utf8(uint8_t x, uint8_t y, const char *s) {
    con_write_at_utf8(x, y, s);
}
