/*
 * compose.c -- 帧调度、damage 合并、统计打点（规格 §4 / §5 / §9）。
 *
 * 节拍：PIT 100Hz，同一 tick 内重复 gui_frame() 只算一次提交
 * （stat_double_commit）。damage 矩形上限 16 个，超出退化为包围盒；
 * 只合成脏区，禁止整帧重绘。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gfx24.h"
#include "render.h"
#include "render_be.h"
#include "mouse.h"
#include "kstring.h"

extern volatile uint32_t timer_ticks;
extern uint64_t perf_now(void);
extern uint32_t perf_mhz(void);

/* 离屏影子缓冲内部接口（be_vbe24.c；kernel 内部，不进 CE ABI）。 */
extern int  vbe24_shadow_init(void);
extern void vbe24_shadow_sink(int on);
extern void vbe24_shadow_blit_rect(int x, int y, int w, int h);
extern int  vbe24_shadow_mode(void);
static int s_shadow_ready;

#define DMG_MAX 16
static struct { int x, y, w, h; } dmg[DMG_MAX];
static int dmg_n;
static int dmg_full;

/* cap2 阴影外扩保守量：6 档最大 pad=10，右下光源偏移 ox/oy 最大 6。
 * 贴边/成组紧贴的块本就不投影（paint.c flush_edges），这里按最大外扩留边，
 * 宁多纳一条窄边，不把阴影像素留在损伤区外。 */
#define HALO_PAD_L 10
#define HALO_PAD_T 10
#define HALO_PAD_R 16
#define HALO_PAD_B 16

static uint32_t s_frames, s_layout_nodes, s_double_commit;
static uint32_t s_damage_rects, s_damage_area;
static uint64_t s_frame_cycles;
static uint32_t s_last_us;
static uint32_t s_last_tick;

/* ---- damage 合并 -------------------------------------------------------- */

static int rects_overlap(int ax, int ay, int aw, int ah,
                         int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void dmg_reset(void) { dmg_n = 0; dmg_full = 0; }

static void dmg_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dmg_full) return;
    for (int i = 0; i < dmg_n; i++) {
        if (rects_overlap(x, y, w, h, dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h)) {
            int x0 = x < dmg[i].x ? x : dmg[i].x;
            int y0 = y < dmg[i].y ? y : dmg[i].y;
            int x1 = x + w > dmg[i].x + dmg[i].w ? x + w : dmg[i].x + dmg[i].w;
            int y1 = y + h > dmg[i].y + dmg[i].h ? y + h : dmg[i].y + dmg[i].h;
            dmg[i].x = x0; dmg[i].y = y0; dmg[i].w = x1 - x0; dmg[i].h = y1 - y0;
            return;
        }
    }
    if (dmg_n < DMG_MAX) {
        dmg[dmg_n].x = x; dmg[dmg_n].y = y;
        dmg[dmg_n].w = w; dmg[dmg_n].h = h;
        dmg_n++;
        return;
    }
    /* 超出上限：退化为全部已有 damage 的包围盒。 */
    int x0 = dmg[0].x, y0 = dmg[0].y;
    int x1 = x0 + dmg[0].w, y1 = y0 + dmg[0].h;
    for (int i = 1; i < dmg_n; i++) {
        if (dmg[i].x < x0) x0 = dmg[i].x;
        if (dmg[i].y < y0) y0 = dmg[i].y;
        if (dmg[i].x + dmg[i].w > x1) x1 = dmg[i].x + dmg[i].w;
        if (dmg[i].y + dmg[i].h > y1) y1 = dmg[i].y + dmg[i].h;
    }
    dmg[0].x = x0; dmg[0].y = y0; dmg[0].w = x1 - x0; dmg[0].h = y1 - y0;
    dmg_n = 1;
    dmg_full = 1;
}

