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

struct svg_icon;                /* v3 svg.c 光栅化图标（运行时缓存句柄） */

/* 声明色哨兵：0xFFFFFFFF 表示继承父块/角色，不固化具体 RGB。 */
#define INHERIT_COLOR 0xFFFFFFFFu

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
    BLK_DIVIDER,
    BLK_CANVAS,
    BLK_FREE,                        /* v2 追加在末尾：自由布局容器 */
    BLK_BUTTON,                      /* desk 追加：按钮 */
    BLK_INPUT,                       /* desk 追加：单行输入框 */
    BLK_TERM,                        /* desk 追加：块内终端 */
    /* ---- cap2 能力扩展：锚定浮层 / 内置单色图标（append-only，旧值不动） ---- */
    BLK_POPOVER,                     /* 锚定浮层：内核做 measure→place/翻转/钳制/内部滚动 */
    BLK_ICON                         /* 内置单色图标：icon 号 + tint 角色着色 */
} blk_kind;

/* 溢出三态（v2，规格 v2 §2）。几何上允许越界；视觉/命中按本字段对齐。 */
enum {
    OVF_CLIP = 0,                   /* 默认：越界部分不画、不可点 */
    OVF_VISIBLE = 1,                /* 越界仍绘制、仍可命中（浮层/徽标） */
    OVF_SCROLL = 2                  /* 裁剪 + 按 scroll_x/y 平移，不改子树几何 */
};

/* 自由布局锚点位掩码（v2）。水平/垂直可独立组合。 */
enum {
    ANC_L       = 0x1,
    ANC_R       = 0x2,
    ANC_T       = 0x4,
    ANC_B       = 0x8,
    ANC_HCENTER = 0x10,
    ANC_VCENTER = 0x20
};

/* 拖拽模式（v2 属性化手势）。 */
enum {
    DRAG_NONE  = 0,                 /* 不可拖 */
    DRAG_TITLE = 1,                 /* 仅标题带可拖 */
    DRAG_FULL  = 2                  /* 整块可拖 */
};

/* 拖拽边界约束（v2）。 */
enum {
    BOUND_FREE   = 0,               /* 不限 */
    BOUND_PARENT = 1,               /* 限制父框内含 padding */
    BOUND_SCREEN = 2,               /* 限制屏幕内 */
    BOUND_LOOSE  = 3                /* 宽松：顶部标题条(24px)在屏内、水平至少留 6px，其余可出屏 */
};

/* 缩放方向位掩码（v2）；0 视为右下（R|B）。 */
enum {
    RESIZE_L = 0x1,
    RESIZE_R = 0x2,
    RESIZE_T = 0x4,
    RESIZE_B = 0x8
};

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

