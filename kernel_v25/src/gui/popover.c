/*
 * popover.c -- cap2 锚定浮层：measure -> place -> 钳制(8px) -> 翻转 -> 内部滚动。
 *
 * 接线桩：不接管几何（按普通容器走父框布局），dismiss 恒不命中。
 */
#include "gui/block.h"
#include "gui_int.h"

void pop_layout(block *b) { (void)b; }

int pop_dismiss_at(int x, int y) { (void)x; (void)y; return 0; }

int pop_esc(void) { return 0; }
