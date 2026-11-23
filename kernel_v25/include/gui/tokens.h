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
    uint32_t fg, bg, accent;   /* 0x00RRGGBB */
    uint8_t  scale;            /* 字缩放 1 / 2 */
    uint8_t  pad;              /* 令牌级 padding（v1 未参与布局，保留） */
    uint8_t  border_w;         /* 0 / 1 / 2 */
    /* ---- v1 预留字段，实现无效果 ---- */
    uint8_t  radius;           /* v1: 恒 0，直角 */
    uint8_t  elevation;        /* v1: 恒 0，无阴影 */
    uint16_t anim_ms;          /* v1: 恒 0，无动画 */
};

enum gui_token {
    TOK_DARK = 0,
    TOK_LIGHT,
    TOK_HIGH_CONTRAST,
    TOK_NTOK
};

const struct style *gui_token_style(enum gui_token t);
const struct style *gui_active_style(void);
void      gui_set_token(enum gui_token t);

#endif /* CATOS_GUI_TOKENS_H */
