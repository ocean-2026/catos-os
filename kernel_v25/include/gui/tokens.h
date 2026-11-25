#ifndef CATOS_GUI_TOKENS_H
#define CATOS_GUI_TOKENS_H

/*
 * tokens.h -- 块化 GUI 样式令牌（规格 §6）。
 *
 * v1 砍项：圆角 / 阴影 / 半透明 / 变换动画。下列字段保留在结构里，
 * 但实现必须是「无效果」：radius 恒 0（直角）、elevation 恒 0（无阴影）、
 * anim_ms 恒 0（无动画）。不许在绘制路径里假承诺这些效果。
 */
#include <stdint.h>

struct style {
    uint32_t fg, bg, accent;   /* 0x00RRGGBB；块级显式色，0 = 未显式指定 */
    uint32_t panel;            /* 应用块底色（区别于桌面 bg） */
    uint32_t bar;              /* 标题栏 / 任务栏 / 菜单底色 */
    uint32_t border;           /* 块描边色 */
    uint32_t line;             /* 分隔线色 */
    /* 语义角色：非 0 表示该色跟随活动令牌的对应角色，不把具体 RGB
     * 烧进块；主题切换时整树重解析，无需清除固化色。 */
    uint8_t  role_fg, role_bg, role_accent;
    uint8_t  scale;            /* 字缩放 1 / 2 */
    uint8_t  pad;              /* 令牌级 padding（v1 未参与布局，保留） */
    uint8_t  border_w;         /* 0 / 1 / 2 */
    /* ---- v1 预留字段，实现无效果 ---- */
    uint8_t  radius;           /* v1: 恒 0，直角 */
    uint8_t  elevation;        /* v1: 恒 0，无阴影 */
    uint16_t anim_ms;          /* v1: 恒 0，无动画 */
    /* ---- 桌面外观档位（calm 模式消费；app 模式下不生效） ---- */
    uint8_t  corner_radius;    /* 0 = 按块尺寸走阶梯；>0 固定角档 px */
    uint8_t  corner_mode;      /* 0 继承模式 / 1 强制直角 / 2 强制圆角 */
    /* ---- cap2：几何/阴影/字体进令牌（三主题各自取值；HC 自动降级） ----
     * radius[] 缺省角档 TL TR BR BL；shadow[0..5] 各 elevation 档强度；
     * spacing 为 8pt 栅格基数；font_* 为四档字高；line_h 是默认行高。 */
    uint8_t  radius4[4];       /* 缺省角档 TL TR BR BL（文档里的 radius[4]，避开旧 radius 单值）；HC 恒 0 */
    uint8_t  shadow[6];        /* 0..5 档；0=无阴影；HC 全 0（用 1px 边框替代） */
    uint8_t  spacing;
    uint8_t  font_small;       /* 8 */
    uint8_t  font_body;        /* 16 */
    uint8_t  font_title;       /* 24 */
    uint8_t  font_display;     /* 32 */
    uint8_t  line_h;           /* 16 字高默认行高 */
    uint8_t  radius_hc_zero;   /* 1 = 本主题强制直角（HC） */
    uint8_t  border_w_hc;      /* HC 下描边宽度（2） */
};

enum gui_token {
    TOK_DARK = 0,
    TOK_LIGHT,
    TOK_HIGH_CONTRAST,
    TOK_NTOK
};

/* 语义着色角色：块按角色跟随活动令牌，而不是固化当时的 RGB。 */
enum gui_role {
    ROLE_NONE = 0,
    ROLE_FG,
    ROLE_BG,
    ROLE_PANEL,
    ROLE_BAR,
    ROLE_ACCENT,
    ROLE_BORDER,
    ROLE_LINE
};

const struct style *gui_token_style(enum gui_token t);
const struct style *gui_active_style(void);
void      gui_set_token(enum gui_token t);

#endif /* CATOS_GUI_TOKENS_H */
