/*
 * textlayout.c -- cap2 文本排版（折行 / 省略 / 四档字号 / 伪粗 / 等宽数字）。
 *
 * 本文件当前是「接线桩」：行为与旧单行 16px 完全一致（gui_text_trim +
 * gui_draw_text），用于冻结 paint.c / layout.c 的调用接口。真实排版由
 * 文本包在保持接口不变的前提下替换下面四个函数体。
 */
#include "gui/block.h"
#include "gui_int.h"

int txt_font_h(uint8_t font_size, uint8_t scale) {
    (void)font_size;
    return 16 * (scale ? scale : 1);
}

int txt_measure_w(const char *s, uint8_t font_size, uint8_t scale,
                  uint8_t tabular) {
    (void)font_size; (void)tabular;
    int n = 0;
    while (s && s[n]) n++;
    return n * 8 * (scale ? scale : 1);
}

int txt_wrapped_h(const char *s, uint8_t font_size, uint8_t scale,
                  uint8_t tabular, int max_w, uint8_t wrap_lines,
                  uint8_t ellipsis, uint8_t line_h_px) {
    (void)s; (void)max_w; (void)wrap_lines; (void)ellipsis; (void)line_h_px;
    return txt_font_h(font_size, scale);
}

void txt_draw_block(block *b, uint32_t fg) {
    if (!b || !b->text) return;
    char buf[192];
    uint8_t sc = b->st.scale ? b->st.scale : 1;
    gui_text_trim(b->text, b->w, sc, buf, sizeof(buf));
    int blen = 0;
    while (buf[blen]) blen++;
    if (b->kind == BLK_BUTTON) {
        int tx = b->x + (b->w - 8 * sc * blen) / 2;
        int ty = b->y + (b->h - 16 * sc) / 2;
        gui_draw_text(tx, ty, buf, fg, sc);
    } else {
        gui_draw_text(b->x, b->y, buf, fg, sc);
    }
}
