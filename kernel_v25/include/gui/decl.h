#ifndef CATOS_GUI_DECL_H
#define CATOS_GUI_DECL_H

/*
 * decl.h -- CE 声明式 ABI（P3，append-only，syscall 槽 72..76）。
 *
 * 应用只能通过「声明一批块描述」把自己挂到内核块树的 WORK 子树之下；
 * 内核 copy_from_user 拷入后整批校验，非法整批拒绝、不触碰旧子树；
 * 内核绝不保存任何用户态指针。按 (pid, local-id) 隔离，退出/被杀整棵
 * 子树回收。CHROME / OVERLAY / TOAST 层一律拒绝。
 */
#include <stdint.h>
#include "ce.h"            /* ce_blk_rec_t / ce_blk_event_t 镜像 */

/* 拒绝码（append，0..-34 既有槽位不动）。 */
#define E_DECL_QUOTA   (-35)   /* 超块数 / 深度 / 文本 / 池配额 */
#define E_DECL_LAYER   (-36)   /* 越层：声明到 WORK 之外 */
#define E_DECL_BADID   (-37)   /* id 非法 / 重复 / 父链断裂 */
#define E_DECL_RES     (-38)   /* 资源句柄缺失 / 不归属本应用 */
#define E_DECL_BADREC  (-39)   /* 记录结构非法 */

/* 配额（单应用）。 */
#define DECL_MAX_BLOCKS   256
#define DECL_MAX_DEPTH    16
#define DECL_MAX_TEXT     4096
#define DECL_MAX_POOL     32768

/* 事件类型（不给绝对屏幕坐标，不给鼠标原始流）。 */
enum {
    DECL_EV_CLICK = 0,
    DECL_EV_KEY,
    DECL_EV_FOCUS_IN,
    DECL_EV_FOCUS_OUT,
    DECL_EV_RESIZE,
    DECL_EV_CLOSE
};

struct task;
struct block;

/* 内核侧接线（task.c 调用）。 */
int  decl_commit(struct task *t, const void *recs, int n,
                 const char *strings, int slen);
int  decl_event_poll(struct task *t, ce_blk_event_t *ev, int timeout_ms);
int  decl_image_load(struct task *t, const char *path);
int  decl_image_free(struct task *t, int res);
int  decl_text_width(const char *s);
void decl_task_cleanup(struct task *t);
void decl_frame_tick(void);

/* 事件实泵：输入/焦点/尺寸路径回投到所属应用的事件队列。 */
void decl_post_click(struct block *b, int lx, int ly);
void decl_post_key(int code, int mods);
void decl_post_focus(struct block *b, int in);
void decl_post_resize(struct block *b, int w, int h);
void decl_post_close(struct block *b);

/* 跨应用焦点链（#33）：Tab 循环 / Esc 归还 / 直接置焦。 */
void decl_focus_set(int tid);
void decl_focus_next(void);
void decl_focus_clear(void);

/* 诊断与统计。 */
void decl_dump_pid(int tid);
int  decl_pid_blocks(int tid);

/* 探针：`gui decl` 自检一轮。 */
int  decl_self_test(void);

#endif /* CATOS_GUI_DECL_H */
