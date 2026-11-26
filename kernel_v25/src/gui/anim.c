/*
 * anim.c -- cap2 固定缓动动画：16ms tick 预生成帧表 + 6 属性独立插值 + 可中断。
 *
 * 口径（规格 cap2 §B）：
 *   - 7 种缓动：1 linear / 2 in / 3 out / 4 inout / 5 out-back /
 *     6 in-back / 7 step；0 = none（零开销，像素不变）。
 *   - 时长档 48/96/144/192/288 ms；帧表按 19 帧上限预生成，整数千分比，
 *     过冲 / 回弹允许帧值越出 0..1000，但末帧必须精确落 1000。
 *   - anim_mask 6 属性各自独立：bit0 x / bit1 y / bit2 w / bit3 h /
 *     bit4 alpha / bit5 elevation（elevation 整档取最近档）。
 *   - 可中断：新目标到来时 anim_kick 接管，起点 = 当前帧值；到点精确落目标，
 *     无残帧。anim_ms==0 时本模块零开销。
 *   - reduced_motion 全局开关：开启后所有动画一帧直达目标。
 *
 * 实现约束：整数运算，无浮点 / SSE；槽表与帧表总体积 < 2 KiB。
 */
#include "gui/block.h"
#include "gui_int.h"

#define ANIM_SLOTS   16
#define ANIM_MAX_IDX 18
#define ANIM_ELEV_MAX 5

typedef struct {
    block_id  id;
    uint16_t  ms;
    uint16_t  elapsed;
    uint8_t   ease, mask;
    uint8_t   active;
    uint8_t   pending;          /* 目标待布局后首次 apply 采集 */
    int32_t   sx, sy, sw, sh;  /* 起始几何（当前帧值） */
    int32_t   tx, ty, tw, th;  /* 目标几何（布局结果） */
    uint8_t   sa, ta, se, te;   /* 起始 / 目标 alpha、elevation */
} anim_slot_t;

static anim_slot_t slots[ANIM_SLOTS];
static uint8_t     reduced_motion;
static int16_t     ease_tab[8][ANIM_MAX_IDX + 1];
static int         tab_built;

/* ---- 帧表：千分比曲线，启动期一次性预生成 -------------------------------- */

static void build_table(void) {
    if (tab_built) return;
    for (int i = 0; i <= ANIM_MAX_IDX; i++) {
        int t  = i * 1000 / ANIM_MAX_IDX;      /* 0..1000 */
        int u  = t - 1000;                     /* -1000..0 */
        int t2 = t * t / 1000;
        int u2 = u * u / 1000;
        int u3 = u * u * u / 1000000;
        ease_tab[1][i] = (int16_t)t;
        ease_tab[2][i] = (int16_t)t2;
        ease_tab[3][i] = (int16_t)(1000 - u2);
        ease_tab[4][i] = (int16_t)(t < 500 ? 2 * t2 : 1000 - 2 * u2);
        /* out-back / in-back：c1=1.702，c3=2.703（千分比系数）。 */
        ease_tab[5][i] = (int16_t)(1000 + 2703 * u3 / 1000 + 1702 * u2 / 1000);
        ease_tab[6][i] = (int16_t)(2703 * u3 / 1000 + 1702 * u2 / 1000);
        ease_tab[7][i] = (int16_t)(i == ANIM_MAX_IDX ? 1000 : 0);
    }
    tab_built = 1;
}

/* ---- 槽位 --------------------------------------------------------------- */

static anim_slot_t *find_slot(block_id id, int alloc) {
    for (int i = 0; i < ANIM_SLOTS; i++)
        if (slots[i].active && slots[i].id == id) return &slots[i];
    if (!alloc) return 0;
    for (int i = 0; i < ANIM_SLOTS; i++)
        if (!slots[i].active) return &slots[i];
    /* 槽满：覆写已推进最久的一条（可中断语义下旧动画直接让位）。 */
    anim_slot_t *old = &slots[0];
    for (int i = 1; i < ANIM_SLOTS; i++)
        if (slots[i].elapsed > old->elapsed) old = &slots[i];
    return old;
}