/* 子树绝对坐标包围盒（含 OVF_VISIBLE 逃逸子块；阴影外扩由 dmg_add_pad 加）。 */
static void subtree_bbox(block *b, int *x0, int *y0, int *x1, int *y1) {
    if (!b) return;
    if (b->x < *x0) *x0 = b->x;
    if (b->y < *y0) *y0 = b->y;
    if (b->x + b->w > *x1) *x1 = b->x + b->w;
    if (b->y + b->h > *y1) *y1 = b->y + b->h;
    for (block *c = b->first_child; c; c = c->next_sibling)
        subtree_bbox(c, x0, y0, x1, y1);
}

/* 带阴影外扩地纳入损伤，并裁到屏内。 */
static void dmg_add_pad(int x, int y, int w, int h) {
    int x0 = x - HALO_PAD_L, y0 = y - HALO_PAD_T;
    int x1 = x + w + HALO_PAD_R, y1 = y + h + HALO_PAD_B;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GFX24_W) x1 = GFX24_W;
    if (y1 > GFX24_H) y1 = GFX24_H;
    dmg_add(x0, y0, x1 - x0, y1 - y0);
}

/* 两矩形是否重叠或 1px 邻接（可并成一条带）。 */
static int dmg_touch(int i, int j) {
    int ax = dmg[i].x, ay = dmg[i].y, aw = dmg[i].w, ah = dmg[i].h;
    int bx = dmg[j].x, by = dmg[j].y, bw = dmg[j].w, bh = dmg[j].h;
    int xov = ax < bx + bw + 1 && bx < ax + aw + 1;
    int yov = ay < by + bh + 1 && by < ay + ah + 1;
    return xov && yov;
}

static void dmg_merge_into(int i, int j) {
    int x0 = dmg[i].x < dmg[j].x ? dmg[i].x : dmg[j].x;
    int y0 = dmg[i].y < dmg[j].y ? dmg[i].y : dmg[j].y;
    int x1 = dmg[i].x + dmg[i].w > dmg[j].x + dmg[j].w
             ? dmg[i].x + dmg[i].w : dmg[j].x + dmg[j].w;
    int y1 = dmg[i].y + dmg[i].h > dmg[j].y + dmg[j].h
             ? dmg[i].y + dmg[i].h : dmg[j].y + dmg[j].h;
    dmg[i].x = x0; dmg[i].y = y0; dmg[i].w = x1 - x0; dmg[i].h = y1 - y0;
    dmg[j] = dmg[dmg_n - 1];
    dmg_n--;
}

/* 区域合并：先两两 union 收敛，再做水平扫描带（同带内 y 交叠的相邻
 * 条带并成一条），减少上报给 perf 的小矩形数。 */
static void dmg_coalesce(void) {
    int changed = 1;
    while (changed && dmg_n > 1) {
        changed = 0;
        for (int i = 0; i < dmg_n && !changed; i++) {
            for (int j = i + 1; j < dmg_n; j++) {
                if (dmg_touch(i, j)) { dmg_merge_into(i, j); changed = 1; break; }
            }
        }
    }
}

/* ---- 绘制顺序收集：layer 升序 → z 升序 → 树先序（稳定） --------------- */

#define GUI_PAINT_MAX 256
static block *paint_list[GUI_PAINT_MAX];
static int paint_count;

static void anim_apply_walk(block *b) {
    if (!b) return;
    anim_apply(b);
    for (block *c = b->first_child; c; c = c->next_sibling)
        anim_apply_walk(c);
}

static void collect(block *b) {
    if (!b) return;
    if (b->visible && paint_count < GUI_PAINT_MAX)
        paint_list[paint_count++] = b;
    for (block *c = b->first_child; c; c = c->next_sibling)
        collect(c);
}

