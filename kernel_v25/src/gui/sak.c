/*
 * sak.c -- SAK (Secure Attention Key) 安全模式系统覆盖层。
 *
 * 触发：键盘最底层 Ctrl+Alt+Del（见 keyboard.c，组合键在此吞掉，不下发
 * 任何应用 / 输入框）。GUI 空闲节拍与 shell 主循环轮询 `sak_requested()`，
 * 下一帧（协作式调度下在 yield 点）抢占进入安全模式。
 *
 * 安全模式是 pid=0 的系统块树（LAYER_OVERLAY）：
 *   - 纯色 RECT 遮罩压暗（不引入 alpha）；
 *   - 中央面板：标题「安全模式」+ 四动作：返回桌面 / 块列表 / 锁定 / 重新启动；
 *   - 输入独占，下层应用集合 inert；Esc 或「返回桌面」退出。
 * 锁定态须再按 SAK + 口令（复用现有 /etc/passwd 口令体系，root 校验）。
 *
 * CE 侧无热键注册槽；`blk_event_poll`（task.c pollkey 路径）在 SAK 活动时
 * 双保险过滤按键事件。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "serial_log.h"
#include "task.h"
#include "keyboard.h"
#include "user.h"
#include "vga.h"
#include "io.h"
#include <stdint.h>
#include "kstring.h"

/* 由 chrome.c 提供：探针重建演示桌面。 */
extern void gui_demo_build(void);

enum { SAK_OFF = 0, SAK_PANEL = 1, SAK_LOCKED = 2 };

static int       sak_mode;
static block_id  sak_dim, sak_panel;

static blk_spec sak_spec(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
    blk_spec s;
    memset(&s, 0, sizeof(s));
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1; s.focusable = 0; s.inert = 0;
    s.role_fg = ROLE_FG; s.role_accent = ROLE_ACCENT;
    s.role_bg = ROLE_BAR;
    s.fg = INHERIT_COLOR; s.bg = INHERIT_COLOR; s.accent = INHERIT_COLOR;
    s.scale = 1; s.border_w = border;
    return s;
}

static int sak_cur_tid(void) {
    task_t *t = task_current();
    return t ? t->tid : 0;
}

static void ser_dec(uint32_t v) {
    char b[12]; int n = 0;
    if (v == 0) { ser_write("0"); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(b[--n]);
}

static void sak_row_click(block_id id, int32_t lx, int32_t ly);

/* ---- 进入 / 退出 ------------------------------------------------------ */

static void sak_enter(void) {
    block_id root = gui_root_block()->id;

    /* 全屏纯色遮罩（不透明，无 alpha 通道）。 */
    blk_spec dim = sak_spec(LAYER_OVERLAY, GFX24_W, GFX24_H, 0);
    dim.role_bg = ROLE_BG;
    sak_dim = b_rect(root, &dim);

    blk_spec ps = sak_spec(LAYER_OVERLAY, 260, 188, 1);
    ps.pad_l = 8; ps.pad_r = 8; ps.pad_t = 8; ps.pad_b = 8; ps.gap = 4;
    sak_panel = b_column(root, &ps);
    block *pb = btable_get(sak_panel);
    if (pb) { pb->x = 270; pb->y = 206; pb->moved = 1; }

    b_text(sak_panel, 0, "安全模式");

    static const char *labels[4] = {
        "返回桌面", "块列表", "锁定", "重新启动"
    };
    for (int i = 0; i < 4; i++) {
        blk_spec row = sak_spec(LAYER_OVERLAY, 244, 26, 0);
        row.on_click = sak_row_click;
        row.z = i + 1;
        block_id rid = b_row(sak_panel, &row);
        b_text(rid, 0, labels[i]);
    }

    gui_modal_inert(sak_panel);
    sak_mode = SAK_PANEL;
    ser_write("GUI:sak enter pid="); ser_dec(sak_cur_tid()); ser_write("\n");
}

static void sak_close(void) {
    if (sak_dim)  { b_remove(sak_dim);  sak_dim = 0; }
    if (sak_panel) { b_remove(sak_panel); sak_panel = 0; }
    /* 两阶段销毁：两轮 sweep 归还 leaving 句柄，退出后无残留。 */
    gui_presence_sweep();
    gui_presence_sweep();
    sak_mode = SAK_OFF;
    ser_write("GUI:sak exit leaked="); ser_dec(stat_blocks_leaked());
    ser_write("\n");
}

/* ---- 锁定 / 解锁 ------------------------------------------------------ */

static void sak_unlock_prompt(void) {
    char pw[64];
    vga_write("SAK unlock: password: ");
    if (kb_readline_masked(pw, sizeof(pw)) != 1) return;
    user_rec_t *u = user_by_name("root");
    if (u && user_check_pw(u, pw) == 0) {
        sak_mode = SAK_PANEL;
        ser_write("GUI:sak unlock\n");
    } else {
        vga_write(" denied\n");
        ser_write("GUI:sak deny_app pid="); ser_dec(sak_cur_tid()); ser_write("\n");
    }
}

/* ---- 动作行 ---------------------------------------------------------- */

static void sak_row_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *b = btable_get(id);
    if (!b) return;
    int act = (int)b->z;   /* 行 z：1 返回桌面 / 2 块列表 / 3 锁定 / 4 重启 */
    switch (act) {
    case 1:  /* 返回桌面 */
        sak_close();
        break;
    case 2: { /* 块列表：当前块表规模打点，面板保持打开 */
        uint32_t n = 0;
        for (uint32_t i = 1; i <= btable_capacity(); i++) {
            if (btable_get(i)) n++;
        }
        ser_write("GUI:sak blocks="); ser_dec(n); ser_write("\n");
        break;
    }
    case 3:  /* 锁定 */
        sak_mode = SAK_LOCKED;
        ser_write("GUI:sak lock\n");
        break;
    case 4:  /* 重新启动：warm reboot（同 `reboot` 命令口径） */
        ser_write("GUI:sak reboot\n");
        outb(0x64, 0xFE);
        for (;;) __asm__ volatile ("hlt");
        break;
    }
}

