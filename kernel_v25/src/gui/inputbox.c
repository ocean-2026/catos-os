/*
 * inputbox.c -- BLK_INPUT 单行输入框（块子树，规格 §7 组件层）。
 *
 * v1 单实例：聚焦后接管键盘（可打印字符 / Backspace / Left/Right /
 * Home/End），绘制文本与光标竖线；Enter 触发 on_submit。不引入进程 /
 * IPC / 显示服务；键盘走现有全局 key_hook 路径，钩子内只打脏位。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "keyboard.h"
#include <stdint.h>
#include <stddef.h>
#include "kstring.h"
#include "fs.h"

#define IB_CAP FS_NAME_MAX

static char     ib_buf[IB_CAP];
static int      ib_len;
static int      ib_pos;
static block_id ib_root;
static int      ib_active;
static uint8_t  ib_owner;        /* 激活时记录归属 tid；块被替换/回收后仍可对账 */
static void   (*ib_submit)(block_id id, const char *text);

void ib_render(block_id id);

static int xlen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

/* ---- 挂载 / 复位 ---------------------------------------------------------- */

/* 点击接管键盘：CE 输入块走这条（无 legacy on_submit 回调，回车由
 * ib_key 经 decl_post_input 回投 CE owner）；已在该块上则不动状态，
 * 避免点击重入清空已输入文本。 */
void ib_activate(block_id id) {
    if (ib_root == id) return;
    ib_root = id;
    ib_submit = 0;
    ib_len = 0; ib_pos = 0;
    ib_buf[0] = 0;
    ib_active = 1;
    block *b = btable_get(id);
    ib_owner = b ? b->owner : 0;
    if (b) b->render = ib_render;
}

void ib_attach(block_id id, void (*on_submit)(block_id, const char *text)) {
    ib_root = id;
    ib_submit = on_submit;
    ib_len = 0; ib_pos = 0;
    ib_buf[0] = 0;
    ib_active = 1;
    ib_owner = 0;
    block *b = btable_get(id);
    if (b) b->render = ib_render;
}

void ib_reset(void) {
    ib_root = 0;
    ib_active = 0;
    ib_owner = 0;
    ib_submit = 0;
    ib_len = 0; ib_pos = 0;
    ib_buf[0] = 0;
}

/* CE 任务退出 / 子树回收前调用：若当前钩子属于该 tid 的块，一并复位，
 * 避免 ib_root 悬空指向已回收块。owner 在激活时记录，兼容子树先被
 * 提交替换、块已释放的场景。 */
void ib_reset_if_owner(int tid) {
    if (ib_active && ib_owner == (uint8_t)tid) ib_reset();
}

int ib_is_active(void) { return ib_active; }

/* ---- 自绘：文本 + 光标竖线（底色 / 边框由 paint 默认路径处理） ------------- */

void ib_render(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    const struct style *tok = gui_active_style();
    int x = b->x + b->pad_l;
    int y = b->y + (b->h - 16) / 2;
    gui_draw_text(x, y, ib_buf, tok->fg, 1);
    int cx = x + 8 * ib_pos;
    gui_fill_rect(cx, y, 1, 16, tok->fg);
}

/* ---- 键盘 ---------------------------------------------------------------- */

static void ib_insert(char c) {
    if (ib_len >= IB_CAP - 1) return;
    for (int i = ib_len; i > ib_pos; i--) ib_buf[i] = ib_buf[i - 1];
    ib_buf[ib_pos++] = c;
    ib_len++;
    ib_buf[ib_len] = 0;
}

static void ib_backspace(void) {
    if (ib_pos <= 0) return;
    for (int i = ib_pos - 1; i < ib_len - 1; i++) ib_buf[i] = ib_buf[i + 1];
    ib_len--; ib_pos--;
    ib_buf[ib_len] = 0;
}

int ib_key(int code, void *ud) {
    (void)ud;
    if (!ib_active) return 0;

    /* 块已被提交替换 / 回收：钩子失效，复位后把键交回默认路径。 */
    block *live = btable_get(ib_root);
    if (!live) { ib_reset(); return 0; }

    switch (code) {
    case 8:  ib_backspace(); break;                          /* Backspace */
    case KB_LEFT:  if (ib_pos > 0) ib_pos--; break;
    case KB_RIGHT: if (ib_pos < ib_len) ib_pos++; break;
    case KB_HOME:  ib_pos = 0; break;
    case KB_END:   ib_pos = ib_len; break;
    case 10:                                              /* Enter 提交 */
        if (ib_submit) ib_submit(ib_root, ib_buf);
        /* CE 归属块：把提交文本回传到声明式事件通道（内核桌面回调不受影响）。 */
        if (live->owner != 0) decl_post_input(live, ib_buf);
        return 1;
    case 27:
        /* legacy 输入框（owner==0）继续吞 Esc；CE 归属块放行，交给桌面
         * 默认路径退出空闲循环。 */
        if (ib_owner != 0) return 0;
        return 1;
    default:
        if (code >= 0x20 && code <= 0x7E) ib_insert((char)code);
        else return 0;
    }
    live->dirty_paint = 1;
    return 1;
}
