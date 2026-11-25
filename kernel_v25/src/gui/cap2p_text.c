/*
 * cap2p_text.c -- 文本排版包探针（wrap / 省略 / 四档字高 / 伪粗 / 等宽）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 */
#include "gui/block.h"
#include "gui_int.h"

int cap2_probe_text(const char *arg) {
    (void)arg;
    return 0;
}