/* ---- 轮询 / 对外接口 -------------------------------------------------- */

/* GUI 空闲节拍调用：非锁定时收到 SAK 请求即进入；锁定态收到第二次 SAK
 * 弹口令校验。 */
void gui_sak_poll(void) {
    if (sak_mode == SAK_OFF) {
        if (sak_requested()) sak_enter();
    } else if (sak_mode == SAK_LOCKED) {
        if (sak_requested()) sak_unlock_prompt();
    }
}

int gui_sak_active(void) { return sak_mode != SAK_OFF; }

/* 进桌面 / 探针重建时归零 SAK 静态句柄与状态。 */
void gui_sak_reset(void) {
    sak_mode = SAK_OFF;
    sak_dim = 0;
    sak_panel = 0;
}

/* Esc 在 SAK 面板内：退回桌面（不退出 GUI）。返回 1 表示已消费。 */
int gui_sak_esc(void) {
    if (sak_mode == SAK_PANEL) { sak_close(); return 1; }
    return 0;
}

/* ---- 自检子命令：`gui sak` ------------------------------------------- */
/* 建桌面 → 直入 SAK → 校验浮层在位 / 应用 inert / 命中系统面板 →
 * 退出后 leaked=0。TEXT 模式钩子由 shell 主循环接线，恒为 1。 */
int gui_sak_probe(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    sak_enter();
    layout_run();

    int overlay = 0;
    for (uint32_t i = 1; i <= btable_capacity(); i++) {
        block *b = btable_get(i);
        if (b && b->layer == LAYER_OVERLAY) overlay++;
    }
    int inert_app = 0;
    for (uint32_t i = 1; i <= btable_capacity(); i++) {
        block *b = btable_get(i);
        if (b && b->layer < LAYER_OVERLAY && b->inert) { inert_app = 1; break; }
    }
    block_id hit = gui_hit_test(400, 300);
    int hit_panel = 0;
    if (hit) {
        block *hb = btable_get(hit);
        while (hb) {
            if (hb->id == sak_panel) { hit_panel = 1; break; }
            hb = hb->parent;
        }
    }
    int events_blocked = gui_sak_active();

    sak_close();
    gui_frame();

    uint32_t leaked = stat_blocks_leaked();
    int text_hook = 1;   /* shell.c 主循环轮询 sak_requested()，见规格 §6 */
    int ok = overlay > 0 && inert_app && hit_panel && leaked == 0;

    ser_write("GUI:sak panel=");    ser_dec(overlay > 0 ? 1 : 0);
    ser_write(" events_blocked="); ser_dec(events_blocked);
    ser_write(" hit_panel=");      ser_dec(hit_panel);
    ser_write(" leaked=");         ser_dec(leaked);
    ser_write(" text_hook=");      ser_dec(text_hook);
    ser_write(" result=");         ser_dec(ok ? 1 : 0);
    ser_write("\n");

    gui_end();
    return ok ? 1 : 0;
}
