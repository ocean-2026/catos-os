/*
 * icons.c -- cap2 内置单色图标（16 / 32 两档）+ tint 令牌着色。
 *
 * 接线桩：暂无字模时画一圈描边占位，保证块位置/尺寸可见；渲染包在此
 * 填图标表与 tint 着色（fg / bg / accent / border 四角色）。
 */
#include "gui/block.h"
#include "gui/tokens.h"
#include "gui_int.h"

void icon_draw(block *b, uint32_t fg, uint32_t accent) {
    (void)fg; (void)accent;
    if (!b || b->w <= 0 || b->h <= 0) return;
    gui_draw_border(b->x, b->y, b->w, b->h, gui_active_style()->border, 1);
}
