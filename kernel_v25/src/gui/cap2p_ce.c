/*
 * cap2p_ce.c -- CE 通道包探针（rec2 全字段镜像 / kind 扩权 / 事件回传）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 */
#include "gui/block.h"
#include "gui_int.h"

int cap2_probe_ce(const char *arg) {
    (void)arg;
    return 0;
}
