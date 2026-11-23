/*
 * tokens.c -- 三套令牌取值（规格 §6）。
 *
 * v1 无 radius / elevation / 动画：字段置 0，绘制路径不产生任何
 * 圆角、阴影或过渡效果。
 */
#include "gui/tokens.h"

static const struct style tok_dark = {
    .fg = 0xE8E8ECu, .bg = 0x1E1E26u, .accent = 0x4C8DFFu,
    .scale = 1, .pad = 0, .border_w = 1,
    .radius = 0, .elevation = 0, .anim_ms = 0
};
static const struct style tok_light = {
    .fg = 0x181820u, .bg = 0xECECF1u, .accent = 0x2563D8u,
    .scale = 1, .pad = 0, .border_w = 1,
    .radius = 0, .elevation = 0, .anim_ms = 0
};
static const struct style tok_hc = {
    .fg = 0xFFFFFFu, .bg = 0x000000u, .accent = 0xFFFF00u,
    .scale = 1, .pad = 0, .border_w = 2,
    .radius = 0, .elevation = 0, .anim_ms = 0
};

const struct style *gui_token_style(enum gui_token t) {
    switch (t) {
    case TOK_DARK:           return &tok_dark;
    case TOK_LIGHT:          return &tok_light;
    case TOK_HIGH_CONTRAST:  return &tok_hc;
    default:                 return &tok_dark;
    }
}
