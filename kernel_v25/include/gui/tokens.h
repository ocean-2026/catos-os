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
