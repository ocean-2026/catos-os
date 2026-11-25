/*
 * cap2probe.c -- cap2 探针族分派：`gui cap2-<case>`。
 *
 * 三个包各自在 cap2p_render.c / cap2p_text.c / cap2p_interact.c 里实现
 * 自己的 case（返回 1 = 已消费），互不改文件，避免并行冲突。
 */
#include "gui/block.h"
#include "gui_int.h"

int cap2_probe_cmd(const char *arg) {
    if (cap2_probe_render(arg))   return 1;
    if (cap2_probe_text(arg))     return 1;
    if (cap2_probe_interact(arg)) return 1;
    if (cap2_probe_ce(arg))       return 1;
    return 0;
}