static void sort_paint(void) {
    /* 稳定插入排序：layer 升序、z 升序；先序次序作为平局保留。 */
    for (int i = 1; i < paint_count; i++) {
        block *key = paint_list[i];
        int j = i - 1;
        while (j >= 0 &&
               (paint_list[j]->layer > key->layer ||
                (paint_list[j]->layer == key->layer &&
                 paint_list[j]->z > key->z))) {
            paint_list[j + 1] = paint_list[j];
            j--;
        }
        paint_list[j + 1] = key;
    }
}

extern void ser_write(const char *s);
extern void ser_putc(char c);

/* anim.c 会话复位（gui_int.h 冻结清单之外的内部入口）。 */
extern void anim_reset(void);

static void ser_dec_u(uint32_t v) {
    char buf[12]; int n = 0;
    if (v == 0) { ser_write("0"); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(buf[--n]);
}

/* ---- 统计 --------------------------------------------------------------- */

void stat_set_layout_nodes(uint32_t n) { s_layout_nodes = n; }
void stat_add_layout_nodes(uint32_t n)  { s_layout_nodes += n; }
void stat_note_double_commit(void)      { s_double_commit++; }

void stat_note_frame(uint64_t cycles, uint32_t rects, uint32_t area) {
    s_frame_cycles = cycles;
    s_damage_rects = rects;
    s_damage_area = area;
    uint32_t mhz = perf_mhz();
    /* freestanding 无 64 位除法：取低 32 位周期做 32 位换算。 */
    s_last_us = mhz ? (uint32_t)((uint32_t)cycles / mhz) : 0;
}

uint32_t stat_frames(void)          { return s_frames; }
uint32_t stat_layout_nodes(void)    { return s_layout_nodes; }
uint32_t stat_blocks_live(void)     { return btable_live(); }
uint32_t stat_blocks_leaked(void)   {
    uint32_t live = btable_live();
    uint32_t tree = tree_count_nodes();
    return live > tree ? live - tree : 0;
}
uint32_t stat_double_commit(void)    { return s_double_commit; }
uint32_t stat_damage_rects(void)     { return s_damage_rects; }
uint32_t stat_damage_area(void)      { return s_damage_area; }
uint64_t stat_frame_cycles(void)    { return s_frame_cycles; }
uint32_t stat_last_frame_us(void)   { return s_last_us; }

/* ---- 第 2a 轮：帧性能观测环形缓冲（只测量，不做优化） ------------------ */

#define PERF_RING 32
static uint32_t perf_on = 1;
static uint32_t perf_us[PERF_RING];
static uint32_t perf_area[PERF_RING];   /* 本帧 damage 合并区域覆盖像素数（触碰写像素估计） */
static uint32_t perf_rects[PERF_RING];  /* 本帧 damage 合并后区域数 */
static uint32_t perf_n, perf_head;

static void perf_record(uint32_t us, uint32_t area, uint32_t rects) {
    if (!perf_on) return;
    perf_us[perf_head] = us;
    perf_area[perf_head] = area;
    perf_rects[perf_head] = rects;
    perf_head = (perf_head + 1) % PERF_RING;
    if (perf_n < PERF_RING) perf_n++;
}

static uint32_t perf_sorted_us[PERF_RING];

static uint32_t perf_sort_us(void) {
    for (uint32_t i = 0; i < perf_n; i++) perf_sorted_us[i] = perf_us[i];
    for (uint32_t i = 1; i < perf_n; i++) {
        uint32_t key = perf_sorted_us[i];
        int j = (int)i - 1;
        while (j >= 0 && perf_sorted_us[j] > key) {
            perf_sorted_us[j + 1] = perf_sorted_us[j];
            j--;
        }
        perf_sorted_us[j + 1] = key;
    }
    return perf_n;
}

static int perf_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

void gui_perf_set(const char *op) {
    if (!op || !*op) { gui_perf_dump(); return; }
    if (perf_eq(op, "on"))    { perf_on = 1; return; }
    if (perf_eq(op, "off"))   { perf_on = 0; return; }
    if (perf_eq(op, "reset")) { perf_n = 0; perf_head = 0; perf_on = 1; return; }
}

void gui_perf_dump(void) {
    if (perf_n == 0) {
        ser_write("GUI:perf n=0\n");
        return;
    }
    uint32_t sum_us = 0, sum_area = 0, sum_rect = 0;
    for (uint32_t i = 0; i < perf_n; i++) {
        sum_us += perf_us[i]; sum_area += perf_area[i]; sum_rect += perf_rects[i];
    }
    uint32_t n = perf_sort_us();
    uint32_t avg_us = (uint32_t)(sum_us / n);
    uint32_t med_us = perf_sorted_us[n / 2];
    uint32_t p95_us = perf_sorted_us[(n * 95) / 100];
    uint32_t avg_area = (uint32_t)(sum_area / n);
    uint32_t avg_rect = (uint32_t)(sum_rect / n);
    ser_write("GUI:perf n="); ser_dec_u(n);
    ser_write(" us_avg="); ser_dec_u(avg_us);
    ser_write(" us_median="); ser_dec_u(med_us);
    ser_write(" us_p95="); ser_dec_u(p95_us);
    ser_write(" px_avg="); ser_dec_u(avg_area);
    ser_write(" rects_avg="); ser_dec_u(avg_rect);
    ser_write("\n");
}

/* ---- 帧 ----------------------------------------------------------------- */

void gui_frame(void) {
    if (timer_ticks == s_last_tick) {
        stat_note_double_commit();
        return;
    }
    s_last_tick = timer_ticks;

    uint64_t t0 = perf_now();

    paint_count = 0;
    collect(gui_root_block());
    sort_paint();

    /* 布局清位前先快照本帧脏块；布局阶段会清 dirty_layout，绘制以快照为准。 */
    for (int i = 0; i < paint_count; i++)
        paint_list[i]->snap =
            (paint_list[i]->dirty_layout || paint_list[i]->dirty_paint) ? 1 : 0;

    /* 动画节拍：tick 推进在布局前；布局后、绘制前把当前帧值写回块几何。
     * 动画期间该块几何以动画帧为准，不参与父级重排纠正。 */
    anim_tick(16);
    layout_run();
    anim_apply_walk(gui_root_block());

    dmg_reset();
    paint_begin_frame();

    /* 第一遍：收集脏块子树包围盒，合并成 damage 链表。
     * 移动损伤 = 旧子树包围盒 ∪ 新子树包围盒（遍历子树取绝对坐标并集，
     * 含逃逸子块与阴影外扩边）；子树随父刚性平移，旧包围盒由新包围盒按
     * 父块位移回推。OVF_VISIBLE 子块脏区并入父块，旧位置由父底色补齐。 */
    for (int i = 0; i < paint_count; i++) {
        block *b = paint_list[i];
        if (!b->snap) continue;
        int x0 = GFX24_W, y0 = GFX24_H, x1 = 0, y1 = 0;
        subtree_bbox(b, &x0, &y0, &x1, &y1);
        if (b->has_old) {
            int dx = b->old_x - b->x, dy = b->old_y - b->y;
            dmg_add_pad(x0 + dx, y0 + dy, x1 - x0, y1 - y0);
            b->has_old = 0;
        }
        dmg_add_pad(x0, y0, x1 - x0, y1 - y0);
        if (b->overflow == OVF_VISIBLE && b->parent)
            dmg_add_pad(b->parent->x, b->parent->y,
                        b->parent->w, b->parent->h);
    }
    dmg_coalesce();

    int shadow = s_shadow_ready;
    if (shadow) vbe24_shadow_sink(1);

    /* 第二遍：按 damage 矩形重合成。对每条损伤矩形，重置裁剪栈后以该矩形
     * 为裁剪口，按 layer→z→树先序（paint_list 已排好）逐节点重画 body；
     * 矩形内的背景节点先画，旧窗口像素被真背景覆盖补齐。dirty/snap 只用于
     * 算损伤矩形，不在矩形循环内跳过或清节点；脏位统一在帧末清理。 */
    for (int r = 0; r < dmg_n; r++) {
        int rx0 = dmg[r].x, ry0 = dmg[r].y;
        int rx1 = rx0 + dmg[r].w, ry1 = ry0 + dmg[r].h;
        paint_begin_frame();
        paint_clip_push_rect(rx0, ry0, dmg[r].w, dmg[r].h);
        /* 覆盖兜底：重合成前先以 bg 角色色填实整条损伤矩形；即使某块
         * 生命周期出错、某片无人画，屏上也只剩纯底色，不烙上一帧残影。
         * 矩形面积 ≤0 时预填被跳过，是唯一需要告警的覆盖缺口点。 */
        if (dmg[r].w <= 0 || dmg[r].h <= 0) {
            ser_write("GUI:warn coveredskip rect=");
            ser_dec_u(rx0); ser_write(","); ser_dec_u(ry0);
            ser_write(","); ser_dec_u(dmg[r].w);
            ser_write(","); ser_dec_u(dmg[r].h); ser_write("\n");
            continue;
        }
        gui_fill_rect(rx0, ry0, dmg[r].w, dmg[r].h, gui_active_style()->bg);
        for (int i = 0; i < paint_count; i++) {
            block *b = paint_list[i];
            if (b->x >= rx1 || b->x + b->w <= rx0 ||
                b->y >= ry1 || b->y + b->h <= ry0) continue;
            paint_node_body_clipped(b);
        }
        paint_begin_frame();
    }

    /* 帧末统一清本帧绘制过节点的脏位；矩形循环内不清，避免后续矩形/节点
     * 被自己的过滤条件误跳过。 */
    for (int i = 0; i < paint_count; i++) {
        paint_list[i]->dirty_paint = 0;
        paint_list[i]->snap = 0;
    }

    /* 光标：合成结束后统一画到影子上；后端精灵路径自行把光标区回拷 LFB。 */
    int mx, my; uint8_t btn;
    mouse_get_state(&mx, &my, &btn);
    struct render_be *be = con_backend();
    if (be && be->mouse_cursor) be->mouse_cursor(mx, my, 1);

    /* 影子 → LFB：只回拷本帧损伤矩形，不整屏拷贝。 */
    if (shadow) {
        vbe24_shadow_sink(0);
        for (int r = 0; r < dmg_n; r++)
            vbe24_shadow_blit_rect(dmg[r].x, dmg[r].y, dmg[r].w, dmg[r].h);
    }

    uint64_t t1 = perf_now();

    uint32_t area = 0;
    for (int i = 0; i < dmg_n; i++) area += (uint32_t)(dmg[i].w * dmg[i].h);
    stat_note_frame(t1 - t0, (uint32_t)dmg_n, area);
    perf_record(s_last_us, stat_damage_area(), stat_damage_rects());

    uint32_t seq = s_frames;
    if (seq == 0) {
        /* 首帧全量合成打点：验收首帧 ≤8ms；frame_us 别名供测量器直取。 */
        ser_write("GUI:frame first=1 us="); ser_dec_u(s_last_us);
        ser_write(" rects="); ser_dec_u((uint32_t)dmg_n);
        ser_write(" area="); ser_dec_u(area);
        ser_write(" frame_us="); ser_dec_u(s_last_us);
        ser_write("\n");
    } else if (seq % 30 == 0) {
        ser_write("GUI:frame n="); ser_dec_u(seq + 1);
        ser_write(" us="); ser_dec_u(s_last_us);
        ser_write("\n");
    }
    s_frames++;

    gui_presence_sweep();
}

static void mark_all_paint(block *b) {
    if (!b) return;
    b->dirty_paint = 1;
    b->snap = 1;
    for (block *c = b->first_child; c; c = c->next_sibling)
        mark_all_paint(c);
}

/* 运行期打开组件：建块后显式 layout_run 会清掉 dirty_layout；这里把整棵
 * 新子树标脏，确保下一帧快照命中并整组重绘（否则叶子落在干净帧上不绘制）。 */
static void mark_subtree_paint(block *b) {
    if (!b) return;
    b->dirty_paint = 1;
    for (block *c = b->first_child; c; c = c->next_sibling)
        mark_subtree_paint(c);
}

/* 多实例瓦片摆放：第 idx 个实例按 body 宽度横排铺满一行，换行列另起一行，
 * 越下缘回卷左上角。只改几何/摆放，不触碰实例数据。纵向不错开（同行对齐）。 */
void ui_cascade_place(block *b, int idx, int w, int h, int gap) {
    if (!b) return;
    block *root = gui_root_block();
    int Rw = root && root->w ? root->w : GFX24_W;
    int Rh = root && root->h ? root->h : GFX24_H;
    const int M = 8;
    int avail_h = Rh - 40;   /* 任务栏带 + 安全边距，不压任务栏 */
    int cols = (Rw - 2 * M + gap) / (w + gap);
    if (cols < 1) cols = 1;
    int col = idx % cols;
    int row = idx / cols;
    int x = M + col * (w + gap);
    int y = M + row * (h + gap);
    if (x + w > Rw - M) { x = Rw - M - w; if (x < M) x = M; }
    if (y + h > avail_h) { x = M; y = M; }
    b->x = x; b->y = y; b->moved = 1;
}

/* 同层当前最大 z：新开面板据此取 max+1，避免与既有工作面板同层同序。 */
int32_t layer_max_z(uint16_t layer) {
    int32_t maxz = -1;
    for (uint32_t i = 1; i <= btable_capacity(); i++) {
        block *q = btable_get(i);
        if (q && q->layer == layer && q->z > maxz) maxz = q->z;
    }
    return maxz;
}

/* 标脏「子树 b ∪ 与矩形 r 相交的同层兄弟」：开/关面板时，被它遮挡或
 * 释放出的旧像素都要由兄弟/背景在同一帧补齐，避免重叠区穿模或残影。 */
void gui_mark_dirty_union(block *b, int x, int y, int w, int h) {
    mark_subtree_paint(b);
    uint16_t layer = b ? b->layer : LAYER_WORK;
    for (uint32_t i = 1; i <= btable_capacity(); i++) {
        block *q = btable_get(i);
        if (!q || q == b || q->layer != layer) continue;
        if (rects_overlap(x, y, w, h, q->x, q->y, q->w, q->h))
            mark_subtree_paint(q);
    }
}

/* 树先序收集（用于同层 z 全量重排）。 */
static void preorder(block *b, block **out, int *n) {
    if (!b || *n >= GUI_PAINT_MAX) return;
    out[(*n)++] = b;
    for (block *c = b->first_child; c; c = c->next_sibling)
        preorder(c, out, n);
}

/* 同层 z 按当前先序依次赋 0..n-1：绘制顺序只认 layer/z，不依赖挂树先序。 */
static void reorder_z(void) {
    block *list[GUI_PAINT_MAX];
    int n = 0;
    preorder(gui_root_block(), list, &n);
    for (int i = 0; i < n; i++) {
        int32_t k = 0;
        for (int j = 0; j < i; j++)
            if (list[j]->layer == list[i]->layer) k++;
        list[i]->z = k;
    }
}

void gui_mark_dirty(block_id id) {
    /* 根块是静态节点（id=ROOT_ID=1000），不在块表范围内，btable_get 返回 0；
     * 直接查表会让"标脏根"静默失效（菜单关闭后整块旧像素残留）。 */
    block *b = btable_get(id);
    if (!b && gui_root_block() && gui_root_block()->id == id) b = gui_root_block();
    mark_subtree_paint(b);
}

/* 抬到最前：先缓存父节点（detach 会清空 b->parent），摘挂回同一父链末尾，
 * 再按树先序全量重排同层 z；整树标脏——面板矩形与其遮挡兄弟矩形的并集
 * 都要重绘，避免旧像素穿模残留。父节点缓存错误会导致块脱离树不再绘制。 */
void gui_raise_to_front(block_id id) {
    block *b = btable_get(id);
    if (!b && gui_root_block() && gui_root_block()->id == id) b = gui_root_block();
    if (!b) return;
    block *p = b->parent;
    if (p) { tree_detach(b); tree_attach(p, b); }
    reorder_z();
    mark_all_paint(gui_root_block());
}

/* 强制下一帧提交：同 tick 内连续 gui_frame() 会被双提交门控判为重复，
 * 运行期切档位后需要拆掉该门控，否则切换结果不重绘。 */
void gui_force_next_frame(void) { s_last_tick = 0; }

/* 切主题：换活动令牌后整树标脏重绘（不做局部配色残留）。 */
void gui_theme_set(enum gui_token t) {
    gui_set_token(t);
    mark_all_paint(gui_root_block());
}

/* Presence 回收：标 leaving 的块存活一帧（leaving 帧计数），
 * 下一帧末尾归还块表空闲链；计数到 2 才真正回收。 */
void gui_presence_sweep(void) {
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (!b || !b->leaving) continue;
        if (b->leaving >= 2) {
            b->parent = b->first_child = b->next_sibling = 0;
            btable_free(b);
        } else {
            b->leaving++;
        }
    }
}

