/*
 * shadows.c -- cap2 elevation 0..5 九宫阴影（9-slice，光源固定右下）。
 *
 * 启动期按档位预生成角部掩码与边缘衰减剖面（分档，不逐像素算距离）；
 * 绘制时 9 切片拼缝共用同一剖面，缝 0px。强度取活动令牌 shadow[el]；
 * HC 主题令牌 shadow 全 0，改用描边替代阴影。
 *
 * 静态占用：6 档 × 10×10 角掩码 + 6 档 × 10 边缘剖面 ≈ 0.7KiB，未越 4KiB 红线。
 */
#include "gui/block.h"
#include "gui/tokens.h"
#include "gui_int.h"
#include "gfx24.h"

extern void gui_draw_border(int x, int y, int w, int h, uint32_t color,
                            uint8_t bw);

#define SH_MAX_PAD 10

/* 各档外扩半径 / 右下偏移（光源固定 x+ / y+）。 */
static const int SH_PAD[6] = { 0, 2, 3, 5, 7, SH_MAX_PAD };
static const int SH_OX[6]  = { 0, 1, 2, 3, 4, 6 };
static const int SH_OY[6]  = { 0, 1, 2, 3, 4, 6 };

static uint8_t sh_corner[6][SH_MAX_PAD][SH_MAX_PAD];
static uint8_t sh_edge[6][SH_MAX_PAD];
static int     sh_ready;

static void sh_init(void) {
    if (sh_ready) return;
    for (int e = 0; e < 6; e++) {
        int pad = SH_PAD[e];
        for (int d = 0; d < pad; d++) {
            /* 内边（贴块体）255，外缘 0；线性衰减。 */
            sh_edge[e][d] = (uint8_t)(pad ? 255u * (uint32_t)(pad - d) / (uint32_t)pad : 0);
            for (int c = 0; c < pad; c++) {
                int dist = d > c ? d : c;
                sh_corner[e][d][c] =
                    (uint8_t)(pad ? 255u * (uint32_t)(pad - dist) / (uint32_t)pad : 0);
            }
        }
    }
    sh_ready = 1;
}

static void sh_blend(int x, int y, uint32_t str) {
    if (x < 0 || y < 0 || x >= GFX24_W || y >= GFX24_H) return;
    /* 阴影色 = 黑；覆盖率 = 剖面 × 令牌强度。 */
    uint32_t cov = str;
    if (cov == 0) return;
    if (cov >= 255) { gfx24_putpx(x, y, 0x000000u); return; }
    uint32_t under = gfx24_getpx(x, y);
    int ia = 255 - (int)cov;
    int r = (int)(under >> 16 & 0xFF) * ia / 255;
    int g = (int)(under >>  8 & 0xFF) * ia / 255;
    int b = (int)(under      & 0xFF) * ia / 255;
    gfx24_putpx(x, y, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

void sh_paint(block *b, uint8_t elevation) {
    if (!b || b->w <= 0 || b->h <= 0) return;
    if (b->kind == BLK_ROOT || b->bleed) return;
    const struct style *tok = gui_active_style();

    /* 防御兜底：值域校验已在 CE 提交期 validate2 拒绝 0..5 之外的档位；
     * 这里再 clamp 一次，防止内核内旧路径 / 残留字段把越界档带进索引表，
     * 避免 shadow[el]/SH_PAD[el]/SH_OX[el] 越界读。 */
    if (elevation > 5) elevation = 5;

    int el = elevation;
    if (b->focused && el < 5) el++;            /* 焦点自动 +1 档，上限 5 */

    /* HC：无阴影，用描边替代。 */
    if (tok->radius_hc_zero || b->hc_force) {
        if (el > 0)
            gui_draw_border(b->x, b->y, b->w, b->h, tok->border,
                            tok->border_w_hc ? tok->border_w_hc : 2);
        return;
    }

    uint32_t str = tok->shadow[el];
    if (!str) return;
    sh_init();

    int pad = SH_PAD[el], ox = SH_OX[el], oy = SH_OY[el];
    if (pad <= 0) return;
    int sx = b->x + ox, sy = b->y + oy;
    int sw = b->w, sh = b->h;

    /* 强度缩放：剖面 0..255 × 令牌强度 0..255。 */
#define SH_STR(d, c) ((uint32_t)sh_edge[el][d] * str / 255u)
#define SH_STRC(d, c) ((uint32_t)sh_corner[el][d][c] * str / 255u)

    /* 上下边带（不含角）。 */
    for (int d = 0; d < pad; d++) {
        uint32_t e = SH_STR(d, 0);
        for (int x = sx; x < sx + sw; x++) {
            sh_blend(x, sy - pad + d, e);          /* 顶边 */
            sh_blend(x, sy + sh + d,  e);          /* 底边 */
        }
        for (int y = sy; y < sy + sh; y++) {
            sh_blend(sx - pad + d, y, e);          /* 左边 */
            sh_blend(sx + sw + d,  y, e);          /* 右边 */
        }
    }
    /* 四角 9-slice 掩码。 */
    for (int dy = 0; dy < pad; dy++) {
        for (int dx = 0; dx < pad; dx++) {
            uint32_t c = SH_STRC(dy, dx);
            sh_blend(sx - pad + dx, sy - pad + dy, c);            /* TL */
            sh_blend(sx + sw + pad - 1 - dx, sy - pad + dy, c);   /* TR */
            sh_blend(sx - pad + dx, sy + sh + pad - 1 - dy, c);  /* BL */
            sh_blend(sx + sw + pad - 1 - dx, sy + sh + pad - 1 - dy, c); /* BR */
        }
    }
#undef SH_STR
#undef SH_STRC
}