/* v2.83 A7.3 位图/画布缩放档位（block.fit）。 */
#define FIT_NONE     0    /* 1:1 贴左上（旧行为，默认） */
#define FIT_CONTAIN  1    /* 等比缩放到完整可见，居中留边（留档） */
#define FIT_FILL     2    /* 拉伸铺满块框（最近邻） */

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
    uint8_t  role_fg, role_bg, role_accent;
    uint8_t  scale, border_w;
    const char   *text;
    const uint8_t *pix;
    int32_t  pix_w, pix_h;
    void (*on_click)(block_id id, int32_t lx, int32_t ly);
    void (*on_drag )(block_id id, int32_t dx, int32_t dy, uint8_t phase);
    void (*on_dblclick)(block_id id, int32_t lx, int32_t ly);
    /* 组件自绘回调：合成层在默认绘制之后、裁剪框内调用；回调只打脏位，
     * 不做嵌套提交。usr 为组件私有上下文（v1 单实例，内核内使用）。 */
    void (*render)(block_id id);
    void  *usr;

    /* ---- v2 自由布局 / 溢出 / 交互属性（规格 v2 §2/§3/§4） ---- */
    uint8_t  overflow;              /* OVF_CLIP / VISIBLE / SCROLL */
    uint8_t  anchor;                /* ANC_* 位掩码 */
    int16_t  pct_x, pct_y;         /* 1/1000 父框偏移；0 = 未声明 */
    int16_t  pct_w, pct_h;         /* 1/1000 父框尺寸；0 = 未声明 */
    int16_t  px_x, px_y;           /* 相对锚点/父框的像素偏移 */
    int16_t  min_w, min_h, max_w, max_h;   /* 0 = 不设 */
    int16_t  m_t, m_r, m_b, m_l;   /* 外边距 */
    uint8_t  drag_mode;            /* DRAG_NONE / TITLE / FULL */
    uint8_t  drag_bounds;          /* BOUND_FREE / PARENT / SCREEN */
    uint8_t  resize_dir;           /* RESIZE_* 位掩码；0 = 右下 */
    uint8_t  resize_edge;          /* 边缘带 px，默认 6 */
    int16_t  resize_min_w, resize_min_h;   /* 默认 24 */
    int16_t  resize_max_w, resize_max_h;  /* 0 = 不设 */
    int16_t  resize_step;          /* 0 = 连续 */
    int16_t  scroll_x, scroll_y;    /* OVF_SCROLL 视口偏移，不改子块几何 */

    /* ---- cap2 能力扩展（规格 v3，全部 append-only；0 = 未声明 ≡ 旧行为） ----
     * 语义冻结：字段名/取值不可擅自改名或降级；消费点见 docs/blk-cap2-实现.md。 */
    uint8_t  alpha;                 /* 0=未声明(≡255)；1..255 块不透明度 */
    uint8_t  alpha_mode;            /* 0=ordered16（present 阶段 4x4 Bayer 量化 16 档）；1=none */
    uint8_t  elevation;             /* 0..5 = flat/raised/hover/float/overlay/modal；光源固定右下 */
    uint8_t  corner_tl, corner_tr, corner_br, corner_bl; /* 0..63 px；128=pill；255=auto */
    uint8_t  wrap_lines;            /* 0=不折行；1..6=固定行数；255=auto(≤6) */
    uint8_t  ellipsis;              /* 0=无；1=字符 + "…"；2=词边界 */
    uint8_t  font_size;             /* 0=16(默认)/1=8/2=16/3=24/4=32 */
    uint8_t  font_bold;             /* 0/1：8px 档 8x16 位图；其余 1px 横向膨胀伪粗 */
    uint8_t  font_tabular;          /* 0/1：等宽数字（时钟跳秒不抖） */
    uint8_t  line_h;                /* 0=走令牌默认行高；>0 显式行高 px */
    uint16_t anim_ms;               /* 0=无动画；48/96/144/192/288 */
    uint8_t  anim_ease;             /* 0=none 1=linear 2=in 3=out 4=inout 5=out-back 6=in-back 7=step */
    uint8_t  anim_mask;             /* bit0 x / bit1 y / bit2 w / bit3 h / bit4 alpha / bit5 elevation */
    uint8_t  scrollbar;             /* 0=auto 1=always 2=never（内核统一 track+thumb） */
    uint8_t  scroll_axis;           /* 0=auto(按溢出轴) 1=x 2=y 3=both（HTML data-scroll） */
    uint8_t  border_sides;          /* bit0 l / bit1 r / bit2 t / bit3 b；0=全边（旧行为） */
    uint8_t  hc_force;              /* 1=本子树按 HC 规则渲染（radius=0/border=2，HTML data-theme="hc"） */
    uint32_t anchor_id;             /* BLK_POPOVER 锚块 id（0=无锚，按父框） */
    uint8_t  pop_prefer;            /* 位 up1/down2/left4/right8；0 视为 down|right */
    uint16_t pop_max_h;             /* 0=不限；超出则浮层内部滚动 */
    uint8_t  icon;                  /* BLK_ICON 内置图标号 */
    uint8_t  tint_role;             /* 0=不自着色 1=fg 2=bg 3=accent 4=border */

    /* ---- cap3 角形态扩展（append-only；与 cap2 逐角半径正交） ----
     * 整块四角形态统一，不允许逐角混用（规则 R-7.5-03）。 */
    uint8_t  corner_shape;          /* 0=ARC（默认）1=SQUIRCLE */
    uint8_t  corner_n;              /* 0=默认 n=5.0；仅 SQUIRCLE 有效；其它值钳到 50 + WARN_RADIUS_N_RANGE */

    /* ---- E 轮描边规格（append-only；gui-fix 合入，0=未设） ---- */
    uint8_t  stroke;                /* 描边线宽；desc.c 镜像到 block.stroke，0 时回退 border_w */

    /* ---- v3 按钮图片资源（append-only；0/NULL = 未声明 ≡ 旧行为） ----
     * img=SVG 路径；icon_sz 图标边长（0/16=16，24=24）；icon_pos 预留
     * （0=左，默认；1=右，本轮不启用）。CE ABI 记录槽位未扩展，字段仅内核内耗。 */
    const char   *img;
    uint8_t  icon_sz;
    uint8_t  icon_pos;
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
    uint8_t    moved;                    /* v1 兼容：被拖拽过，布局保留绝对位置 */
    uint8_t    snap;                     /* 本帧快照：布局清位前记录的脏标记 */
    uint8_t    bleed;                    /* 全出血：忽略父级 padding，对齐父级整框 */

    /* ---- v2 声明几何镜像（与 blk_spec 对应） ---- */
    uint8_t  overflow;
    uint8_t  anchor;
    int16_t  pct_x, pct_y, pct_w, pct_h;
    int16_t  px_x, px_y;
    int16_t  min_w, min_h, max_w, max_h;
    int16_t  m_t, m_r, m_b, m_l;
    uint8_t  drag_mode, drag_bounds;
    uint8_t  resize_dir, resize_edge;
    int16_t  resize_min_w, resize_min_h, resize_max_w, resize_max_h, resize_step;
    int16_t  scroll_x, scroll_y;
    int32_t  pos_x, pos_y;              /* 拖拽收敛的偏移属性，跨帧保持 */

    /* ---- cap2 声明镜像（与 blk_spec cap2 段一一对应） ---- */
    uint8_t  alpha;                     /* 本块声明不透明度（1..255） */
    uint8_t  alpha_mode, elevation;
    uint8_t  corner_tl, corner_tr, corner_br, corner_bl;
    uint8_t  wrap_lines, ellipsis, font_size, font_bold, font_tabular, line_h;
    uint16_t anim_ms;
    uint8_t  anim_ease, anim_mask;
    uint8_t  scrollbar, scroll_axis, border_sides, hc_force;
    uint32_t anchor_id;
    uint8_t  pop_prefer;
    uint16_t pop_max_h;
    uint8_t  icon, tint_role;
    uint8_t  corner_shape, corner_n;    /* cap3：0=ARC / 1=SQUIRCLE；n 单位 0.1（50=5.0） */
    /* POPOVER 运行时：最近一次锚点矩形 + 内容自然高（供翻转/滚动范围） */
    int32_t  pop_ax, pop_ay, pop_aw, pop_ah;
    int32_t  pop_content_h;
    uint8_t  stroke;                    /* E 轮描边线宽镜像（append-only） */

    /* ---- v3 按钮图片资源镜像（与 blk_spec 末尾三项同序同步） ---- */
    const char   *img;
    uint8_t  icon_sz, icon_pos;
    const struct svg_icon *ic;          /* 运行时缓存句柄（内部，不入 ABI） */

    /* v2.83 A7.3 位图/画布缩放档位：0=none（1:1 左上，旧行为）；1=contain；2=fill。
     * 内部字段，不入 blk_spec / CE ABI。 */
    uint8_t  fit;

    const char   *text;
    const uint8_t *pix;
    int32_t    pix_w, pix_h;
    uint8_t    pix_opaque_only;          /* v1 只支持不透明位图 */

    struct style st;

    void (*on_click)(block_id id, int32_t lx, int32_t ly);
    void (*on_drag )(block_id id, int32_t dx, int32_t dy, uint8_t phase);
    void (*on_dblclick)(block_id id, int32_t lx, int32_t ly);
    /* v2.83: 1 = 松开且仍命中同块才回调 on_click（按钮/菜单行/开始菜单行）；
     * 0 = 保持旧的按下即触发（画布/可拖面板等，本轮不推广）。 */
    uint8_t  click_on_up;
    /* v2.84 P0-2: 1 = 图标先扫源 16×16 墨迹 bbox，再最近邻撑满块框（桌面条目）；
     * 0 = 保持旧整格 2× 居中（标题栏/菜单/对话框图标复用同绘制，像素不变）。
     * 内部字段，不入 blk_spec / CE ABI。 */
    uint8_t  icon_fit;
    void (*render)(block_id id);
    void  *usr;
    uint8_t *buf;                       /* BLK_CANVAS 运行时缓冲（24bpp） */

    /* ---- 内部字段（不对外） ---- */
    struct block *free_next;             /* 空闲链 */
    uint8_t  desc_seen;                  /* commit diff 本轮是否见过 */
    uint8_t  owner;                      /* 预留：0=内核，>0=pid；v1 恒 0 */

    /* 脏矩形旧矩形缓存：几何变更前记下旧矩形，供损伤收集算「旧 ∪ 新」。
     * has_old=0 时以下四个值无效。收集进 damage 后即清零。 */
    int32_t  old_x, old_y, old_w, old_h;
    uint8_t  has_old;
} block;

#endif /* CATOS_GUI_BLOCK_H */
