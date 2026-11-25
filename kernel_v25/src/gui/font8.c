/*
 * font8.c -- cap2 8px 档点阵字库（ASCII 95 + 常用标点）。
 *
 * 字模不依赖外部字体文件：首次取用时由内核自带的 IBM VGA 8x16 ASCII
 * 字模（render_font_glyph）竖向两两 OR 压缩成 8x8，行像素不丢失。
 * 宽字符（CJK）不在此表，8px 档由 textlayout.c 对 16x16 汉字模做
 * 2x2 覆盖率加权降采样。
 *
 * 伪粗 8px 档：f8_ascii_bold() 产出 8x16 位图（行复制 + 横向 1px
 * 膨胀，右移 1px 叠加）。
 */
#include <stdint.h>
#include "render_be.h"

#define F8_N 95

static uint8_t f8_table[F8_N][8];
static uint8_t f8_ready = 0;

static void f8_build(void) {
    for (int i = 0; i < F8_N; i++) {
        const unsigned char *g =
            render_font_glyph((unsigned char)(0x20 + i));
        for (int r = 0; r < 8; r++)
            f8_table[i][r] = (uint8_t)(g[2 * r] | g[2 * r + 1]);
    }
    f8_ready = 1;
}

/* 返回 8 字节（8 列 x 8 行，MSB-left，top-to-bottom）；缺字回落 '?'。 */
const uint8_t *f8_ascii(unsigned char ch) {
    if (!f8_ready) f8_build();
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    return f8_table[ch - 0x20];
}

/* 伪粗 8x16 位图：8x8 源模横向右移 1px 叠加，行复制撑到 16 行。 */
void f8_ascii_bold(unsigned char ch, uint8_t out[16]) {
    const uint8_t *g = f8_ascii(ch);
    for (int r = 0; r < 8; r++) {
        uint8_t d = (uint8_t)(g[r] | (g[r] >> 1));
        out[2 * r] = d;
        out[2 * r + 1] = d;
    }
}
