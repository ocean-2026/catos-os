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
static void   (*ib_submit)(block_id id, const char *text);

void ib_render(block_id id);

static int xlen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

/* ---- 挂载 / 复位 ---------------------------------------------------------- */

void ib_attach(block_id id, void (*on_submit)(block_id, const char *text)) {
    ib_root = id;
    ib_submit = on_submit;
    ib_len = 0; ib_pos = 0;
    ib_buf[0] = 0;
    ib_active = 1;
    block *b = btable_get(id);
    if (b) b->render = ib_render;
}

void ib_reset(void) {
    ib_root = 0;
    ib_active = 0;
    ib_submit = 0;
    ib_len = 0; ib_pos = 0;
    ib_buf[0] = 0;
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

    switch (code) {
    case 8:  ib_backspace(); break;                          /* Backspace */
    case KB_LEFT:  if (ib_pos > 0) ib_pos--; break;
    case KB_RIGHT: if (ib_pos < ib_len) ib_pos++; break;
    case KB_HOME:  ib_pos = 0; break;
    case KB_END:   ib_pos = ib_len; break;
    case 10:                                              /* Enter 提交 */
        if (ib_submit) ib_submit(ib_root, ib_buf);
        /* CE 归属块：把提交文本回传到声明式事件通道（内核桌面回调不受影响）。 */
        {
            block *b = btable_get(ib_root);
            if (b && b->owner != 0) decl_post_input(b, ib_buf);
        }
        return 1;
    case 27:  return 1;                                   /* Esc 仅消费，不退桌面 */
    default:
        if (code >= 0x20 && code <= 0x7E) ib_insert((char)code);
        else return 0;
    }
    block *b = btable_get(ib_root);
    if (b) b->dirty_paint = 1;
    return 1;
}