static int32_t lerp_i(int32_t s, int32_t t, int v1000) {
    int32_t diff = t - s;
    return s + diff * v1000 / 1000;
}

void anim_kick(block *b) {
    if (!b) return;
    /* 0 = 无动画；未声明动画属性时零开销，不建档。 */
    if (!b->anim_ms || !b->anim_mask || b->anim_ease == 0) return;
    if (reduced_motion) return;                 /* 直达目标，不建档 */
    anim_slot_t *s = find_slot(b->id, 1);
    if (!s) return;
    s->id      = b->id;
    s->active  = 1;
    s->pending = 1;
    s->ms      = b->anim_ms;
    s->elapsed = 0;
    s->ease    = b->anim_ease;
    s->mask    = b->anim_mask;
    /* 起点 = 当前帧值（可中断接管：不从旧动画半途卡死）。 */
    s->sx = b->x; s->sy = b->y; s->sw = b->w; s->sh = b->h;
    s->sa = b->alpha ? b->alpha : 255;
    s->se = b->elevation;
}

void anim_tick(uint16_t dt_ms) {
    if (reduced_motion) return;
    build_table();
    for (int i = 0; i < ANIM_SLOTS; i++) {
        if (!slots[i].active) continue;
        uint32_t e = (uint32_t)slots[i].elapsed + dt_ms;
        slots[i].elapsed = e > 65535 ? 65535 : (uint16_t)e;
    }
}

void anim_apply(block *b) {
    if (!b) return;
    anim_slot_t *s = find_slot(b->id, 0);
    if (!s) return;

    if (s->pending) {
        /* 布局后采集目标：几何 / alpha / elevation 以本帧布局结果为准。 */
        s->tx = b->x; s->ty = b->y; s->tw = b->w; s->th = b->h;
        s->ta = b->alpha ? b->alpha : 255;
        s->te = b->elevation;
        s->pending = 0;
    }

    int done = reduced_motion || s->elapsed >= s->ms;
    int v = 1000;
    if (!done) {
        int frac = (int)s->elapsed * 1000 / (int)s->ms;
        if (frac > 1000) frac = 1000;
        int idx = frac * ANIM_MAX_IDX / 1000;
        v = ease_tab[s->ease][idx];
    }

    if (done) {
        /* 到点精确落目标：无残帧。 */
        b->x = s->tx; b->y = s->ty; b->w = s->tw; b->h = s->th;
        b->alpha = s->ta; b->elevation = s->te;
        s->active = 0;
    } else {
        if (s->mask & 1)  b->x = lerp_i(s->sx, s->tx, v);
        if (s->mask & 2)  b->y = lerp_i(s->sy, s->ty, v);
        if (s->mask & 4)  b->w = lerp_i(s->sw, s->tw, v);
        if (s->mask & 8)  b->h = lerp_i(s->sh, s->th, v);
        if (s->mask & 16) b->alpha = (uint8_t)lerp_i(s->sa, s->ta, v);
        if (s->mask & 32) {
            /* elevation 整档切换：不插值，过半切到目标档，取最近整档。 */
            int e = v >= 500 ? s->te : s->se;
            if (e < 0) e = 0;
            if (e > ANIM_ELEV_MAX) e = ANIM_ELEV_MAX;
            b->elevation = (uint8_t)e;
        }
    }
    /* 只标脏绘制：动画期间块几何以帧值为准，不参与父级重排纠正。 */
    b->dirty_paint = 1;
}

void anim_reduced(int on) {
    reduced_motion = on ? 1 : 0;
    if (reduced_motion) {
        /* 开关落下：在途动画全部作废，下一帧布局值即终态。 */
        for (int i = 0; i < ANIM_SLOTS; i++) slots[i].active = 0;
    }
}

int anim_busy(void) {
    for (int i = 0; i < ANIM_SLOTS; i++)
        if (slots[i].active) return 1;
    return 0;
}

/* 会话复位：gui_begin 重建块表后清空在途槽，避免旧 id 复用误动画。 */
void anim_reset(void) {
    for (int i = 0; i < ANIM_SLOTS; i++) slots[i].active = 0;
    reduced_motion = 0;
}
