/*
 * LEGACY REFERENCE ONLY, NOT SHIPPED.
 *
 * 旧命令式位图文本原型（gfx_blit / display_mode(3) 路径）；块化桌面只认
 * 声明式块树，此路径退役。源码保留作学习参考，不打包。
 */

/* gfxtext.c -- PROTOTYPE (v2.73): CE-side gfx text rendering.
 *
 * Goal: prove a ring3 CE app can draw crisp 24bpp text using ONLY the
 * existing gfx_blit ABI (slot 63) + the genuine IBM VGA 8x16 font, with
 * ZERO kernel/ABI changes.  If this works, desktop.c can be moved off the
 * text backend without touching the kernel.
 *
 * Method: compose an offscreen RGB24 window (title bar + text rows) at
 * SCALE=2 (each font pixel -> 2x2 screen px), then hand the whole bitmap
 * to gfx_blit in one syscall.  The LFB stays supervisor-only.
 */
#include "ce_abi.h"

/* Genuine IBM VGA 8x16 ROM font: static const unsigned char font_ascii[95][16] */
#include "../src/font_ascii.inc"

/* __ce_api is provided by ce_app.c (weak, in .ce_api) since v2.49.
 * Do NOT redefine it here: a strong redefinition shifts which .ce_api cell
 * build_ce.sh's nm regex picks, so the kernel patches a different slot. */

#define SCALE 2
#define CW    (8 * SCALE)
#define CH    (16 * SCALE)
#define COLS  24
#define ROWS  6
#define SW    (COLS * CW)          /* 384 */
#define SH    (ROWS * CH)          /* 192 */

static unsigned char fb[SW * SH * 3];   /* 384*192*3 = 216 KB, under 256 KB bss cap */
static gfx_blit_t blit;

static void px(int x, int y, unsigned r, unsigned g, unsigned b) {
    if (x < 0 || x >= SW || y < 0 || y >= SH) return;
    unsigned char *p = fb + (y * SW + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}
static void rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j, r, g, b);
}
static void glyph(char ch, int col, int row, unsigned r, unsigned g, unsigned b) {
    unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c > 0x7e) c = '?';
    const unsigned char *rows = font_ascii[c - 0x20];
    int ox = col * CW, oy = row * CH;
    for (int j = 0; j < 16; j++) {
        unsigned char bits = rows[j];
        for (int i = 0; i < 8; i++) {
            if (!(bits & (0x80 >> i))) continue;
            rect(ox + i * SCALE, oy + j * SCALE, SCALE, SCALE, r, g, b);
        }
    }
}
static void text(int col, int row, const char *s, unsigned r, unsigned g, unsigned b) {
    while (*s && col < COLS) { glyph(*s++, col++, row, r, g, b); }
}

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    catos_api_t *api = __ce_api;
    if (!api->gfx_info || !api->gfx_blit || !api->display_mode) {
        api->write("GFXTEXT SKIP (no drawing ABI)\n");
        return 0;
    }
    if (api->gfx_info(GFX_INFO_ACTIVE) != 1) {
        api->display_mode(3);
        if (api->gfx_info(GFX_INFO_ACTIVE) != 1) {
            api->write("GFXTEXT SKIP (mode not vbe24)\n");
            return 0;
        }
    }

    /* Compose the offscreen window. */
    rect(0, 0, SW, SH, 0x0a, 0x0e, 0x14);              /* window bg   */
    rect(0, 0, SW, CH + 4, 0x1f, 0x3a, 0x5f);          /* title bar   */
    rect(0, CH + 4, SW, 2, 0x35, 0x7e, 0xce);          /* accent line */
    text(1, 0, "CatOS CDE - gfx24 text", 0xff, 0xff, 0xff);

    text(1, 2, "The quick brown fox", 0xe0, 0xe6, 0xed);
    text(1, 3, "jumps over the lazy", 0x7e, 0xce, 0xc8);
    text(1, 4, "dog 0123456789!", 0xf0, 0xc6, 0x74);
    text(1, 5, "ABCDEFGHIJKLMNOPQRST", 0xff, 0x9b, 0x9b);

    blit.src = fb; blit.x = 208; blit.y = 200;
    blit.w = SW; blit.h = SH;
    int rc = api->gfx_blit(&blit);
    if (rc != 0) { api->write("GFXTEXT FAIL (blit)\n"); return 1; }

    api->write("GFXTEXT PASS (CE-side gfx text, zero ABI change)\n");
    /* Stay in gfx mode so the probe can screendump; caller switches back. */
    return 0;
}
