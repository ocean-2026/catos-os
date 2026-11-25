/*
 * tokens.c -- 三套令牌取值（规格 §6）。
 *
 * v1 无 radius / elevation / 动画：字段置 0，绘制路径不产生任何
 * 圆角、阴影或过渡效果。
 */
#include "gui/tokens.h"

static const struct style tok_dark = {
    .fg = 0xE8E8ECu, .bg = 0x1E1E26u, .accent = 0x4C8DFFu,
    .panel = 0x2A2A34u, .bar = 0x3A3A48u, .border = 0x555A66u, .line = 0x555A66u,
    .scale = 1, .pad = 0, .border_w = 1,
    .radius = 0, .elevation = 0, .anim_ms = 0,
    .radius4 = { 8, 8, 8, 8 },
    .shadow = { 0, 28, 44, 64, 88, 120 },
    .spacing = 8,
    .font_small = 8, .font_body = 16, .font_title = 24, .font_display = 32,
    .line_h = 18, .radius_hc_zero = 0, .border_w_hc = 2
};
static const struct style tok_light = {
    .fg = 0x181820u, .bg = 0xECECF1u, .accent = 0x2563D8u,
    .panel = 0xDDDDE6u, .bar = 0xC8C8D6u, .border = 0x8A8F9Eu, .line = 0x8A8F9Eu,
    .scale = 1, .pad = 0, .border_w = 1,
    .radius = 0, .elevation = 0, .anim_ms = 0,
    .radius4 = { 8, 8, 8, 8 },
    .shadow = { 0, 24, 40, 56, 80, 112 },
    .spacing = 8,
    .font_small = 8, .font_body = 16, .font_title = 24, .font_display = 32,
    .line_h = 18, .radius_hc_zero = 0, .border_w_hc = 2
};
static const struct style tok_hc = {
    .fg = 0xFFFFFFu, .bg = 0x000000u, .accent = 0xFFFF00u,
    .panel = 0x101010u, .bar = 0x202020u, .border = 0xFFFF00u, .line = 0xFFFF00u,
    .scale = 1, .pad = 0, .border_w = 2,
    .radius = 0, .elevation = 0, .anim_ms = 0,
    .radius4 = { 0, 0, 0, 0 },
    .shadow = { 0, 0, 0, 0, 0, 0 },
    .spacing = 8,
    .font_small = 8, .font_body = 16, .font_title = 24, .font_display = 32,
    .line_h = 18, .radius_hc_zero = 1, .border_w_hc = 2
};

const struct style *gui_token_style(enum gui_token t) {
    switch (t) {
    case TOK_DARK:           return &tok_dark;
    case TOK_LIGHT:          return &tok_light;
    case TOK_HIGH_CONTRAST:  return &tok_hc;
    default:                 return &tok_dark;
    }
}

/* 当前生效主题；绘制路径一律取这里，不允许在 UI 路径写死 RGB。 */
static const struct style *active_style = &tok_dark;

const struct style *gui_active_style(void) { return active_style; }

void gui_set_token(enum gui_token t) {
    active_style = gui_token_style(t);
}
