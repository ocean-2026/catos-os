/*
 * icons.c -- cap2 内置单色图标（16 档位图；≥24px 块 2× 放大到 32 档）+ tint 着色。
 *
 * 图标号（b->icon）：0 menu / 1 search / 2 settings / 3 file / 4 folder /
 * 5 warn / 6 info / 7 close。tint_role：0=不自着色(走 fg) 1=fg 2=bg
 * 3=accent 4=border。逐像素经 blk_cov 覆盖率混合。
 *
 * 位图存储：8 图标 × 16 行 × 16bit = 256B 静态，未越 4KiB 红线。
 */
#include "gui/block.h"
#include "gui/tokens.h"
#include "gui_int.h"
#include "gfx24.h"

/* 每行 16 位：col c 对应位 (15-c)。 */
static const uint16_t ICON_GLYPH[8][16] = {
    /* 0 menu：三条横杠 */
    { 0x0000,0x0000,0x0000,0x0000,
      0xFFF8,0x0000,0x0000,0xFFF8,
      0x0000,0x0000,0xFFF8,0x0000,
      0x0000,0xFFF8,0x0000,0x0000 },
    /* 1 search：圆 + 斜柄 */
    { 0x0000,0x0000,0x0000,0x0000,
      0x03C0,0x07E0,0x07E0,0x07E0,
      0x07E0,0x07E0,0x03C0,0x0010,
      0x0008,0x0004,0x0000,0x0000 },
    /* 2 settings：圆环 + 中心 */
    { 0x0000,0x0000,0x0000,0x0000,
      0x03C0,0x07E0,0x0420,0x0420,
      0x07E0,0x0420,0x0420,0x07E0,
      0x03C0,0x0000,0x0000,0x0000 },
    /* 3 file：文档轮廓 */
    { 0x0000,0x0000,0x0000,0x07E0,
      0x0810,0x07E0,0x0810,0x0810,
      0x0810,0x0810,0x0810,0x07E0,
      0x0000,0x0000,0x0000,0x0000 },
    /* 4 folder：文件夹 */
    { 0x0000,0x0000,0x0000,0x0000,
      0x0F00,0xFFF8,0x1008,0x1008,
      0x1008,0x1008,0x1008,0xFFF8,
      0x0000,0x0000,0x0000,0x0000 },
    /* 5 warn：三角 + 感叹号 */
    { 0x0000,0x0000,0x0000,0x0080,
      0x01C0,0x01C0,0x03E0,0x0080,
      0x0080,0x0080,0x0080,0x07F0,
      0x0000,0x0000,0x0000,0x0000 },
    /* 6 info：圆 i */
    { 0x0000,0x0000,0x0000,0x0000,
      0x03E0,0x07F0,0x0080,0x07F0,
      0x0080,0x0080,0x0080,0x07F0,
      0x03E0,0x0000,0x0000,0x0000 },
    /* 7 close：X */
    { 0x0000,0x0000,0x0000,0x0000,
      0x0810,0x0420,0x0240,0x0180,
      0x0180,0x0240,0x0420,0x0810,
      0x0000,0x0000,0x0000,0x0000 }
};

void icon_draw(block *b, uint32_t fg, uint32_t accent) {
    if (!b || b->w <= 0 || b->h <= 0) return;
    const struct style *tok = gui_active_style();
    uint32_t color;
    switch (b->tint_role) {
    case 1:  color = tok->fg;     break;
    case 2:  color = tok->bg;     break;
    case 3:  color = tok->accent; break;
    case 4:  color = tok->border; break;
    default: color = fg;          break;
    }

    uint8_t ic = b->icon;
    if (ic >= 8) ic = 0;
    int scale = (b->w >= 24 && b->h >= 24) ? 2 : 1;
    int gw = 16 * scale, gh = 16 * scale;
    int ox = b->x + (b->w - gw) / 2;
    int oy = b->y + (b->h - gh) / 2;
    const uint16_t *glyph = ICON_GLYPH[ic];

    for (int row = 0; row < 16; row++) {
        uint16_t bits = glyph[row];
        for (int col = 0; col < 16; col++) {
            if (!(bits & (1u << (15 - col)))) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int px = ox + col * scale + sx;
                    int py = oy + row * scale + sy;
                    int cov = blk_cov(b, px, py);
                    blk_px_blend(px, py, color, cov);
                }
        }
    }
    (void)accent;
}
