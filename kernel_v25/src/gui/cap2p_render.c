/*
 * cap2p_render.c -- 渲染/合成包探针（alpha / 阴影 / 逐角 / 图标 / 滚动条绘制）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 */
#include "gui/block.h"
#include "gui_int.h"

int cap2_probe_render(const char *arg) {
    (void)arg;
    return 0;
}