static void gui_reset_common(int keep_ce) {
    if (keep_ce) tree_reset_keep_ce();
    else         tree_reset();
    anim_reset();
    dmg_reset();
    s_frames = 0; s_layout_nodes = 0; s_double_commit = 0;
    s_damage_rects = 0; s_damage_area = 0;
    s_frame_cycles = 0; s_last_us = 0;
    s_last_tick = 0;
}

void gui_reset(void) { gui_reset_common(0); }

/* gui_begin 会话重建专用：保留控制台阶段已提交的 CE 声明子树（见 tree.c）。 */
void gui_reset_keep_ce(void) { gui_reset_common(1); }

void gui_begin(void) {
    /* v2.80 hotfix: 组件/画布先收摊（画布页池按块持有，不关会跨会话泄漏，
     * 第二次建画布直接失败），再清输入态与 chrome 静态句柄，最后重建块表。 */
    gui_canvas_close_all();
    gui_notepad_close();
    gui_filemgr_close();
    gui_images_close();
    gui_browser_close();
    gui_settings_close();
    gui_term_reset();
    ib_reset();
    gui_input_reset();
    /* 跨会话清残留按键 / 半包 / 积压滚轮：与 GUI 输入态在同一处一起复位，
     * 避免上一轮的 sticky 按键或拖拽捕获带进新一轮桌面。保留绝对落点。 */
    mouse_state_reset();
    gui_chrome_reset();
    gui_reset_keep_ce();
    gfx24_clear(gui_active_style()->bg);
    /* 影子缓冲在会话开始（帧计时之外）一次性分配并与首屏对齐；避免把
     * 1.44MB 堆零写/种子拷计入首帧/拖帧的 frame_us 测量窗口。分配失败
     * 后 s_shadow_ready=0，回退 legacy 直写 LFB，行为同基线。 */
    vbe24_shadow_init();
    s_shadow_ready = vbe24_shadow_mode() != 0;
    block *root = gui_root_block();
    root->x = 0; root->y = 0;
    root->w = GFX24_W; root->h = GFX24_H;
    root->dirty_layout = 1;
}

void gui_end(void) {
    /* 退出桌面边界：先清 GUI 拖拽/捕获/焦点/退出请求态，再清鼠标驱动会话态
     * （残留按键 / 半包 / 滚轮），两边一起清，避免半清把 sticky 态留给下一轮。 */
    gui_input_reset();
    mouse_state_reset();
    gui_reset();
}
