/*
 * sb.c -- cap2 标准滚动条：内核统一 track + thumb + 滚轮 + 拖 thumb。
 *
 * 接线桩：sb_visible 恒 0（旧行为：容器自己画，内核不介入）。
 */
#include "gui/block.h"
#include "gui_int.h"

int sb_visible(block *b) { (void)b; return 0; }

void sb_paint(block *b) { (void)b; }

int sb_wheel(block *b, int dy) { (void)b; (void)dy; return 0; }

int sb_mouse(block *b, int x, int y, uint8_t phase) {
    (void)b; (void)x; (void)y; (void)phase;
    return 0;
}
