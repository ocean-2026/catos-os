#ifndef CATOS_GUI_BLOCK_H
#define CATOS_GUI_BLOCK_H

/*
 * block.h -- 块化 GUI 数据模型（规格 §1）。
 *
 * 全系统只有「块」这一种节点：标题栏、任务栏、开始菜单、通知、提示
 * 全部是块；一个带标题栏子块的 BLK_STACK 子树就是传统意义上的应用
 * 容器。代码 / 结构 / API / 注释里禁止出现传统桌面窗口概念的命名。
 */
#include <stdint.h>
#include "tokens.h"

typedef uint32_t block_id;          /* 0 = 无效；>=1 全局唯一 */

typedef enum {
    BLK_ROOT = 0,
    BLK_COLUMN,
    BLK_ROW,
    BLK_STACK,
    BLK_TEXT,
    BLK_RECT,
    BLK_IMAGE,
    BLK_SURFACE,
    BLK_SPACER,
    BLK_DIVIDER
} blk_kind;

/* 主轴对齐（COLUMN 纵向 / ROW 横向） */
enum {
    ALN_START = 0,
    ALN_CENTER,
    ALN_END,
    ALN_SPACE_BETWEEN
};

/* 交叉轴对齐 */
enum {
    XALN_START = 0,
    XALN_CENTER,
    XALN_END,
    XALN_STRETCH
};

/* 层枚举（规格 §5），绘制按 layer 升序 → z 升序 → 树先序 */
enum {
    LAYER_BG      = 0,
    LAYER_CONTENT = 100,
    LAYER_WORK    = 200,
    LAYER_CHROME  = 3000,
    LAYER_OVERLAY = 4000,
    LAYER_TOAST   = 5000
};

/* 声明属性集合（规格 §2 blk_spec）。fg/bg 为 0xFFFFFFFF 表示继承父块。 */
typedef struct {
    int32_t  req_w, req_h;              /* -1 = auto */
    int32_t  pad_t, pad_r, pad_b, pad_l;
    int32_t  gap;
    uint8_t  align_main, align_cross, align_self;
    uint16_t layer;
    int32_t  z;
    uint8_t  visible, focusable, inert;
    uint32_t fg, bg, accent;
    uint8_t  scale, border_w;
    const char   *text;
    const uint8_t *pix;
    int32_t  pix_w, pix_h;
    void (*on_click)(block_id id, int32_t lx, int32_t ly);
    void (*on_drag )(block_id id, int32_t dx, int32_t dy, uint8_t phase);
} blk_spec;

/* 一帧 build() 产出的描述项；gui_commit_desc 按 id 与上帧 diff。 */
typedef struct {
    block_id      id;
    blk_kind      kind;
    block_id      parent;
    const blk_spec *spec;
    const char    *text;
    const uint8_t *pix;
    int32_t       pix_w, pix_h;
} blk_desc;

typedef struct block {
    block_id   id;
    blk_kind   kind;
    struct block *parent, *first_child, *next_sibling;

    int32_t    req_w, req_h;            /* 声明尺寸，-1 = auto */
    int32_t    x, y, w, h;              /* 布局结果（像素，绝对坐标） */
    int32_t    pad_t, pad_r, pad_b, pad_l;
    int32_t    gap;
    uint8_t    align_main, align_cross, align_self;

    uint16_t   layer;
    int32_t    z;
    uint8_t    visible, focusable, focused, inert;

    uint8_t    dirty_layout, dirty_paint, dirty_children, leaving;
    uint8_t    moved;                    /* 被拖拽/移动过，布局保留其绝对位置 */
    uint8_t    snap;                     /* 本帧快照：布局清位前记录的脏标记 */

    const char   *text;
    const uint8_t *pix;
    int32_t    pix_w, pix_h;
    uint8_t    pix_opaque_only;          /* v1 只支持不透明位图 */

    struct style st;

    void (*on_click)(block_id id, int32_t lx, int32_t ly);
    void (*on_drag )(block_id id, int32_t dx, int32_t dy, uint8_t phase);

    /* ---- 内部字段（不对外） ---- */
    struct block *free_next;             /* 空闲链 */
    uint8_t  desc_seen;                  /* commit diff 本轮是否见过 */
    uint8_t  owner;                      /* 预留：0=内核，>0=pid；v1 恒 0 */
} block;

#endif /* CATOS_GUI_BLOCK_H */
