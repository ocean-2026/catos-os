/*
 * cap2p_interact.c -- 交互/通道包探针（popover 翻转钳制 / CE BUTTON+INPUT /
 * 滚动条滚轮与拖 thumb / 动画 12 帧落点）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 */
#include "gui/block.h"
#include "gui_int.h"

int cap2_probe_interact(const char *arg) {
    (void)arg;
    return 0;
}
