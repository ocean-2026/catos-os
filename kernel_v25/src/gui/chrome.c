/*
 * chrome.c -- 内核内 demo 块树、shell 主循环与自检探针（规格 §10 M0）。
 *
 * v1 供给方只有内核内模块：任务栏、开始按钮占位、时钟、示例块。
 * 不做应用节点化；CE 接入留 v2。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/tokens.h"
#include "gui/svg.h"
#include "gfx24.h"
#include "render.h"
#include "rtc.h"
#include "serial_log.h"
#include "task.h"
#include "kstring.h"
#include "keyboard.h"
#include "fs.h"
#include "lang.h"       /* i18n 第一刀：菜单/标题/任务栏/状态 trs 内联对 */
#include "cdeconf.h"    /* v2.82 A1.4: /etc/cde.conf scheme -> gui token  */
#include "ce.h"         /* ce_blk_rec2_t：声明语法 img=/icon= 探针落地 */

static void geom_walk(block *b);
static void gui_menu_toggle(block_id id, int32_t lx, int32_t ly);

static uint32_t s_idle_frame_tick;
static int gui_idle_step(void);

static int strcmp_probe(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* v2.81 isolated canvas channel (src/gui/cvchan.c). */
extern int  cvchan_self_test(void);
extern void cvchan_frame_tick(void);

int gui_cvchan_probe(void) { return cvchan_self_test(); }
void gui_cvchan_frame_tick(void) { cvchan_frame_tick(); }

/* SAK 安全模式（src/gui/sak.c）。 */
extern void gui_sak_poll(void);
extern int  gui_sak_probe(void);
extern void gui_sak_reset(void);

/* P3 声明式 ABI（src/gui/decl.c）。 */
extern int  decl_self_test(void);
extern void decl_dump_pid(int tid);
extern int  decl_ce_probe_commit(int tid, const void *recs, int n,
                                 const char *strings, int slen, uint32_t rec_size);
extern void decl_ce_probe_cleanup(int tid);
extern block_id decl_ce_probe_local_to_global(int tid, int lid);
extern int  decl_ce_probe_count(int tid);

static void split_word(const char *arg, char *word, int cap, const char **rest) {
    word[0] = 0; *rest = 0;
    if (!arg) return;
    while (*arg == ' ') arg++;
    int n = 0;
    while (*arg && *arg != ' ' && n < cap - 1) word[n++] = *arg++;
    word[n] = 0;
    while (*arg == ' ') arg++;
    *rest = arg;
}

static void xcopy_local(char *d, int cap, const char *s) {
    int n = 0; if (s) while (s[n]) n++;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
}

static void ser_dec(uint32_t v) {
    char b[12]; int n = 0;
    if (v == 0) { ser_write("0"); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) ser_putc(b[--n]);
}

static void ser_hash(uint32_t h) {
    ser_write("0x");
    for (int i = 7; i >= 0; i--) {
        int d = (h >> (i * 4)) & 15;
        ser_putc((char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}

/* ---- demo 块树 ---------------------------------------------------------- */

static blk_spec spec_panel(uint16_t layer, int32_t w, int32_t h, uint8_t border) {
    blk_spec s;
    memset(&s, 0, sizeof(s));
    s.req_w = w; s.req_h = h;
    s.layer = layer; s.z = 0;
    s.visible = 1; s.focusable = 0; s.inert = 0;
    /* 按语义角色跟随活动令牌，不固化当时 RGB；切主题时整树重解析。 */
    s.role_fg = ROLE_FG; s.role_accent = ROLE_ACCENT;
    if (layer == LAYER_BG)                       s.role_bg = ROLE_BG;
    else if (layer == LAYER_CHROME || layer == LAYER_OVERLAY) s.role_bg = ROLE_BAR;
    else                                          s.role_bg = ROLE_PANEL;
    s.fg = INHERIT_COLOR; s.bg = INHERIT_COLOR; s.accent = INHERIT_COLOR;
    s.scale = 1; s.border_w = border;
    return s;
}

static block_id clock_row = 0;
static block_id clock_text_blk = 0;
static char clock_last[16] = "";

/* D 轮 overlay 几何：任务栏/开始按钮句柄 + 任务栏高度档（默认 32，高 48）。 */
static block_id taskbar_id = 0;
static block_id start_id   = 0;
static int      taskbar_h   = 32;
static int      probe_tb_high = 0;
static int      probe_menu_tall = 0;

/* C1：任务栏运行实例按钮注册表。每个在跑实例一个按钮（标题沿用实例池
 * 现有编号，不重排）；点击 = 聚焦 + raise + 键盘路由回该实例 hook；活动项
 * 高亮口径与点窗口聚焦一致。开始菜单只做启动器，不再列运行实例。 */
enum { TB_KIND_TERM = 1, TB_KIND_FM = 2, TB_KIND_BR = 3, TB_KIND_NP = 4 };
typedef struct {
    int       kind;
    int       idx;
    block_id  btn;
    char      label[24];
} tb_entry_t;
#define TB_MAX 12
static tb_entry_t tb_ent[TB_MAX];
static int        tb_n;
static block_id   tb_slot;            /* 任务栏实例按钮容器行 */
static uint32_t   tb_sig;

/* 菜单键盘选择：可点行按显示顺序常驻索引。 */
static block_id menu_row_ids[32];
static int      menu_row_n   = 0;
static int      menu_sel     = -1;
static void     menu_scroll_bottom(void);

#define DEMO_ICON_W 16
#define DEMO_ICON_H 16
static uint8_t demo_icon[DEMO_ICON_W * DEMO_ICON_H * 3];
static int demo_icon_ready;

static void demo_icon_build(void) {
    if (demo_icon_ready) return;
    for (int y = 0; y < DEMO_ICON_H; y++) {
        for (int x = 0; x < DEMO_ICON_W; x++) {
            uint8_t v = ((x ^ y) & 1) ? 0xE0u : 0x20u;
            uint8_t *p = &demo_icon[((y * DEMO_ICON_W) + x) * 3];
            p[0] = v; p[1] = v; p[2] = v;
        }
    }
    demo_icon_ready = 1;
}

/* D7 裁剪探针：一张 200x40 的强调色位图，塞进 80x40 的窄父块，
 * 超出父框的部分必须被裁剪栈切掉，父框外保持桌面底色。 */
#define CROP_BMP_W 200
#define CROP_BMP_H 40
static uint8_t crop_bmp[CROP_BMP_W * CROP_BMP_H * 3];
static int crop_bmp_ready;

static void crop_bmp_build(void) {
    if (crop_bmp_ready) return;
    uint32_t acc = gui_active_style()->accent;
    uint8_t r = (uint8_t)((acc >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((acc >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(acc & 0xFFu);
    for (int i = 0; i < CROP_BMP_W * CROP_BMP_H; i++) {
        crop_bmp[i * 3 + 0] = r;
        crop_bmp[i * 3 + 1] = g;
        crop_bmp[i * 3 + 2] = b;
    }
    crop_bmp_ready = 1;
}

static void shot_crop_build(void) {
    crop_bmp_build();
    blk_spec parent = spec_panel(LAYER_WORK, 80, 40, 1);
    block_id pid = b_column(0, &parent);
    b_image(pid, 0, crop_bmp, CROP_BMP_W, CROP_BMP_H);
    layout_run();
    block *pp = btable_get(pid);
    pp->x = 440; pp->y = 80; pp->w = 80; pp->h = 40; pp->moved = 1;
    for (block *c = pp->first_child; c; c = c->next_sibling) {
        c->x = 440; c->y = 80; c->moved = 1;
    }
}

/* 11b 拖拽残影探针：A 先落在旧矩形 (64,300,120,80)，拖到右侧；
 * 抓帧断言旧矩形区域已回到桌面底色，不残留 A 的旧像素。 */
static void shot_drag_build(void) {
    blk_spec wa = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id A = b_stack(0, &wa);
    b_text(A, 0, "A");
    block_id B = b_stack(0, &wa);
    b_text(B, 0, "B");
    layout_run();
    block *pa = btable_get(A);
    block *pb = btable_get(B);
    pa->x = 64;  pa->y = 300; pa->moved = 1;
    pb->x = 300; pb->y = 300; pb->moved = 1;
    gui_frame();                              /* 先把 A/B 画到固定位置 */
    gui_inject_press(70, 305);
    gui_inject_move(200, 320);
    gui_inject_release();
    gui_frame();                               /* 拖拽后帧：旧矩形应回底色 */
}

static void mark_subtree_paint(block *b) {
    if (!b) return;
    b->dirty_paint = 1;
    for (block *c = b->first_child; c; c = c->next_sibling)
        mark_subtree_paint(c);
}

/* 样例一：仪表盘式卡片叠放（FREE 内错位叠放，z 递增）。 */
static void shot_free_dash_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);
    const char *names[3] = { "Card 1", "Card 2", "Card 3" };
    for (int i = 0; i < 3; i++) {
        blk_spec s = spec_panel(LAYER_WORK, 220, 140, 1);
        s.anchor = ANC_L | ANC_T;
        s.px_x = 60 + i * 40; s.px_y = 50 + i * 30;
        s.z = i;
        block_id c = b_rect(F, &s);
        blk_spec t = spec_panel(LAYER_WORK, 120, 16, 0);
        b_text(c, &t, names[i]);
    }
}

/* 样例二：右侧固定侧栏 + 中央自适应区。 */
static void shot_free_side_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec side = spec_panel(LAYER_WORK, 180, 0, 1);
    side.anchor = ANC_R | ANC_T | ANC_B; side.pct_h = 1000;
    block_id S = b_rect(F, &side);
    blk_spec st = spec_panel(LAYER_WORK, 160, 16, 0);
    b_text(S, &st, "Sidebar");

    blk_spec main = spec_panel(LAYER_CONTENT, 0, 0, 1);
    main.anchor = ANC_L | ANC_T | ANC_B;
    main.pct_w = 700; main.pct_h = 1000;
    main.m_r = 180;
    block_id M = b_rect(F, &main);
    blk_spec mt = spec_panel(LAYER_CONTENT, 160, 16, 0);
    b_text(M, &mt, "Adaptive center");
}

/* 样例四：溢出三态像素断言。父面板内 VISIBLE 子块拖出父框：旧位置恢复
 * 父底色，新位置显示子块；另放一对叠放块验证 z 高者覆盖重叠区。 */
static void shot_free_overflow_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec par = spec_panel(LAYER_WORK, 240, 160, 1);
    block_id P = b_rect(F, &par);
    block *pp = btable_get(P);
    pp->x = 100; pp->y = 100; pp->moved = 1;

    blk_spec ch = spec_panel(LAYER_WORK, 80, 60, 1);
    ch.overflow = OVF_VISIBLE;
    ch.role_bg = ROLE_BAR;
    ch.drag_mode = DRAG_FULL;
    ch.px_x = 20; ch.px_y = 20;
    block_id C = b_rect(P, &ch);

    /* 叠放对：A 面板在下，B 条色在上（z=1），重叠区只显示 B。 */
    blk_spec oa = spec_panel(LAYER_WORK, 100, 80, 1);
    oa.role_bg = ROLE_PANEL; oa.px_x = 40; oa.px_y = 300;
    b_rect(F, &oa);
    blk_spec ob = spec_panel(LAYER_WORK, 100, 80, 1);
    ob.role_bg = ROLE_BAR; ob.z = 1; ob.px_x = 70; ob.px_y = 320;
    b_rect(F, &ob);
    layout_run();
    mark_subtree_paint(gui_root_block());
    block *pc = btable_get(C);

    /* 把子块从父内 (120,120) 拖到父外 (300,220)。 */
    gui_inject_press(pc->x + 10, pc->y + 10);
    gui_inject_move(310, 230);
    gui_inject_release();
    gui_frame();
}

/* 样例三：悬浮球 + 百分比定位徽标（OVF_VISIBLE 浮层）。 */
static void shot_free_float_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec ball = spec_panel(LAYER_OVERLAY, 48, 48, 1);
    ball.anchor = ANC_HCENTER | ANC_VCENTER;
    ball.overflow = OVF_VISIBLE;
    b_rect(F, &ball);

    blk_spec badge = spec_panel(LAYER_OVERLAY, 40, 24, 1);
    badge.anchor = ANC_R | ANC_T; badge.pct_y = 150;
    badge.overflow = OVF_VISIBLE;
    b_rect(F, &badge);
}

/* 探针场景标志（仅 shot-* 开屏档使用，不进产品默认桌面）。 */
static int probe_no_sample;     /* 不建示例面板，避免压住待测窗口顶角 */
static int probe_menu_raise;    /* 菜单上移，避开任务栏裁切 BL/BR */
static int probe_card_only;     /* 控件卡参照帧：只建卡片，不放按钮/输入栏 */
static int probe_flush_inset;   /* C 轮：父卡内嵌贴左/贴顶 + 贴右/贴底对照子块 */
static int probe_flush_hpair;   /* C 轮：两块横向紧贴，零缝，各自 r12 */
static int probe_flush_vgroup;  /* C 轮：纵向按钮整组，仅外沿四角圆 */
static int probe_flush_bleed;  /* C 轮：full-bleed 通栏底条，bleed=1 */

/* 探针控件卡：一张卡片内放一个按钮与一个输入框，供 r12 / r8 档角部采样。
 * 自由层绝对定位，对齐 shot_free_interact_build 范式，不依赖 column 布局。 */
static void shot_controls_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec card = spec_panel(LAYER_WORK, 320, 160, 1);
    block_id C = b_rect(F, &card);
    block *cp = btable_get(C);
    cp->x = 140; cp->y = 120; cp->moved = 1;

    if (!probe_card_only) {
        blk_spec btn = spec_panel(LAYER_WORK, 120, 24, 1);
        block_id B = b_button(F, &btn, "Button");
        block *bp = btable_get(B);
        bp->x = 156; bp->y = 140; bp->moved = 1;

        blk_spec inp = spec_panel(LAYER_WORK, 180, 24, 1);
        block_id I = b_input(F, &inp, 0);
        block *ip = btable_get(I);
        ip->x = 156; ip->y = 184; ip->moved = 1;
    }
    layout_run();
    mark_subtree_paint(gui_root_block());
}

/* E1-③ 探针：按钮三态同点采样（idle/hover/pressed 分帧注入，不触发点击/焦点反色）。 */
static block_id e1_btn_id;
static void shot_e1_btn_build(const char *state) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec btn = spec_panel(LAYER_WORK, 160, 28, 1);
    e1_btn_id = b_button(F, &btn, "Btn");
    block *bp = btable_get(e1_btn_id);
    bp->x = 320; bp->y = 280; bp->moved = 1;

    if (state && !strcmp_probe(state, "hover"))
        gui_inject_hover_block(e1_btn_id);
    else if (state && !strcmp_probe(state, "pressed"))
        gui_inject_press_block(e1_btn_id);
    else if (state && !strcmp_probe(state, "blank"))
        gui_inject_press(600, 400);   /* E1-② 桌面空白命中：不应改任何块状态 */

    layout_run();
    mark_subtree_paint(gui_root_block());
    block *b = btable_get(e1_btn_id);
    ser_write("GUI:e1-btn rect ");
    if (b) { ser_dec(b->x); ser_write(","); ser_dec(b->y); ser_write(",");
             ser_dec(b->w); ser_write(","); ser_dec(b->h); }
    ser_write(" state="); ser_write(state ? state : "idle"); ser_write("\n");
}

/* v3 按钮图片资源探针：shot-icons（全量截图）/ shot-icons-bad（仅坏路径）。 */
#define ICON_BTN_N 6
static block_id icon_btn_ids[ICON_BTN_N];

static int icon_hover_on;   /* 探针：把首按钮注入 hover，验证 hover 背景下图标仍可见。 */
static void shot_icons_build(int bad_only, int hover) {
    icon_hover_on = hover;
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);
    struct { const char *img; const char *txt; uint8_t sz; } defs[ICON_BTN_N] = {
        { "/icons/ok.svg",      "OK",     16 },
        { "/icons/folder.svg",  "Folder", 16 },
        { "/icons/missing.svg", "Missing",16 },
        { "/icons/ok.svg",      "Big 24", 24 },
        { "/icons/folder.svg",  "",       16 },
        { "/nope/x.svg",        "NoFile", 16 }
    };
    int start = bad_only ? 5 : 0;
    int n = bad_only ? 1 : ICON_BTN_N;
    for (int i = 0; i < n; i++) {
        int di = start + i;
        blk_spec btn = spec_panel(LAYER_WORK, 220, 32, 1);
        btn.img = defs[di].img;
        btn.icon_sz = defs[di].sz;
        icon_btn_ids[di] = b_button(F, &btn, defs[di].txt);
        block *bp = btable_get(icon_btn_ids[di]);
        if (bp) { bp->x = 40; bp->y = 40 + di * 44; bp->moved = 1; }
    }
    if (icon_hover_on && icon_btn_ids[0])
        gui_inject_hover_block(icon_btn_ids[0]);
    layout_run();
    mark_subtree_paint(gui_root_block());
}

/* v3 声明语法探针：按钮 img=/icon= 走 CE rec2 声明通道（validate2→apply2）落地。
 * 三按钮：① ok.svg/16；② /nope/x.svg 回落内置；③ ok.svg icon_sz=99（非法档位，
 * 内核忽略该项回默认档，按钮仍正常渲染）。 */
#define DECL_IMG_N 3
static block_id decl_img_btn_ids[DECL_IMG_N];

static int decl_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void decl_rec2_init(ce_blk_rec2_t *r, int id, int parent, uint8_t kind,
                           int text_off) {
    memset(r, 0, sizeof(*r));
    r->base.id = id;
    r->base.parent = parent;
    r->base.kind = kind;
    r->base.layer = LAYER_WORK;
    r->base.req_w = -1;
    r->base.req_h = -1;
    r->base.text_off = text_off;
    r->base.pix_off = -1;
    r->base.res = -1;
    r->cap_ver = CE_BLK_REC2_VER;
}

static void shot_decl_img_build(void) {
    int tid = 10;
    decl_ce_probe_cleanup(tid);

    ce_blk_rec2_t recs[4];
    decl_rec2_init(&recs[0], 1, 0, BLK_COLUMN, -1);

    const char *p_ok  = "/icons/ok.svg";
    const char *p_bad = "/nope/x.svg";
    const char *t1 = "DeclSVG", *t2 = "DeclFB", *t3 = "BadIcon";
    char pool[160]; int po = 0;
    int off_ok  = po; memcpy(pool + po, p_ok,  (unsigned)decl_strlen(p_ok)  + 1);
                      po += decl_strlen(p_ok)  + 1;
    int off_bad = po; memcpy(pool + po, p_bad, (unsigned)decl_strlen(p_bad) + 1);
                      po += decl_strlen(p_bad) + 1;
    int off_t1  = po; memcpy(pool + po, t1,    (unsigned)decl_strlen(t1)    + 1);
                      po += decl_strlen(t1)    + 1;
    int off_t2  = po; memcpy(pool + po, t2,    (unsigned)decl_strlen(t2)    + 1);
                      po += decl_strlen(t2)    + 1;
    int off_t3  = po; memcpy(pool + po, t3,    (unsigned)decl_strlen(t3)    + 1);
                      po += decl_strlen(t3)    + 1;

    decl_rec2_init(&recs[1], 2, 1, BLK_BUTTON, off_t1);
    recs[1].base.req_w = 220; recs[1].base.req_h = 32;
    recs[1].img_off = off_ok;  recs[1].icon_sz = 16;

    decl_rec2_init(&recs[2], 3, 1, BLK_BUTTON, off_t2);
    recs[2].base.req_w = 220; recs[2].base.req_h = 32;
    recs[2].img_off = off_bad; recs[2].icon_sz = 16;

    decl_rec2_init(&recs[3], 4, 1, BLK_BUTTON, off_t3);
    recs[3].base.req_w = 220; recs[3].base.req_h = 32;
    recs[3].img_off = off_ok;  recs[3].icon_sz = 99;  /* 非法档位：应被忽略 */

    int rc = decl_ce_probe_commit(tid, recs, 4, pool, (uint32_t)po,
                                  (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0) {
        ser_write("GUI:decl-img FAIL commit rc="); ser_dec(rc); ser_putc('\n');
        return;
    }
    for (int di = 0; di < DECL_IMG_N; di++) {
        block_id gid = decl_ce_probe_local_to_global(tid, di + 2);
        decl_img_btn_ids[di] = gid;
        block *bp = btable_get(gid);
        if (bp) { bp->x = 40; bp->y = 40 + di * 44; bp->moved = 1; }
    }
    layout_run();
    mark_subtree_paint(gui_root_block());

    for (int di = 0; di < DECL_IMG_N; di++) {
        block *b = btable_get(decl_img_btn_ids[di]);
        ser_write("GUI:decl-img btn="); ser_dec(di);
        ser_write(" img="); ser_write(b && b->img ? b->img : "-");
        ser_write(" sz="); ser_dec(b ? b->icon_sz : 0);
        ser_putc('\n');
    }
}

/* v3 图片资源自检：图标墨迹、越界、缓存槽位 / 回落计数。 */
static void gui_probe_icon_audit(void) {
    ser_write("GUI:icon-audit enter\n");
    gui_begin();
    shot_icons_build(0, 0);
    int miss = 0, fall = 0;
    int slots = 0;
    int bad = 0;
    for (int i = 0; i < ICON_BTN_N; i++) {
        block *b = btable_get(icon_btn_ids[i]);
        if (!b) { bad = 1; continue; }
        const struct svg_icon *ic = b->img ? svg_resolve(b->img, b->icon_sz, 0) : 0;
        int ink = 0;
        if (ic) {
            for (int k = 0; k < ic->w * ic->h; k++)
                if (ic->mask[k]) { ink = 1; break; }
        }
        int inside = b->x >= 0 && b->y >= 0 &&
                     b->x + b->w <= GFX24_W && b->y + b->h <= GFX24_H;
        if (!ink) bad = 1;
        if (!inside) bad = 1;
        ser_write("GUI:icon btn="); ser_dec(i);
        ser_write(" ink="); ser_dec(ink);
        ser_write(" mono="); ser_dec(ic ? ic->mono : 0);
        ser_write(" rect "); ser_dec(b->x); ser_write(","); ser_dec(b->y);
        ser_write(","); ser_dec(b->w); ser_write(","); ser_dec(b->h);
        ser_write("\n");
    }
    slots = svg_cache_stats(&miss, &fall);
    ser_write("GUI:icon-audit slots="); ser_dec(slots);
    ser_write(" miss="); ser_dec(miss);
    ser_write(" fallback="); ser_dec(fall);
    ser_write(" result="); ser_dec(bad ? 0 : 1); ser_write("\n");
    gui_end();
}

/* E1-① 描边正反例：A 行 border=1 必显环；B 行 border=0 环上不得取到描边色。 */
static block_id e1_row_a, e1_row_b;
static void shot_e1_outline_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec a = spec_panel(LAYER_WORK, 240, 24, 1);
    e1_row_a = b_row(F, &a);
    block *pa = btable_get(e1_row_a); pa->x = 280; pa->y = 220; pa->moved = 1;
    b_text(e1_row_a, 0, "Stroked");

    blk_spec b = spec_panel(LAYER_WORK, 240, 24, 0);
    e1_row_b = b_row(F, &b);
    block *pb = btable_get(e1_row_b); pb->x = 280; pb->y = 270; pb->moved = 1;
    b_text(e1_row_b, 0, "Plain");

    layout_run();
    mark_subtree_paint(gui_root_block());
    block *A = btable_get(e1_row_a), *B = btable_get(e1_row_b);
    ser_write("GUI:e1-outline a ");
    if (A) { ser_dec(A->x); ser_write(","); ser_dec(A->y); ser_write(",");
             ser_dec(A->w); ser_write(","); ser_dec(A->h); }
    ser_write(" b ");
    if (B) { ser_dec(B->x); ser_write(","); ser_dec(B->y); ser_write(",");
             ser_dec(B->w); ser_write(","); ser_dec(B->h); }
    ser_write("\n");
}

/* E1-② 点击残留：标题带 / nav 项 / 正文 / 任务栏 Start 四切片。
 * idle 帧不注入；其余变体在 layout 后单点命中目标块并释放，串口上报各块矩形，
 * 断言主体区(y<568)未涉及块 diff=0、无块尺寸方块残留。 */
static block_id e1_res_title, e1_res_nav, e1_res_body;
static void shot_e1_residue_build(const char *state) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec t = spec_panel(LAYER_WORK, 300, 28, 1);
    e1_res_title = b_row(F, &t);
    block *tp = btable_get(e1_res_title); tp->x = 200; tp->y = 80; tp->moved = 1;
    b_text(e1_res_title, 0, "Title");

    blk_spec n = spec_panel(LAYER_WORK, 140, 28, 1);
    e1_res_nav = b_row(F, &n);
    block *np = btable_get(e1_res_nav); np->x = 40; np->y = 130; np->moved = 1;
    b_text(e1_res_nav, 0, "Nav");

    blk_spec d = spec_panel(LAYER_WORK, 300, 120, 1);
    e1_res_body = b_rect(F, &d);
    block *dp = btable_get(e1_res_body); dp->x = 200; dp->y = 130; dp->moved = 1;

    layout_run();
    mark_subtree_paint(gui_root_block());

    block *T = btable_get(e1_res_title), *N = btable_get(e1_res_nav), *D = btable_get(e1_res_body);
    if (state && !strcmp_probe(state, "title")) {
        gui_inject_press(T->x + T->w / 2, T->y + T->h / 2); gui_inject_release();
    } else if (state && !strcmp_probe(state, "nav")) {
        gui_inject_press(N->x + N->w / 2, N->y + N->h / 2); gui_inject_release();
    } else if (state && !strcmp_probe(state, "body")) {
        gui_inject_press(D->x + D->w / 2, D->y + D->h / 2); gui_inject_release();
    } else if (state && !strcmp_probe(state, "start")) {
        gui_inject_press(20, 584); gui_inject_release();
    }

    ser_write("GUI:e1-residue state="); ser_write(state ? state : "idle");
    ser_write(" title ");
    if (T) { ser_dec(T->x); ser_write(","); ser_dec(T->y); ser_write(",");
             ser_dec(T->w); ser_write(","); ser_dec(T->h); }
    ser_write(" nav ");
    if (N) { ser_dec(N->x); ser_write(","); ser_dec(N->y); ser_write(",");
             ser_dec(N->w); ser_write(","); ser_dec(N->h); }
    ser_write(" body ");
    if (D) { ser_dec(D->x); ser_write(","); ser_dec(D->y); ser_write(",");
             ser_dec(D->w); ser_write(","); ser_dec(D->h); }
    ser_write("\n");
}

/* E3-④ 输入框光标/键入：idle 聚焦空框；typed 聚焦后键入 abc。串口上报矩形 + len/pos。 */
static block_id e3_input_id;
static void shot_e3_input_build(const char *state) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec in = spec_panel(LAYER_WORK, 240, 28, 1);
    e3_input_id = b_input(F, &in, 0);
    block *ip = btable_get(e3_input_id); ip->x = 200; ip->y = 200; ip->moved = 1;

    layout_run();
    mark_subtree_paint(gui_root_block());

    gui_inject_press(ip->x + ip->w / 2, ip->y + ip->h / 2);   /* 聚焦接管键盘 */
    if (state && !strcmp_probe(state, "typed")) {
        gui_inject_key(0x61); gui_inject_key(0x62); gui_inject_key(0x63);  /* a b c */
    }
    gui_inject_release();

    int len = 0, pos = 0; ib_get_state(&len, &pos);
    ser_write("GUI:e3-input state="); ser_write(state ? state : "idle");
    ser_write(" rect ");
    if (ip) { ser_dec(ip->x); ser_write(","); ser_dec(ip->y); ser_write(",");
              ser_dec(ip->w); ser_write(","); ser_dec(ip->h); }
    ser_write(" len="); ser_dec(len); ser_write(" pos="); ser_dec(pos); ser_write("\n");
}

/* E2-⑤ z-order：下层大卡 + 上层小卡重叠。base 仅下层（基线）；stack 两层同叠。
 * 重叠区取样应取到上层（bar 色）；base 原重叠区与下层非重叠区同色、diff=0。 */
static block_id e2z_lo, e2z_hi;
static void shot_e2_z_build(const char *state) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec lo = spec_panel(LAYER_WORK, 280, 160, 1);
    e2z_lo = b_rect(F, &lo);
    block *lp = btable_get(e2z_lo); lp->x = 160; lp->y = 160; lp->moved = 1;

    e2z_hi = 0;
    if (!(state && !strcmp_probe(state, "base"))) {
        blk_spec hi = spec_panel(LAYER_WORK, 160, 80, 1);
        hi.role_bg = ROLE_BAR;
        e2z_hi = b_rect(F, &hi);
        block *hp = btable_get(e2z_hi); hp->x = 280; hp->y = 200; hp->moved = 1;
    }

    layout_run();
    mark_subtree_paint(gui_root_block());

    block *L = btable_get(e2z_lo), *H = btable_get(e2z_hi);
    ser_write("GUI:e2-z state="); ser_write(state ? state : "stack");
    ser_write(" lo ");
    if (L) { ser_dec(L->x); ser_write(","); ser_dec(L->y); ser_write(",");
             ser_dec(L->w); ser_write(","); ser_dec(L->h); }
    ser_write(" hi ");
    if (H) { ser_dec(H->x); ser_write(","); ser_dec(H->y); ser_write(",");
             ser_dec(H->w); ser_write(","); ser_dec(H->h); }
    ser_write("\n");
}

/* C 轮组 1：父卡 r16 内嵌两子块——C1 贴左+贴顶，C2 贴右+贴底（对照）。
 * 自由层绝对定位，8pt 网格；父卡 pad=0，内容框即整框。 */
static void shot_flush_inset_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec par = spec_panel(LAYER_WORK, 320, 200, 1);
    block_id P = b_rect(F, &par);
    block *pp = btable_get(P);
    pp->x = 80; pp->y = 80; pp->moved = 1;
    pp->st.corner_radius = 16;

    blk_spec c1 = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id C1 = b_rect(P, &c1);
    block *c1p = btable_get(C1);
    c1p->x = 80;  c1p->y = 80;  c1p->moved = 1;

    blk_spec c2 = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id C2 = b_rect(P, &c2);
    block *c2p = btable_get(C2);
    c2p->x = 280; c2p->y = 200; c2p->moved = 1;

    layout_run();
    mark_subtree_paint(gui_root_block());
}

/* C 轮组 2：两块横向紧贴，B.x = A.x + A.w（精确相等、零缝），各自 r12。 */
static void shot_flush_hpair_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec a = spec_panel(LAYER_WORK, 160, 80, 1);
    block_id A = b_rect(F, &a);
    block *ap = btable_get(A);
    ap->x = 80;  ap->y = 120; ap->moved = 1;
    ap->st.corner_radius = 12;

    blk_spec b = spec_panel(LAYER_WORK, 160, 80, 1);
    block_id B = b_rect(F, &b);
    block *bp = btable_get(B);
    bp->x = 240; bp->y = 120; bp->moved = 1;
    bp->st.corner_radius = 12;

    layout_run();
    mark_subtree_paint(gui_root_block());
}

/* C 轮组 3：纵向按钮整组 4 行，行高 24、y 严格相接；仅整组最外沿四角圆，
 * 组内相接边方角（同列兄弟紧贴由 flush_edges 判 flush）。 */
static void shot_flush_vgroup_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    for (int i = 0; i < 4; i++) {
        blk_spec r = spec_panel(LAYER_WORK, 160, 24, 1);
        block_id id = b_button(F, &r, "Item");
        block *p = btable_get(id);
        p->x = 80; p->y = 100 + i * 24; p->moved = 1;
    }
    layout_run();
    mark_subtree_paint(gui_root_block());
}

/* C 轮组 4：full-bleed 通栏底条，左右顶满屏幕，bleed=1；左右角方角、
 * 不新画弧，外侧沿用屏幕边。 */
static void shot_flush_bleed_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec bar = spec_panel(LAYER_WORK, GFX24_W, 48, 0);
    bar.role_bg = ROLE_BAR;
    block_id Bar = b_rect(F, &bar);
    block *bp = btable_get(Bar);
    bp->x = 0; bp->y = 552; bp->moved = 1;
    bp->bleed = 1;

    layout_run();
    mark_subtree_paint(gui_root_block());
}

/* 样例五：左手柄缩放像素断言。BAR 块初始 (230,90,160,100)，拖左手柄
 * 左移到 180：左边界随动，右边界保持 390 不动（min 80 / max 300）。 */
static void shot_free_interact_build(void) {
    blk_spec fr = spec_panel(LAYER_WORK, GFX24_W, GFX24_H, 0);
    block_id F = b_free(0, &fr);

    blk_spec par = spec_panel(LAYER_WORK, 300, 200, 1);
    block_id P = b_rect(F, &par);
    block *pp = btable_get(P);
    pp->x = 180; pp->y = 60; pp->moved = 1;

    blk_spec r = spec_panel(LAYER_WORK, 160, 100, 1);
    r.role_bg = ROLE_BAR;
    r.resize_dir = RESIZE_L | RESIZE_R;
    r.resize_min_w = 80; r.resize_max_w = 300;
    block_id R = b_rect(P, &r);
    block *pr = btable_get(R);
    pr->x = 230; pr->y = 90; pr->moved = 1;
    layout_run();
    mark_subtree_paint(gui_root_block());

    gui_inject_press(pr->x + 3, pr->y + pr->h / 2);
    gui_inject_move(180, pr->y + pr->h / 2);
    gui_inject_release();
    gui_frame();
}

static block_id demo_add_sample(void) {
    block_id root_id = gui_root_block()->id;
    blk_spec sample_spec = spec_panel(LAYER_WORK, 320, 200, 1);
    sample_spec.focusable = 1;
    sample_spec.drag_mode = DRAG_TITLE;
    sample_spec.pad_l = sample_spec.pad_r = sample_spec.pad_t = sample_spec.pad_b = 0;
    block_id sample = b_column(root_id, &sample_spec);

    blk_spec title = spec_panel(LAYER_WORK, 320, 24, 0);
    title.pad_l = 4;
    title.align_cross = XALN_CENTER;
    title.role_bg = ROLE_BAR;
    title.bg = INHERIT_COLOR;
    block_id bar = b_row(sample, &title);
    blk_spec title_txt = spec_panel(LAYER_WORK, 96, 16, 0);
    title_txt.align_cross = XALN_CENTER;
    b_text(bar, &title_txt, "CatOS Sample");

    blk_spec body = spec_panel(LAYER_WORK, 320, 176, 0);
    body.pad_l = 4; body.pad_r = 4; body.pad_t = 4; body.pad_b = 4;
    body.gap = 8;
    block_id body_id = b_column(sample, &body);
    blk_spec body_txt = spec_panel(LAYER_WORK, 200, 16, 0);
    b_text(body_id, &body_txt, "demo content area");

    /* 完整链路样例：TEXT → DIVIDER → IMAGE → SPACER 竖排（COLUMN）。 */
    blk_spec rule = spec_panel(LAYER_WORK, 200, 1, 0);
    b_divider(body_id, &rule);
    demo_icon_build();
    b_image(body_id, 0, demo_icon, DEMO_ICON_W, DEMO_ICON_H);
    b_space(body_id, 200, 8);
    return sample;
}

/* ---- /Desktop 图标列 ------------------------------------------------------- */

#define DESK_MAX_ITEMS 6
#define DESK_ITEM_W    96
#define DESK_ITEM_H    52
#define DESK_LAYOUT    "/etc/desktop.layout"

static char desk_names[DESK_MAX_ITEMS][FS_NAME_MAX];
static int  desk_n;
static block_id desk_item_blk[DESK_MAX_ITEMS];
/* P1-1: 当前选中条目序号；-1 = 无选中。内部运行时状态，不入 blk_spec / CE ABI。 */
static int  desk_sel = -1;

/* R5: 拖拽位置持久层（/etc/desktop.layout，行格式 `name x y`）。开机枚举时
 * 按名读回；无记录的条目走右侧纵向列兜底。FS 单条目内存视图上限 FS_DATA_MAX
 * (4096B)，每行 ≈ 名称(≤48)+坐标，6 条目远低于上限；追加满则本条位置不落盘，
 * 保留内存内几何降级。 */
static char desk_save_name[DESK_MAX_ITEMS][FS_NAME_MAX];
static int  desk_save_x[DESK_MAX_ITEMS];
static int  desk_save_y[DESK_MAX_ITEMS];
static int  desk_save_n;

/* shell.c 远程命令执行入口：桌面双击 .ce/.exe 复用同一条 run 分发路径。 */
extern int shell_ssh_exec_line(const char *line, char *out, int outmax, int *rc);

static int desk_xlen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static void desk_copy(char *d, int cap, const char *s) {
    int k = 0;
    while (s[k] && k < cap - 1) { d[k] = s[k]; k++; }
    d[k] = 0;
}

static int desk_ext_is(const char *name, const char *ext) {
    int dot = -1, n = desk_xlen(name);
    for (int k = 0; k < n; k++) if (name[k] == '.') dot = k;
    if (dot < 0) return 0;
    const char *e = name + dot + 1;
    for (int i = 0; ext[i]; i++) {
        char a = e[i], b = ext[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return e[desk_xlen(ext)] == 0;
}

/* 名称比较：先字节精确，再大小写不敏感兜底（FAT 卷大小写不敏感）。 */
static int desk_name_eq(const char *a, const char *b) {
    if (!strcmp_probe(a, b)) return 1;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* 解析 `name x y` 行；名称不含空白。返回 0 成功。 */
static int desk_layout_parse(const char *line, char *name, int namecap,
                             int *x, int *y) {
    int o = 0, k = 0;
    while (line[k] && line[k] != ' ' && line[k] != '\t') {
        if (o < namecap - 1) name[o++] = line[k];
        k++;
    }
    name[o] = 0;
    if (!name[0]) return -1;
    while (line[k] == ' ' || line[k] == '\t') k++;
    *x = 0;
    while (line[k] >= '0' && line[k] <= '9') { *x = *x * 10 + (line[k] - '0'); k++; }
    while (line[k] == ' ' || line[k] == '\t') k++;
    *y = 0;
    while (line[k] >= '0' && line[k] <= '9') { *y = *y * 10 + (line[k] - '0'); k++; }
    return 0;
}

static void desk_append_int(char *d, int cap, int *o, int v) {
    char tmp[12]; int n = 0;
    if (v <= 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && *o < cap - 1) d[(*o)++] = tmp[--n];
}

static void desk_layout_load(void) {
    desk_save_n = 0;
    fs_entry_t *f = fs_open_file(DESK_LAYOUT);
    if (!f) return;
    int cnt = fs_line_count(f);
    for (int i = 0; i < cnt; i++) {
        char line[FS_NAME_MAX + 24], nm[FS_NAME_MAX];
        int x, y;
        if (fs_get_line(f, i, line, sizeof(line)) < 0) continue;
        if (desk_layout_parse(line, nm, sizeof(nm), &x, &y) < 0) continue;
        if (desk_save_n >= DESK_MAX_ITEMS) break;
        desk_copy(desk_save_name[desk_save_n], FS_NAME_MAX, nm);
        desk_save_x[desk_save_n] = x;
        desk_save_y[desk_save_n] = y;
        desk_save_n++;
    }
}

static int desk_save_lookup(const char *name, int *x, int *y) {
    for (int i = 0; i < desk_save_n; i++) {
        if (desk_name_eq(desk_save_name[i], name)) {
            *x = desk_save_x[i]; *y = desk_save_y[i]; return 1;
        }
    }
    return 0;
}

/* 拖动结束：按名更新持久层（同名旧行删行后追加；无则追加），再落盘。 */
static void desk_layout_save(int i, int x, int y) {
    fs_entry_t *f = fs_create_file(DESK_LAYOUT);
    if (!f) { ser_write("GUI:desk layout open fail\n"); return; }
    int cnt = fs_line_count(f);
    for (int k = 0; k < cnt; k++) {
        char line[FS_NAME_MAX + 24], nm[FS_NAME_MAX];
        int ox, oy;
        if (fs_get_line(f, k, line, sizeof(line)) < 0) continue;
        if (desk_layout_parse(line, nm, sizeof(nm), &ox, &oy) == 0 &&
            desk_name_eq(nm, desk_names[i])) {
            fs_delete_line(f, k);
            break;
        }
    }
    char rec[FS_NAME_MAX + 24]; int o = 0, k = 0;
    while (desk_names[i][k] && o < (int)sizeof(rec) - 8) rec[o++] = desk_names[i][k++];
    rec[o++] = ' '; desk_append_int(rec, sizeof(rec), &o, x);
    rec[o++] = ' '; desk_append_int(rec, sizeof(rec), &o, y);
    rec[o] = 0;
    if (fs_append_line(f, rec) < 0) {
        ser_write("GUI:desk layout full, pos not saved\n");
        return;
    }
    fs_save(f);
}

/* 按块 id 反查条目序号（z 字段在拖拽/合成过程中会被改写，不能作为索引）。 */
static int desk_item_index(block_id id) {
    for (int i = 0; i < desk_n; i++)
        if (desk_item_blk[i] == id) return i;
    return -1;
}

static void desk_open_entry(int i) {
    char nm[FS_NAME_MAX];
    desk_copy(nm, sizeof(nm), desk_names[i]);
    int n = desk_xlen(nm);
    int is_dir = n && nm[n - 1] == '/';
    if (is_dir) nm[--n] = 0;
    char path[FS_PATH_MAX];
    desk_copy(path, sizeof(path), "/Desktop/");
    /* xcat 等价拼接 */
    { int o = desk_xlen(path); int k = 0;
      while (nm[k] && o < (int)sizeof(path) - 1) path[o++] = nm[k++];
      path[o] = 0; }

    if (is_dir)                       { gui_filemgr_open(path); return; }
    if (desk_ext_is(nm, "txt"))       { gui_notepad_open(path); return; }
    if (desk_ext_is(nm, "bmp") || desk_ext_is(nm, "ppm"))
                                      { gui_images_open(path); return; }
}

/* hov 是否落在块 b 的子树内（含 b 自身）。hover 目标是最深叶子，条目容器需沿父链上溯。 */
static int desk_in_subtree(block *hov, block *b) {
    for (block *p = hov; p; p = p->parent)
        if (p == b) return 1;
    return 0;
}

/* P1-1 条目自绘：hover 走 border 令牌细描边；选中走 accent 令牌粗描边 + panel 淡底。
 * 颜色全部取活动令牌，dark/light 对版，不硬编码 RGB；命中仍走既有 leaf，本回调只读
 * gui_hover_target() 与 desk_sel，不改命中链。render 在默认绘制之后、子块之前回调，
 * 淡底位于图标/标签之下。 */
static void desk_item_render(block_id id) {
    int i = desk_item_index(id);
    block *b = btable_get(id);
    if (i < 0 || !b) return;
    const struct style *tok = gui_active_style();
    block *hov = gui_hover_target();
    int hovered = hov && desk_in_subtree(hov, b);
    if (desk_sel == i) {
        gui_fill_rect(b->x, b->y, b->w, b->h, tok->panel);
        gui_draw_border(b->x, b->y, b->w, b->h, tok->accent, 2);
    } else if (hovered) {
        gui_draw_border(b->x, b->y, b->w, b->h, tok->border, 1);
    }
}

/* 点桌面空白（背景块）：取消选中，不触发任何打开动作。 */
static void desk_blank_click(block_id id, int32_t lx, int32_t ly) {
    (void)id; (void)lx; (void)ly;
    if (desk_sel >= 0) {
        block *o = btable_get(desk_item_blk[desk_sel]);
        if (o) o->dirty_paint = 1;
        desk_sel = -1;
        ser_write("GUI:desk sel=-1\n");
    }
}

static void desk_icon_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    int i = desk_item_index(id);
    if (i < 0) return;
    /* 选中切换：再点同一条目取消，点其它条目改选；单击打开契约保持不变。 */
    int old = desk_sel;
    desk_sel = (desk_sel == i) ? -1 : i;
    if (old >= 0) { block *o = btable_get(desk_item_blk[old]); if (o) o->dirty_paint = 1; }
    block *nb = btable_get(desk_item_blk[i]); if (nb) nb->dirty_paint = 1;
    ser_write("GUI:desk sel=");
    if (desk_sel < 0) ser_write("-1"); else ser_dec((uint32_t)desk_sel);
    ser_write("\n");
    desk_open_entry(i);
}

/* R5 双击：.ce/.exe 走现有 `run` 路径真运行；目录→FM；其余类型双击等同单击。 */
static void desk_icon_dblclick(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    int i = desk_item_index(id);
    if (i < 0) return;
    char nm[FS_NAME_MAX];
    desk_copy(nm, sizeof(nm), desk_names[i]);
    int n = desk_xlen(nm);
    int is_dir = n && nm[n - 1] == '/';
    if (is_dir) { desk_open_entry(i); return; }
    if (desk_ext_is(nm, "ce") || desk_ext_is(nm, "exe")) {
        char path[FS_PATH_MAX];
        desk_copy(path, sizeof(path), "/Desktop/");
        { int o = desk_xlen(path); int k = 0;
          while (nm[k] && o < (int)sizeof(path) - 1) path[o++] = nm[k++];
          path[o] = 0; }
        char line[FS_PATH_MAX + 8]; int lo = 0;
        line[lo++] = 'r'; line[lo++] = 'u'; line[lo++] = 'n'; line[lo++] = ' ';
        int p = 0;
        while (path[p] && lo < (int)sizeof(line) - 1) line[lo++] = path[p++];
        line[lo] = 0;
        static char dbl_cap[1024]; int rc = 0;
        shell_ssh_exec_line(line, dbl_cap, sizeof(dbl_cap), &rc);
        ser_write("GUI:desk dbl run="); ser_write(path);
        ser_write(" rc="); ser_dec((uint32_t)rc); ser_write("\n");
        return;
    }
    desk_open_entry(i);
}

/* R5 拖拽结束回调：把条目桌面坐标落持久层。phase 2 = 松手。 */
static void desk_item_drag(block_id id, int32_t dx, int32_t dy, uint8_t phase) {
    (void)dx; (void)dy;
    if (phase != 2) return;
    int i = desk_item_index(id);
    block *b = btable_get(id);
    if (i < 0 || !b) return;
    desk_layout_save(i, b->x, b->y);
}

/* v2.82 R3: 桌面条目显示为"桌面上的文件"——每个条目一个图标 + 文件名。
 * R5: 条目为根块下的自由布局容器，整块可拖；A7.6 用 BOUND_LOOSE（顶部条在屏内、
 * 可部分出屏），位置持久化到 /etc/desktop.layout；无记录条目按右侧纵向列兜底排布。 */
static uint8_t desk_item_icon(int is_dir) {
    return is_dir ? 4 : 3;
}

/* 一个 UTF-8 码点的字节长度（非法序列按单字节回落，与内核 utf8_to_u16 一致）。 */
static int desk_u8_len(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) return 2;
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) return 3;
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) return 4;
    return 1;
}

/* 标签截断：按码点计宽——ASCII 记 1 单位、其它记 2 单位，上限 9 单位，
 * 超限以 "..." 结尾。必须按码点步进，否则 UTF-8 续字节会被重复计宽，
 * 中文名字被过早截断。 */
static void desk_label(const char *name, char *out, int cap) {
    int units = 0, k = 0, o = 0;
    while (name[k] && o < cap - 3) {
        int clen = desk_u8_len(name + k);
        int u = ((unsigned char)name[k] < 128) ? 1 : 2;
        if (units + u > 9) break;           /* 9 单位 + "..." ≈ 96px 列宽 */
        if (o + clen > cap - 3) break;      /* 不留半截多字节字符 */
        for (int j = 0; j < clen; j++) out[o++] = name[k++];
        units += u;
    }
    if (name[k]) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; }
    out[o] = 0;
}

static void build_desktop_icons(block_id root_id) {
    static char desk_labels[DESK_MAX_ITEMS][FS_NAME_MAX];  /* b_text 只存指针，必须常驻 */
    fs_mkdir("/Desktop");   /* 已存在则忽略 */
    char names[DESK_MAX_ITEMS][FS_NAME_MAX];
    int n = fs_enum_entries("/Desktop", names, DESK_MAX_ITEMS);
    if (n < 0) n = 0;
    if (n > DESK_MAX_ITEMS) n = DESK_MAX_ITEMS;  /* 枚举返回总数，超上限静默截断，不得越界写 desk_names/desk_labels */
    desk_n = n;
    desk_sel = -1;   /* P1-1：每轮重建桌面清空选中态 */
    desk_layout_load();

    if (n == 0) {
        /* D1: 空桌面提示为纯文字块——不带面板底色、自然宽、屏幕居中，
         * 完整显示 "Empty"（此前 spec_panel 带 ROLE_PANEL 底且被列宽裁剪）。
         * 根块是 STACK 布局：居中走 align_main/align_cross，不用 anchor。 */
        blk_spec t = spec_panel(LAYER_WORK, 0, 16, 0);
        t.role_bg = ROLE_NONE;
        t.bg = INHERIT_COLOR;
        t.align_main = ALN_CENTER;
        t.align_cross = XALN_CENTER;
        t.wrap_lines = 1; t.ellipsis = 1;
        block_id tb = b_free(root_id, &t);
        b_text(tb, 0, trs("Empty", "空桌面"));
    } else {
        /* 兜底纵向列：靠右、从上到下；持久化坐标覆盖兜底位置。 */
        int fx = GFX24_W - 64 - DESK_ITEM_W;
        int fy0 = 64;
        int pitch = DESK_ITEM_H + 8;
        for (int i = 0; i < n; i++) {
            char shown[FS_NAME_MAX];
            desk_copy(desk_names[i], FS_NAME_MAX, names[i]);
            desk_copy(shown, sizeof(shown), names[i]);
            int sl = desk_xlen(shown);
            int is_dir = sl && shown[sl - 1] == '/';
            if (is_dir) shown[sl - 1] = 0;      /* 标签不带尾 '/' */

            int sx = 0, sy = 0, saved = desk_save_lookup(desk_names[i], &sx, &sy);

            blk_spec item = spec_panel(LAYER_WORK, DESK_ITEM_W, DESK_ITEM_H, 0);
            item.gap = 1;
            item.role_bg = ROLE_NONE;
            item.bg = INHERIT_COLOR;
            item.align_cross = XALN_CENTER;
            item.on_click = desk_icon_click;
            item.on_dblclick = desk_icon_dblclick;
            item.on_drag = desk_item_drag;
            item.render = desk_item_render;
            item.drag_mode = DRAG_FULL;
            item.drag_bounds = BOUND_LOOSE;
            item.z = i;
            block_id it = b_column(root_id, &item);
            desk_item_blk[i] = it;
            block *pi = btable_get(it);
            /* 根块是 STACK 布局：STACK 忽略子块 px_x/anchor，只保留 moved
             * 绝对几何。兜底位置也显式落 x/y + moved=1，与可拖窗口同口径。 */
            int ix = saved ? sx : fx;
            int iy = saved ? sy : fy0 + i * pitch;
            pi->x = ix; pi->y = iy;
            pi->pos_x = ix; pi->pos_y = iy;
            pi->px_x = ix; pi->px_y = iy;
            pi->moved = 1;

            blk_spec ic = spec_panel(LAYER_WORK, 32, 32, 0);
            ic.align_cross = XALN_CENTER;
            ic.tint_role = 3;                  /* accent：跟主题令牌走 */
            ic.z = i;                          /* 与容器同 z：合成按 layer→z 升序，父先序在前，底色才不会盖掉子块 */
            block_id ic_id = b_icon(it, &ic, desk_item_icon(is_dir));
            block *ic_b = btable_get(ic_id);
            if (ic_b) ic_b->icon_fit = 1;      /* P0-2：桌面图标墨迹撑满块框 */

            desk_label(shown, desk_labels[i], sizeof(desk_labels[i]));
            blk_spec lb = spec_panel(LAYER_WORK, 0, 16, 0);   /* 自然宽 + 居中，文件名才像 Windows 图标下标签 */
            lb.role_bg = ROLE_NONE;            /* P0-1r：标签去面板底色条，继承桌面背景 */
            lb.bg = INHERIT_COLOR;
            lb.align_cross = XALN_CENTER;
            lb.wrap_lines = 1; lb.ellipsis = 1;
            lb.z = i;
            b_text(it, &lb, desk_labels[i]);
        }
    }
    ser_write("GUI:desk items="); ser_dec((uint32_t)n); ser_write("\n");
}

/* ---- C1 任务栏实例按钮 --------------------------------------------------- */

static int tb_xlen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static void tb_make_label(char *out, int cap, const char *prefix, int idx) {
    xcopy_local(out, cap, prefix);
    int o = tb_xlen(out);
    int num = idx + 1;
    char tmp[12]; int m = 0;
    if (num == 0) tmp[m++] = '0';
    while (num) { tmp[m++] = (char)('0' + num % 10); num /= 10; }
    while (m && o < cap - 1) out[o++] = tmp[--m];
    out[o] = 0;
}

static int tb_is_active(int kind, int idx) {
    switch (kind) {
    case TB_KIND_TERM: return gui_term_active_idx() == idx &&
                               gui_key_hook_owner() == gui_term_focus_ud(idx);
    case TB_KIND_FM:   return gui_fm_active_slot() == idx &&
                               gui_key_hook_owner() == gui_fm_focus_ud(idx);
    case TB_KIND_BR:   return gui_br_active_idx() == idx &&
                               gui_key_hook_owner() == gui_br_focus_ud(idx);
    case TB_KIND_NP:   return gui_notepad_active();
    }
    return 0;
}

static const char *tb_kind_name(int kind) {
    switch (kind) {
    case TB_KIND_TERM: return "term";
    case TB_KIND_FM:   return "files";
    case TB_KIND_BR:   return "browser";
    case TB_KIND_NP:   return "notepad";
    }
    return "?";
}

/* 汇总当前在跑实例（编号沿用各实例池槽位，不重排）。 */
static int tb_collect(int *kind, int *idx, char (*labels)[24], int cap) {
    int n = 0;
    for (int i = 0; i < 8 && n < cap; i++)
        if (gui_term_root(i)) {
            kind[n] = TB_KIND_TERM; idx[n] = i;
            tb_make_label(labels[n], 24, trs("Terminal ", "终端 "), i); n++;
        }
    for (int i = 0; i < 8 && n < cap; i++)
        if (gui_fm_in_use(i)) {
            kind[n] = TB_KIND_FM; idx[n] = i;
            tb_make_label(labels[n], 24, trs("Files ", "文件 "), i); n++;
        }
    for (int i = 0; i < 8 && n < cap; i++)
        if (gui_br_root(i)) {
            kind[n] = TB_KIND_BR; idx[n] = i;
            tb_make_label(labels[n], 24, trs("Browser ", "浏览器 "), i); n++;
        }
    if (gui_notepad_active() && n < cap) {
        kind[n] = TB_KIND_NP; idx[n] = 0;
        xcopy_local(labels[n], 24, trs("Notepad", "记事本")); n++;
    }
    return n;
}

static void tb_btn_click(block_id id, int32_t lx, int32_t ly) {
    (void)lx; (void)ly;
    block *bl = btable_get(id);
    tb_entry_t *e = bl ? (tb_entry_t *)bl->usr : 0;
    if (!e) return;
    switch (e->kind) {
    case TB_KIND_TERM: gui_term_focus(e->idx); break;
    case TB_KIND_FM:   gui_fm_focus(e->idx); break;
    case TB_KIND_BR:   gui_br_focus(e->idx); break;
    case TB_KIND_NP:   gui_notepad_focus(); break;
    }
}

static void tb_rebuild(void) {
    if (!taskbar_id || !tb_slot) return;
    block *slot = btable_get(tb_slot);
    if (slot) {
        while (slot->first_child) b_remove(slot->first_child->id);
    }
    tb_n = 0;
    int kind[TB_MAX], idx[TB_MAX];
    char labels[TB_MAX][24];
    int n = tb_collect(kind, idx, labels, TB_MAX);
    for (int i = 0; i < n; i++) {
        blk_spec b = spec_panel(LAYER_CHROME, 0, 24, 1);
        b.role_bg = ROLE_PANEL;   /* 按钮面板底，与任务栏 bar 底拉开层次 */
        b.pad_l = 6; b.pad_r = 6;
        b.align_cross = XALN_CENTER;
        b.on_click = tb_btn_click;
        tb_ent[tb_n].kind = kind[i];
        tb_ent[tb_n].idx = idx[i];
        xcopy_local(tb_ent[tb_n].label, 24, labels[i]);
        b.usr = &tb_ent[tb_n];
        /* BLK_BUTTON 单块承载底色/描边/聚焦反色/居中文本（同浏览器 Back/Reload）。 */
        block_id bid = b_button(tb_slot, &b, tb_ent[tb_n].label);
        /* 根因：同层绘制序 = layer→z→树先序。桌面静态子树在首次 raise 时被
         * reorder_z 按先序赋了递增 z（容器 z=3…），而本批动态挂入的按钮仍带
         * 规格默认 z=0，排序落在父容器之前；父容器不透明底色后画，把按钮整段
         * 盖掉。这里让按钮 z 跟随父容器（与桌面条目同 z 惯用法一致：同 z 走树
         * 先序，父先于子），按钮才不会被父底色覆盖。 */
        {
            int32_t pz = slot ? slot->z : 0;
            block *bb = btable_get(bid);
            if (bb) bb->z = pz;
        }
        tb_ent[tb_n].btn = bid;
        tb_n++;
    }
    layout_run();
    /* 动态重建的按钮在手动 layout_run 后脏位被清：整子树补标画脏，
     * 否则本帧损伤矩形不覆盖任务栏，按钮底色/文字不上屏。 */
    mark_subtree_paint(btable_get(tb_slot));
}

/* 每帧轮询：实例集合变化才重建按钮；否则只刷新活动高亮。 */
static void tb_poll(void) {
    if (!taskbar_id) return;
    int kind[TB_MAX], idx[TB_MAX];
    char labels[TB_MAX][24];
    int n = tb_collect(kind, idx, labels, TB_MAX);
    uint32_t sig = 2166136261u;
    for (int i = 0; i < n; i++) {
        sig = sig * 31u + (uint32_t)kind[i];
        sig = sig * 131u + (uint32_t)idx[i];
        for (const char *p = labels[i]; *p; p++) sig = sig * 33u + (uint32_t)*p;
    }
    if (sig != tb_sig) {
        tb_rebuild();
        tb_sig = sig;
    }
    for (int i = 0; i < tb_n; i++) {
        block *bb = btable_get(tb_ent[i].btn);
        if (!bb) continue;
        int act = tb_is_active(tb_ent[i].kind, tb_ent[i].idx);
        if (bb->focused != act) { bb->focused = act; bb->dirty_paint = 1; }
    }
}

/* 探针打点：转储当前任务栏条目（kind / 编号 / 标签 / 活动）。 */
static void tb_dump(void) {
    for (int i = 0; i < tb_n; i++) {
        ser_write("TB:entry kind=");
        ser_write(tb_kind_name(tb_ent[i].kind));
        ser_write(" idx="); ser_dec((uint32_t)tb_ent[i].idx);
        ser_write(" label="); ser_write(tb_ent[i].label);
        ser_write(" active="); ser_dec(tb_is_active(tb_ent[i].kind, tb_ent[i].idx) ? 1 : 0);
        ser_write("\n");
    }
}

static void clock_refresh(int force) {
    rtc_time_t t;
    rtc_read(&t);
    char clk[16];
    int o = 0;
    clk[o++] = (char)('0' + t.hour / 10); clk[o++] = (char)('0' + t.hour % 10);
    clk[o++] = ':';
    clk[o++] = (char)('0' + t.minute / 10); clk[o++] = (char)('0' + t.minute % 10);
    clk[o++] = ':';
    clk[o++] = (char)('0' + t.second / 10); clk[o++] = (char)('0' + t.second % 10);
    clk[o] = 0;
    if (!force && clock_last[0] && strcmp_probe(clk, clock_last) == 0) return;
    for (int i = 0; i < 16; i++) clock_last[i] = clk[i];
    block *tb = btable_get(clock_text_blk);
    if (tb) {
        tb->text = clock_last;
        tb->dirty_paint = 1;   /* 只打自身脏位，不整树重排 */
    }
}

void gui_demo_build(void) {
    gui_style_load();   /* 开机 / 进桌面读取 /etc/style 档位，默认 app */
    /* v2.82 A1.4: mirror the saved shell scheme onto the gui token palette.
     * Only `light` has a matching TOK_LIGHT token; dark / solarized / mono
     * fall back to TOK_DARK (no separate gui token exists for the latter). */
    {
        const char *sc = cdeconf_scheme();
        if (sc && *sc && strcmp_probe(sc, "light") == 0)
            gui_theme_set(TOK_LIGHT);
        else
            gui_theme_set(TOK_DARK);
    }
    block *root = gui_root_block();
    root->w = GFX24_W; root->h = GFX24_H;
    root->pad_l = 64; root->pad_r = 64; root->pad_t = 64; root->pad_b = 64;
    block_id root_id = root->id;

    /* 桌面底色：全出血贴屏，忽略父级 padding。 */
    blk_spec bg = spec_panel(LAYER_BG, GFX24_W, GFX24_H, 0);
    bg.on_click = desk_blank_click;   /* P1-1：点桌面空白取消选中，不触发打开 */
    block_id bg_id = b_rect(root_id, &bg);
    btable_get(bg_id)->bleed = 1;

    if (!probe_no_sample) demo_add_sample();

    /* 任务栏：全出血贴底，宽 = 屏宽，交叉轴 END 钉下边缘。高度两档：默认 32，高 48。 */
    taskbar_h = probe_tb_high ? 48 : 32;
    blk_spec bar_tb = spec_panel(LAYER_CHROME, GFX24_W, taskbar_h, 0);
    bar_tb.align_cross = XALN_END;
    bar_tb.pad_l = 8; bar_tb.pad_r = 8;   /* v2.82 R1 P1-1: symmetric side padding */
    taskbar_id = b_row(root_id, &bar_tb);
    block *taskbar = btable_get(taskbar_id);
    taskbar->bleed = 1;
    /* v2.82 R1 P1-1: distribute the row so Start stays at the left edge and
     * the clock block is pinned to the right edge (no elastic spacer block). */
    taskbar->align_main = ALN_SPACE_BETWEEN;

    blk_spec start = spec_panel(LAYER_CHROME, 64, 24, 0);
    start.pad_l = 4;
    start.align_cross = XALN_CENTER;
    start.on_click = gui_menu_toggle;
    start_id = b_row(taskbar_id, &start);
    { block *sb0 = btable_get(start_id); if (sb0) sb0->click_on_up = 1; }
    blk_spec start_txt = spec_panel(LAYER_CHROME, 40, 16, 0);
    start_txt.align_cross = XALN_CENTER;
    b_text(start_id, &start_txt, trs("Start", "开始"));

    /* C1：运行实例按钮容器（Start 与时钟之间）；按钮由 tb_poll 按在跑实例填充。 */
    blk_spec tb_s = spec_panel(LAYER_CHROME, 0, 24, 0);
    tb_s.gap = 4;
    tb_s.align_cross = XALN_CENTER;
    tb_slot = b_row(taskbar_id, &tb_s);

    blk_spec clk_s = spec_panel(LAYER_CHROME, 80, 24, 0);
    clk_s.align_main = ALN_END;
    clk_s.align_cross = XALN_CENTER;
    clk_s.pad_l = 4;
    clock_row = b_row(taskbar_id, &clk_s);
    blk_spec clk_txt = spec_panel(LAYER_CHROME, 64, 16, 0);
    clk_txt.align_cross = XALN_CENTER;
    clock_text_blk = b_text(clock_row, &clk_txt, "00:00:00");
    clock_refresh(1);

    build_desktop_icons(root_id);
}

/* v2.82 R1 P0-1: live theme sync from the shell.  Maps the active scheme onto
 * the GUI token palette and forces the whole tree to repaint, so a desktop
 * that is already built follows `theme light|dark` immediately instead of
 * waiting for the next gui_demo_build().  If no tree exists yet this only
 * caches the token; gui_demo_build() re-derives it from cdeconf_scheme() too. */
void gui_theme_sync_scheme(const char *scheme) {
    if (scheme && *scheme && strcmp_probe(scheme, "light") == 0)
        gui_theme_set(TOK_LIGHT);
    else
        gui_theme_set(TOK_DARK);
    gui_force_next_frame();
}

/* ---- 开始菜单（LAYER_OVERLAY，模态） ---------------------------------- */

static block_id menu_root = 0;
static int      menu_open = 0;

static void id_label(block_id id, char *out) {
    const char *pre = "block ";
    int o = 0;
    while (*pre) out[o++] = *pre++;
    char num[10]; int n = 0;
    if (id == 0) num[n++] = '0';
    while (id) { num[n++] = (char)('0' + id % 10); id /= 10; }
    while (n) out[o++] = num[--n];
    out[o] = 0;
}

static void menu_clear_inert(void) {
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b) b->inert = 0;
    }
}

static int menu_collect(block_id *out, int cap) {
    int n = 0;
    for (uint32_t id = 1; id <= btable_capacity() && n < cap; id++) {
        block *b = btable_get(id);
        if (b && b->layer == LAYER_WORK && b->focusable) out[n++] = id;
    }
    return n;
}

/* 行容器自身可能不挂文本，标签在其首个子文本块上；取用于加速键匹配。 */
static const char *row_label(block *r) {
    if (!r) return 0;
    if (r->text) return r->text;
    if (r->first_child) return r->first_child->text;
    return 0;
}

static void on_menu_row(block_id id, int32_t lx, int32_t ly) {
    block *row = btable_get(id);
    if (!row) return;
    int32_t act = row->z;
    /* 顺序硬约束：先收菜单再执行动作。gui_menu_close() 会把全局 key hook
     * 清空，若先 open 组件再 close，组件刚装的键盘钩子会被这一清抹掉，
     * 表现为"窗口打开了但一个字都打不进去"（R4 终端无法输入根因）。 */
    gui_menu_close();
    if (act > 0) {
        gui_focus_raise((block_id)act);
    } else if (act == 0) {
        demo_add_sample();
    } else {
        switch (-act) {
        case 1: gui_notepad_open(0); break;
        case 2: gui_canvas_demo();   break;
        case 3: gui_filemgr_open(0); break;
        case 4: gui_images_open(0);  break;
        case 5: gui_browser_open(0); break;
        case 6: gui_settings_open(); break;
        case 7: gui_term_open();     break;
        }
    }
}

/* ---- D 轮 overlay 几何契约（规格 §16 新增规格） -------------------------- */
/* 求解顺序（固定，不可调换）：
 *   1. 期望尺寸（shrink-to-content：列布局按内容收敛自然宽高）；
 *   2. 按可用区钳制宽高（max_w/max_h = 屏幕 − 任务栏带 − 安全边距）；
 *   3. 最后才定锚点 / 放置方向（place above|below，空间不足 flip 到另一侧），
 *      再把整框 clamp 回屏内安全区。
 * 新字段语义：anchor 锚定参照（Start 按钮左边）、place 上方/下方、flip 翻侧、
 * clamp 钳制进可用区、max_w/max_h 尺寸上限。overlay 与任务栏不同父，不套 §14。 */
#define MENU_SAFE   8
#define MENU_GAP    8

static int menu_place_above = 1;   /* 1=above 任务栏之上；0=below（翻转后） */
static int menu_flip        = 0;
static int menu_natural_h   = 0;

static void menu_paint_sel(void) {
    for (int i = 0; i < menu_row_n; i++) {
        block *r = btable_get(menu_row_ids[i]);
        if (!r) continue;
        r->focused = (i == menu_sel) ? 1 : 0;
        r->dirty_paint = 1;
    }
}

static int menu_key(int code, void *ud) {
    (void)ud;
    if (!menu_open || menu_row_n <= 0) return 0;
    switch (code) {
    case KB_UP:
        menu_sel = (menu_sel <= 0) ? menu_row_n - 1 : menu_sel - 1;
        menu_paint_sel(); return 1;
    case KB_DOWN:
        menu_sel = (menu_sel >= menu_row_n - 1) ? 0 : menu_sel + 1;
        menu_paint_sel(); return 1;
    case 10: {  /* Enter：执行当前选中项 */
        block *r = btable_get(menu_row_ids[menu_sel]);
        if (r && r->on_click) r->on_click(r->id, 0, 0);
        return 1;
    }
    case 27:   /* Esc：关闭菜单（与全局 Esc 路径一致，钩子消费） */
        gui_menu_close(); return 1;
    default: {
        /* 字母直达：按行标签 `&X` 加速键匹配；菜单打开期间未匹配的字母
         * 一律吞键，不得漏进背后的输入框。 */
        int c = code;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c >= 'a' && c <= 'z') {
            for (int i = 0; i < menu_row_n; i++) {
                block *r = btable_get(menu_row_ids[i]);
                if (r && txt_accel_char(row_label(r)) == c) {
                    menu_sel = i; menu_paint_sel(); return 1;
                }
            }
            return 1;
        }
        return 0;
    }
    }
}

void gui_menu_open(void) {
    if (menu_open) return;
    block_id root_id = gui_root_block()->id;

    blk_spec menu = spec_panel(LAYER_OVERLAY, 180, 0, 2);
    menu.pad_l = 4; menu.pad_r = 4; menu.pad_t = 4; menu.pad_b = 4; menu.gap = 2;
    menu_root = b_column(root_id, &menu);
    block *mr = btable_get(menu_root);
    mr->moved = 1;   /* 浮层固定锚点，根 STACK 布局不得改写其绝对位置 */
    mr->st.corner_radius = 16;   /* §16.1 菜单外框 r16 */

    /* C1：开始菜单只做启动器，不再列运行实例（运行态归任务栏按钮承载）。 */
    menu_row_n = 0;

    static const char *app_en[7] = { "&Notepad", "&Canvas demo", "&Files", "&Images", "&Browser", "&Settings", "&Terminal" };
    static const char *app_zh[7] = { "记事本(&N)", "画布演示(&C)", "文件(&F)", "图片(&I)", "浏览器(&B)", "设置(&S)", "终端(&T)" };
    for (int a = 0; a < 7; a++) {
        blk_spec app = spec_panel(LAYER_OVERLAY, 172, 24, 1);
        app.on_click = on_menu_row;
        app.z = -(a + 1);
        block_id aid = b_row(menu_root, &app);
        { block *ab = btable_get(aid); if (ab) ab->click_on_up = 1; }
        if (menu_row_n < 32) menu_row_ids[menu_row_n++] = aid;
        b_text(aid, 0, trs(app_en[a], app_zh[a]));
    }

    blk_spec launch = spec_panel(LAYER_OVERLAY, 172, 24, 1);
    launch.on_click = on_menu_row;
    launch.z = 0;
    block_id lid = b_row(menu_root, &launch);
    { block *lb = btable_get(lid); if (lb) lb->click_on_up = 1; }
    if (menu_row_n < 32) menu_row_ids[menu_row_n++] = lid;
    b_text(lid, 0, trs("Launch &demo", "启动示例(&D)"));

    /* 探针超高例：追加若干合成行，强制内容高度超过 max_h，走内部滚动。 */
    if (probe_menu_tall) {
        static char tall_labels[16][16];
        for (int t = 0; t < 16 && menu_row_n < 32; t++) {
            blk_spec row = spec_panel(LAYER_OVERLAY, 172, 24, 1);
            row.on_click = on_menu_row;
            row.z = 0;
            block_id tid = b_row(menu_root, &row);
            menu_row_ids[menu_row_n++] = tid;
            int o = 0; const char *pre = "Item ";
            while (*pre) tall_labels[t][o++] = *pre++;
            tall_labels[t][o++] = (char)('1' + t); tall_labels[t][o] = 0;
            b_text(tid, 0, tall_labels[t]);
        }
    }

    layout_run();   /* ① shrink-to-content：按内容收敛自然宽高 */
    menu_natural_h = mr->h;

    /* ② 按可用区钳制（屏幕 − 任务栏带 − 安全边距）。 */
    block *tb = btable_get(taskbar_id);
    block *st = btable_get(start_id);
    int avail_l = MENU_SAFE;
    int avail_r = GFX24_W - MENU_SAFE;
    int avail_t = MENU_SAFE;
    int avail_b = tb ? tb->y - MENU_GAP : GFX24_H - MENU_SAFE;
    int max_w = avail_r - avail_l;
    int max_h = avail_b - avail_t;

    int mw = mr->w, mh = mr->h;
    if (mw > max_w) mw = max_w;
    int overflow = OVF_CLIP;
    if (mh > max_h) { mh = max_h; overflow = OVF_SCROLL; mr->scroll_y = 0; }
    mr->max_w = (int16_t)max_w;
    mr->max_h = (int16_t)max_h;
    mr->req_w = mw; mr->req_h = mh;   /* 二次 measure 保留钳制后的视口尺寸 */
    mr->w = mw; mr->h = mh;
    mr->overflow = (uint8_t)overflow;

    /* ③ 定锚点 / 方向：锚 Start 左边，默认 above（底边贴任务栏上沿）；
     * 上方空间不足则 flip 到任务栏下方；最后 clamp 回屏内安全区。 */
    int ax = st ? st->x : MENU_SAFE;
    menu_place_above = 1; menu_flip = 0;
    int y = avail_b - mh;
    if (y < avail_t) {
        menu_place_above = 0; menu_flip = 1;
        y = (tb ? tb->y + tb->h : GFX24_H - MENU_SAFE) + MENU_GAP;
    }
    int x = ax;
    if (x + mw > avail_r) x = avail_r - mw;
    if (x < avail_l) x = avail_l;
    if (y < avail_t) y = avail_t;
    if (y + mh > GFX24_H - MENU_SAFE) y = GFX24_H - MENU_SAFE - mh;
    mr->x = x; mr->y = y;
    mr->dirty_layout = 1;
    layout_run();

    menu_sel = menu_row_n > 0 ? 0 : -1;
    menu_paint_sel();
    if (probe_menu_tall) menu_scroll_bottom();
    gui_modal_inert(menu_root);
    gui_set_key_hook(menu_key, 0);
    menu_open = 1;
}

void gui_menu_close(void) {
    if (!menu_open) return;
    if (menu_root) { b_remove(menu_root); menu_root = 0; }
    menu_clear_inert();
    gui_set_key_hook(0, 0);
    menu_row_n = 0; menu_sel = -1;
    menu_open = 0;
}

int gui_menu_is_open(void) { return menu_open; }

/* v2.83 Alt+字母加速键：菜单未开则先打开（预选首项）；已开则按行标签 `&X`
 * 直达匹配并高亮该行。任何 Alt+字母都由本路径吞掉，不落到背后输入框。
 * 返回 1 = 已消费；非字母返回 0。 */
int gui_menu_accel(int code) {
    int c = code;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c < 'a' || c > 'z') return 0;
    if (!menu_open) gui_menu_open();
    if (menu_row_n <= 0) return 1;
    for (int i = 0; i < menu_row_n; i++) {
        block *r = btable_get(menu_row_ids[i]);
        if (r && txt_accel_char(row_label(r)) == c) {
            menu_sel = i; menu_paint_sel(); return 1;
        }
    }
    return 1;   /* 未匹配：保持预选首项，吞键 */
}

static void gui_menu_toggle(block_id id, int32_t lx, int32_t ly) {
    if (menu_open) gui_menu_close(); else gui_menu_open();
}

/* 超高例：滚到底（末项完整进内容框，首项滚出视口）。 */
static void menu_scroll_bottom(void) {
    block *mr = btable_get(menu_root);
    if (!mr || mr->overflow != OVF_SCROLL) return;
    int content = menu_natural_h;
    int view = mr->h;
    mr->scroll_y = (int16_t)(content > view ? content - view : 0);
    mr->dirty_paint = 1;
    for (int i = 0; i < menu_row_n; i++) {
        block *r = btable_get(menu_row_ids[i]);
        if (r) r->dirty_paint = 1;
    }
}

/* D 轮探针：把菜单 / 任务栏 / 开始按钮矩形与几何状态打到串口，供像素断言对照。 */
static void menu_report_rects(void) {
    block *mr = btable_get(menu_root);
    block *tb = btable_get(taskbar_id);
    block *st = btable_get(start_id);
    ser_write("GUI:menu-rect ");
    if (mr) { ser_dec(mr->x); ser_write(","); ser_dec(mr->y); ser_write(",");
              ser_dec(mr->w); ser_write(","); ser_dec(mr->h); }
    else ser_write("-1,-1,-1,-1");
    ser_write(" taskbar-rect ");
    if (tb) { ser_dec(tb->x); ser_write(","); ser_dec(tb->y); ser_write(",");
              ser_dec(tb->w); ser_write(","); ser_dec(tb->h); }
    else ser_write("-1,-1,-1,-1");
    ser_write(" start-rect ");
    if (st) { ser_dec(st->x); ser_write(","); ser_dec(st->y); ser_write(",");
              ser_dec(st->w); ser_write(","); ser_dec(st->h); }
    else ser_write("-1,-1,-1,-1");
    ser_write(" place="); ser_dec(menu_place_above);
    ser_write(" flip="); ser_dec(menu_flip);
    ser_write(" maxh="); ser_dec(mr ? mr->max_h : 0);
    ser_write(" nat="); ser_dec(menu_natural_h);
    ser_write(" scroll="); ser_dec(mr ? mr->scroll_y : 0);
    ser_write(" sel="); ser_dec(menu_sel);
    ser_write(" rows="); ser_dec(menu_row_n);
    ser_write(" ovf="); ser_dec(mr ? mr->overflow : 0);
    if (menu_row_n > 0) {
        block *f = btable_get(menu_row_ids[0]);
        block *l = btable_get(menu_row_ids[menu_row_n - 1]);
        ser_write(" first ");
        if (f) { ser_dec(f->x); ser_write(","); ser_dec(f->y); ser_write(",");
                 ser_dec(f->w); ser_write(","); ser_dec(f->h); }
        ser_write(" last ");
        if (l) { ser_dec(l->x); ser_write(","); ser_dec(l->y); ser_write(",");
                 ser_dec(l->w); ser_write(","); ser_dec(l->h); }
    }
    ser_write("\n");
}

/* ---- 自检探针 ---------------------------------------------------------- */

#define PROBE_DESC_MAX 64

void gui_probe_idempotent(void) {
    static blk_desc descs[PROBE_DESC_MAX];
    static blk_spec specs[PROBE_DESC_MAX];
    uint32_t seed = 2463534242u;
    int fail = 0;

    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    for (int round = 0; round < 20; round++) {
        seed = seed * 1664525u + 1013904223u;
        int n = 5 + (int)((seed >> 16) % 20);
        if (n > PROBE_DESC_MAX) n = PROBE_DESC_MAX;
        for (int i = 0; i < n; i++) {
            seed = seed * 1664525u + 1013904223u;
            memset(&specs[i], 0, sizeof(specs[i]));
            specs[i].req_w = 20 + (int)((seed >> 13) % 200);
            specs[i].req_h = 20 + (int)((seed >> 7) % 120);
            specs[i].pad_l = specs[i].pad_r = 2;
            specs[i].pad_t = specs[i].pad_b = 2;
            specs[i].gap = 2;
            specs[i].layer = LAYER_CONTENT;
            specs[i].visible = 1;
            specs[i].border_w = 1; specs[i].scale = 1;
            specs[i].fg = gui_active_style()->fg;
            specs[i].bg = gui_active_style()->bg;
            descs[i].id = (block_id)(i + 1);
            descs[i].kind = i == 0 ? BLK_STACK
                : (blk_kind)(BLK_RECT + ((seed >> 5) % 4));
            descs[i].parent = i == 0 ? 0 : 1;
            descs[i].spec = &specs[i];
            descs[i].text = 0; descs[i].pix = 0;
        }
        gui_rebuild(descs, n);
        layout_run();
        uint32_t h1 = gui_tree_hash();
        gui_commit_desc(descs, n);
        layout_run();
        uint32_t h2 = gui_tree_hash();
        if (h1 != h2) fail = 1;
        ser_write("GUI:idempotent round="); ser_dec((uint32_t)round);
        ser_write(" hash="); ser_hash(h2);
        ser_write(" match="); ser_dec(h1 == h2 ? 1 : 0); ser_write("\n");
    }
    ser_write(fail ? "GUI:idempotent=FAIL\n" : "GUI:idempotent=OK\n");
    gui_end();
}

void gui_probe_leak(void) {
    gui_begin();
    for (int i = 0; i < 1000; i++) {
        block_id id = b_rect(0, 0);
        b_remove(id);
        /* 批次回收：两轮 sweep 对应 leaving 两帧，句柄归还空闲链。 */
        if ((i & 7) == 7) { gui_presence_sweep(); gui_presence_sweep(); }
    }
    gui_presence_sweep();
    gui_presence_sweep();
    ser_write("GUI:leak live="); ser_dec(stat_blocks_live());
    ser_write(" tree="); ser_dec(tree_count_nodes());
    ser_write(" leaked="); ser_dec(stat_blocks_leaked()); ser_write("\n");
    gui_end();
}

void gui_probe_dirty(void) {
    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    blk_spec col = spec_panel(LAYER_CONTENT, 0, 0, 0);
    col.gap = 2;
    col.pad_l = col.pad_r = col.pad_t = col.pad_b = 2;
    block_id A = b_column(0, &col);
    blk_spec leaf = spec_panel(LAYER_CONTENT, 60, 40, 1);
    block_id a1 = b_rect(A, &leaf);
    b_rect(A, &leaf);
    block_id B = b_column(0, &col);
    b_rect(B, &leaf);
    b_rect(B, &leaf);

    layout_run();

    blk_spec ch = leaf;
    ch.req_w = 120;
    b_set_props(a1, &ch);
    layout_run();

    uint32_t measured = stat_layout_nodes();
    uint32_t total = tree_count_nodes();
    block *pa = btable_get(A);
    uint32_t sub = tree_count_subtree(pa);

    ser_write("GUI:dirty measured="); ser_dec(measured);
    ser_write(" total="); ser_dec(total);
    ser_write(" affected_subtree="); ser_dec(sub);
    ser_write(" untouched="); ser_dec(total - sub - 1);
    int ok = (measured <= sub + 1) && (measured < total);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    geom_walk(gui_root_block());
    gui_end();
}

void gui_probe_drag(void) {
    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    blk_spec wa = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id A = b_stack(0, &wa);
    b_text(A, 0, "A");
    block_id B = b_stack(0, &wa);
    b_text(B, 0, "B");
    layout_run();

    block *pa = btable_get(A);
    block *pb = btable_get(B);
    /* B 挪到右下，避免与 A 在原点重叠造成命中归属歧义。 */
    pb->x = 200; pb->y = 100; pb->moved = 1;
    int b0x = pb->x, b0y = pb->y, b0w = pb->w, b0h = pb->h;

    gui_inject_press(20, 10);    /* A 标题栏区域 */
    gui_inject_move(130, 110);    /* 拖到新位置 */
    gui_inject_release();

    gui_frame();

    uint32_t measured = stat_layout_nodes();
    uint32_t rects = stat_damage_rects();
    uint32_t area = stat_damage_area();
    uint32_t us = stat_last_frame_us();
    uint32_t sub = tree_count_subtree(pa);
    int b_same = pb->x == b0x && pb->y == b0y && pb->w == b0w && pb->h == b0h;

    ser_write("GUI:drag ax="); ser_dec((uint32_t)pa->x);
    ser_write(" ay="); ser_dec((uint32_t)pa->y);
    ser_write(" bx="); ser_dec((uint32_t)pb->x);
    ser_write(" by="); ser_dec((uint32_t)pb->y);
    ser_write(" measured="); ser_dec(measured);
    ser_write(" total="); ser_dec(tree_count_nodes());
    ser_write(" subtree="); ser_dec(sub);
    ser_write(" damage_rects="); ser_dec(rects);
    ser_write(" damage_area="); ser_dec(area);
    ser_write(" frame_us="); ser_dec(us);
    ser_write(" b_same="); ser_dec(b_same ? 1 : 0);
    int ok = b_same && measured <= sub + 1 && area <= 120000u && us <= 4000u;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* 禁拖自检：背景/图标/任务栏等硬禁拖块被拖动时几何必须零变化；
 * 正控：显式 DRAG_TITLE 的示例面板标题带仍应可拖。 */
void gui_probe_deny(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    block *root = gui_root_block();
    block *denied[8]; int dn = 0;
    int sx[8], sy[8], sw[8], sh[8];
    block *sample = 0;
    for (block *c = root->first_child; c && dn < 8; c = c->next_sibling) {
        if (c->drag_mode == DRAG_TITLE || c->drag_mode == DRAG_FULL) {
            sample = c;
            continue;
        }
        denied[dn] = c;
        sx[dn] = c->x; sy[dn] = c->y; sw[dn] = c->w; sh[dn] = c->h;
        dn++;
    }

    /* 空白桌面拖 10 步，再拖桌面图标；禁拖块几何不得变化。 */
    gui_inject_press(400, 300);
    for (int i = 0; i < 10; i++)
        gui_inject_move(400 + 10 * (i + 1), 300 + 6 * (i + 1));
    gui_inject_release();
    gui_inject_press(668, 64);
    for (int i = 0; i < 10; i++)
        gui_inject_move(668 - 8 * (i + 1), 64 + 6 * (i + 1));
    gui_inject_release();

    int moved = 0;
    for (int k = 0; k < dn; k++) {
        block *c = denied[k];
        if (c->x != sx[k] || c->y != sy[k] ||
            c->w != sw[k] || c->h != sh[k])
            moved++;
    }

    int sample_before_x = sample ? sample->x : 0;
    int sample_before_y = sample ? sample->y : 0;
    if (sample) {
        gui_inject_press(224, 76);
        gui_inject_move(320, 140);
        gui_inject_release();
    }
    int sample_moved = sample &&
        (sample->x != sample_before_x || sample->y != sample_before_y);

    ser_write("GUI:deny denied="); ser_dec(dn);
    ser_write(" denied_moved="); ser_dec(moved);
    ser_write(" sample_moved="); ser_dec(sample_moved ? 1 : 0);
    int ok = (moved == 0) && sample_moved;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_hit(void) {
    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    blk_spec lo = spec_panel(LAYER_CONTENT, 160, 100, 1);
    b_stack(0, &lo);
    blk_spec hi = spec_panel(LAYER_CONTENT, 160, 100, 1);
    hi.z = 1;
    block_id Hi = b_stack(0, &hi);
    layout_run();

    block_id hit = gui_hit_test(80, 80);
    ser_write("GUI:hit hit="); ser_dec(hit);
    ser_write(" want="); ser_dec(Hi);
    int ok = hit == Hi;

    /* 父块角 3px 内返回 RESIZE：边缘带优先于子块命中。放到空白右下角。 */
    blk_spec pc = spec_panel(LAYER_CONTENT, 160, 100, 1);
    pc.resize_dir = RESIZE_R | RESIZE_B;
    block_id P2 = b_rect(0, &pc);
    layout_run();
    block *pp2 = btable_get(P2);
    pp2->x = 560; pp2->y = 420; pp2->moved = 1;
    block_id act = 0; uint8_t rdir = 0;
    int mode = gui_interaction_at(pp2->x + pp2->w - 3,
                                  pp2->y + pp2->h - 3, &act, &rdir);
    int resize_hit = (mode == GUI_IT_RESIZE && act == P2 &&
                      (rdir & RESIZE_R) && (rdir & RESIZE_B));
    ok = ok && resize_hit;
    ser_write(" resize_hit="); ser_dec(resize_hit);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

extern volatile uint32_t timer_ticks;

void gui_probe_rate(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    uint32_t t0 = timer_ticks;
    for (int i = 0; i < 33; i++) {   /* 约 1 秒，3 tick/帧 */
        gui_frame();
        task_sleep_ticks(3);
    }
    uint32_t frames = stat_frames();
    uint32_t dc = stat_double_commit();
    ser_write("GUI:rate frames="); ser_dec(frames);
    ser_write(" double_commit="); ser_dec(dc);
    ser_write(" ticks="); ser_dec(timer_ticks - t0);
    int ok = frames <= 33 && dc == 0;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* 生产节流实测：走与空闲循环同一条 gui_idle_step 路径跑满 99 tick（≈1s），
 * 不自行 sleep 3 tick 造节拍；帧计数应 ≈33（≤33），同 tick 重复提交为 0。 */
void gui_probe_idle(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    s_idle_frame_tick = timer_ticks - 3;   /* 首帧立即提交 */
    uint32_t t0 = timer_ticks;
    while (timer_ticks - t0 < 99) {
        gui_idle_step();
        task_sleep_ticks(1);
    }
    uint32_t frames = stat_frames();
    uint32_t dc = stat_double_commit();
    ser_write("GUI:idle frames="); ser_dec(frames);
    ser_write(" double_commit="); ser_dec(dc);
    ser_write(" ticks="); ser_dec(timer_ticks - t0);
    int ok = frames <= 33 && dc == 0;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

static void geom_walk(block *b) {
    ser_write("GUI:geom id="); ser_dec(b->id);
    ser_write(" kind="); ser_dec((uint32_t)b->kind);
    ser_write(" x="); ser_dec((uint32_t)b->x);
    ser_write(" y="); ser_dec((uint32_t)b->y);
    ser_write(" w="); ser_dec((uint32_t)b->w);
    ser_write(" h="); ser_dec((uint32_t)b->h);
    ser_write(" layer="); ser_dec(b->layer);
    ser_write(" z="); ser_dec((uint32_t)b->z);
    ser_write("\n");
    for (block *c = b->first_child; c; c = c->next_sibling)
        geom_walk(c);
}

void gui_probe_geom(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    geom_walk(gui_root_block());
    gui_end();
}

static int bounds_walk(block *b, int *bad) {
    int ok = 1;
    for (block *c = b->first_child; c; c = c->next_sibling) {
        int px = b->x, py = b->y, pw = b->w, ph = b->h;
        if (!c->bleed) {
            px += b->pad_l; py += b->pad_t;
            pw -= b->pad_l + b->pad_r; ph -= b->pad_t + b->pad_b;
        }
        int inside = c->x >= px && c->y >= py &&
                     c->x + c->w <= px + pw && c->y + c->h <= py + ph;
        if (!inside) {
            *bad = 1;
            ser_write("GUI:bounds id="); ser_dec(c->id);
            ser_write(" parent="); ser_dec(b->id);
            ser_write(" child=("); ser_dec(c->x); ser_write(","); ser_dec(c->y);
            ser_write(","); ser_dec(c->w); ser_write(","); ser_dec(c->h);
            ser_write(") parent=("); ser_dec(px); ser_write(","); ser_dec(py);
            ser_write(","); ser_dec(pw); ser_write(","); ser_dec(ph);
            ser_write(")\n");
        }
        ok &= inside;
        ok &= bounds_walk(c, bad);
    }
    return ok;
}

void gui_probe_bounds(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    int bad = 0;
    block *root = gui_root_block();
    int root_ok = root->x >= 0 && root->y >= 0 &&
                  root->x + root->w <= GFX24_W && root->y + root->h <= GFX24_H;
    if (!root_ok) bad = 1;
    bounds_walk(root, &bad);
    ser_write("GUI:bounds result="); ser_dec(bad ? 0 : 1); ser_write("\n");
    gui_end();
}

/* A7.6 BOUND_LOOSE 自检：自建 loose 块分别拖向左上 / 右下出屏，回读夹紧坐标。
 * 断言：顶部标题条(24px)在屏内、水平至少留 6px；左上拖后块左缘为负（确实部分出屏），
 * 证明不是整窗夹紧。BOUND_SCREEN/PARENT 路径不在本探针覆盖内。 */
static void gui_probe_loose(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    blk_spec sp = spec_panel(LAYER_WORK, 96, 52, 0);
    sp.drag_mode = DRAG_FULL;
    sp.drag_bounds = BOUND_LOOSE;
    block_id id = b_column(0, &sp);
    block *b = btable_get(id);
    if (b) { b->x = 300; b->y = 200; b->pos_x = 300; b->pos_y = 200; b->moved = 1; }
    layout_run();

    int w = b->w, h = b->h, th = h < 24 ? h : 24;
    /* 左上出屏 */
    gui_inject_press(b->x + w / 2, b->y + h / 2);
    gui_inject_move(-300, -300);
    gui_inject_release();
    int lx = b->x, ly = b->y;
    /* 复位到中央，再拖右下出屏（避免上一步把按压点带出屏外导致二次命中失败）。 */
    b->x = 300; b->y = 200; b->pos_x = 300; b->pos_y = 200;
    gui_inject_press(b->x + w / 2, b->y + h / 2);
    gui_inject_move(GFX24_W + 300, GFX24_H + 300);
    gui_inject_release();
    int rx = b->x, ry = b->y;

    int top_in  = (ly >= 0) && (ly + th <= GFX24_H);
    int bot_in  = (ry >= 0) && (ry + th <= GFX24_H);
    int left_vis  = lx >= -(w - 6) && lx <= GFX24_W - 6;
    int right_vis = rx >= -(w - 6) && rx <= GFX24_W - 6;
    int partial = lx < 0;   /* 左缘出屏 = 块确实部分在屏外 */
    int ok = top_in && bot_in && left_vis && right_vis && partial;

    ser_write("GUI:loose w="); ser_dec(w); ser_write(" h="); ser_dec(h);
    ser_write(" lx="); ser_dec(lx); ser_write(" ly="); ser_dec(ly);
    ser_write(" rx="); ser_dec(rx); ser_write(" ry="); ser_dec(ry);
    ser_write(" partial="); ser_dec(partial);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* P1-1 hover/选中态自检：hover 落在条目子树 -> 单击选中 -> 点空白取消。
 * 命中/打开契约走既有 leaf；本探针只读 desk_sel 与 gui_hover_target() 做断言。 */
static void gui_probe_hsel(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    if (desk_n <= 0) { ser_write("GUI:hsel items=0 result=0\n"); gui_end(); return; }
    block *b0 = btable_get(desk_item_blk[0]);
    int cx = b0->x + b0->w / 2, cy = b0->y + b0->h / 2;

    gui_inject_hover(cx, cy);
    block *hov = gui_hover_target();
    int hov_in = hov && desk_in_subtree(hov, b0);

    gui_inject_press(cx, cy); gui_inject_release();
    int sel1 = desk_sel;

    gui_inject_press(40, 40); gui_inject_release();
    int sel2 = desk_sel;

    int ok = hov_in && sel1 == 0 && sel2 < 0;
    ser_write("GUI:hsel hov_in="); ser_dec((uint32_t)hov_in);
    ser_write(" sel1="); ser_dec((uint32_t)sel1);
    ser_write(" sel2="); ser_dec((uint32_t)sel2);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* ---- R4 裁剪审计（只读探针） -------------------------------------------- */
/* 遍历块表：列出 visible 但 w/h<=0 的塌陷块，以及未声明允许溢出（非
 * OVF_VISIBLE / moved / bleed / 高层浮层）却越出父内容框的子块。
 * 规则口径与 layout.c clamp_child 完全一致；探针只读数，不改几何。 */
static int audit_zero_n, audit_over_n;
static void audit_walk(block *b) {
    for (block *c = b->first_child; c; c = c->next_sibling) {
        if (c->visible && (c->w <= 0 || c->h <= 0)) {
            audit_zero_n++;
            ser_write("GUI:audit-zero id="); ser_dec(c->id);
            ser_write(" parent="); ser_dec(b->id);
            ser_write(" kind="); ser_dec((uint32_t)c->kind);
            ser_write(" w="); ser_dec((int32_t)c->w);
            ser_write(" h="); ser_dec((int32_t)c->h);
            { int mw = (c->kind == BLK_TEXT) ?
                txt_measure_w(c->text, c->font_size,
                              c->st.scale ? c->st.scale : 1, c->font_tabular) : -1;
              ser_write(" measw="); ser_dec(mw < 0 ? 0 : (uint32_t)mw); }
            ser_write(" txt="); ser_write(c->text ? c->text : "(null)");
            ser_write("\n");
        }
        int declared = (c->overflow == OVF_VISIBLE || c->moved || c->bleed ||
                        c->layer > b->layer);
        if (!declared) {
            int px = b->x, py = b->y, pw = b->w, ph = b->h;
            if (!c->bleed) {
                px += b->pad_l; py += b->pad_t;
                pw -= b->pad_l + b->pad_r;
                ph -= b->pad_t + b->pad_b;
            }
            int inside = c->x >= px && c->y >= py &&
                         c->x + c->w <= px + pw && c->y + c->h <= py + ph;
            if (!inside) {
                audit_over_n++;
                ser_write("GUI:audit-over id="); ser_dec(c->id);
                ser_write(" parent="); ser_dec(b->id);
                ser_write(" child=("); ser_dec((int32_t)c->x);
                ser_write(","); ser_dec((int32_t)c->y);
                ser_write(","); ser_dec((int32_t)c->w);
                ser_write(","); ser_dec((int32_t)c->h);
                ser_write(") parent=("); ser_dec(px); ser_write(","); ser_dec(py);
                ser_write(","); ser_dec(pw); ser_write(","); ser_dec(ph);
                ser_write(")\n");
            }
        }
        audit_walk(c);
    }
}

static void gui_probe_audit(const char *comp) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:audit comp=none zero=0 overflow=0 reason=mode\n");
        con_set_gui_mode(0);
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();

    if (!strcmp_probe(comp, "notepad")) gui_notepad_open(0);
    else if (!strcmp_probe(comp, "canvas")) gui_canvas_demo();
    else if (!strcmp_probe(comp, "files")) gui_filemgr_open(0);
    else if (!strcmp_probe(comp, "images")) gui_images_open(0);
    else if (!strcmp_probe(comp, "browser")) gui_browser_probe_sample();
    else if (!strcmp_probe(comp, "settings")) gui_settings_open();
    else if (!strcmp_probe(comp, "term")) gui_term_open();

    layout_run();
    gui_frame();
    audit_zero_n = 0; audit_over_n = 0;
    audit_walk(gui_root_block());
    ser_write("GUI:audit comp="); ser_write(comp ? comp : "none");
    ser_write(" zero="); ser_dec((uint32_t)audit_zero_n);
    ser_write(" overflow="); ser_dec((uint32_t)audit_over_n);
    ser_write("\n");
    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
}

void gui_probe_home(void) {
    gui_begin();
    gui_demo_build();
    gui_frame();                 /* 首帧全量合成，打点 GUI:frame first=1 */
    uint32_t us = stat_last_frame_us();
    ser_write("GUI:home frame_us="); ser_dec(us);
    ser_write(" result="); ser_dec(us <= 8000u ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_menu(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    gui_menu_open();
    layout_run();
    int overlay = 0;
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b && b->layer == LAYER_OVERLAY) overlay++;
    }
    int items = 0;
    block_id menu_ids[32];
    items = menu_collect(menu_ids, 32);

    /* 触发 Launch demo 条目：应新增一个示例块并关闭菜单。 */
    block *mr = btable_get(menu_root);
    block *launch = 0;
    if (mr) for (block *c = mr->first_child; c; c = c->next_sibling) launch = c;
    if (launch && launch->on_click) launch->on_click(launch->id, 0, 0);
    layout_run();
    int work_after = 0;
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b && b->layer == LAYER_WORK && b->focusable) work_after++;
    }

    ser_write("GUI:menu open="); ser_dec(menu_open);
    ser_write(" items="); ser_dec(items);
    ser_write(" overlay="); ser_dec(overlay);
    ser_write(" launched="); ser_dec(work_after);
    ser_write(" result="); ser_dec((overlay > 0 && work_after == 2) ? 1 : 0);
    ser_write("\n");

    gui_menu_close();
    gui_end();
}

/* 菜单条目点击探针：开菜单 → 命中 Notepad 行（复用真实按下/命中路径）→
 * 断言记事本被激活；菜单随点击关闭后下一帧损伤面积必须 >0（旧菜单像素
 * 被底层重画掉，不留残影）。 */
void gui_probe_menuclick(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    int active_before = gui_notepad_active();
    gui_menu_open();
    layout_run();

    /* Notepad 条目：应用行 z = -1。 */
    block *mr = btable_get(menu_root);
    block *row = 0;
    if (mr) for (block *c = mr->first_child; c; c = c->next_sibling)
        if (c->z == -1) { row = c; break; }

    int pressed = 0;
    if (row) {
        int cx = row->x + row->w / 2;
        int cy = row->y + row->h / 2;
        gui_inject_press(cx, cy);
        gui_inject_release();
        pressed = 1;
    }
    int active = gui_notepad_active();

    gui_frame();   /* 关菜单后一帧：损伤面积 > 0 才说明旧菜单区域被重画。 */
    uint32_t area = stat_damage_area();
    /* 稳态（菜单已关）块计数基线：第二轮开菜单 → 点 z>0 行 → 关菜单后，
     * 示例块不得被回收，稳态 live 必须与本轮前相同。 */
    uint32_t live_before = stat_blocks_live();

    /* z>0 行（示例块 focusable）走 focus_raise：摘挂 + z 递增后块必须仍挂在
     * 根树上（防摘树重挂丢失）、仍可见、仍可命中。菜单关闭会释放菜单行，
     * 全局块表计数会回落，故这里沿父链验证目标块仍挂到根，不比全局计数。 */
    int raise_pressed = 0, raise_live = 0, raise_visible = 0, raise_hit = 0;
    gui_menu_open();
    layout_run();
    block *mr2 = btable_get(menu_root);
    block *srow = 0;
    if (mr2) for (block *c = mr2->first_child; c; c = c->next_sibling)
        if (c->z > 0) { srow = c; break; }
    if (srow) {
        int cx = srow->x + srow->w / 2;
        int cy = srow->y + srow->h / 2;
        gui_inject_press(cx, cy);
        gui_inject_release();
        raise_pressed = 1;
        block *sample = btable_get((block_id)srow->z);
        if (sample) {
            block *up = sample;
            while (up->parent) up = up->parent;
            raise_live = (up == gui_root_block()) ? 1 : 0;
            raise_visible = sample->visible ? 1 : 0;
            int hx = sample->x + sample->w / 2;
            int hy = sample->y + sample->h / 2;
            raise_hit = gui_hit_test(hx, hy) != 0 ? 1 : 0;
        }
    }
    gui_frame();
    uint32_t live_after = stat_blocks_live();
    int raise_ok = raise_pressed && raise_live && raise_visible && raise_hit;

    /* 注：菜单开关会释放/重建菜单行，全局 live 前后不可直接相等比较；
     * 示例块存活以 raise_live/raise_visible/raise_hit 为准，live 前后值仅作遥测。 */
    int ok = pressed && active_before == 0 && active == 1 && area > 0 && raise_ok;
    ser_write("GUI:menuclick pressed="); ser_dec(pressed);
    ser_write(" active="); ser_dec(active);
    ser_write(" damage_area="); ser_dec(area);
    ser_write(" raise_live="); ser_dec(raise_live);
    ser_write(" raise_visible="); ser_dec(raise_visible);
    ser_write(" raise_hit="); ser_dec(raise_hit);
    ser_write(" live_before="); ser_dec(live_before);
    ser_write(" live_after="); ser_dec(live_after);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* A7.3 最近邻缩放 + 逆映射探针：320×240 源铺满 640×480 块，四角采样 = 源四角；
 * 四角/中心逆映射误差 0；回打合成帧耗时（µs）。 */
static void gui_probe_scaled(void) {
    gui_begin();
    con_request_mode(CON_MODE_VBE24, 0);
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    blk_spec cs = spec_panel(LAYER_WORK, 640, 480, 0);
    block_id cid = b_canvas(0, &cs, 320, 240);
    block *cb = btable_get(cid);
    if (cb) { cb->req_w = 640; cb->req_h = 480; cb->fit = FIT_FILL; }

    /* 源四角 + 中心，互不相同的纯色。 */
    cv_pixel(cid,   0,   0, 0xFF0000u);   /* TL 红 */
    cv_pixel(cid, 319,   0, 0x00FF00u);   /* TR 绿 */
    cv_pixel(cid,   0, 239, 0x0000FFu);   /* BL 蓝 */
    cv_pixel(cid, 319, 239, 0xFFFF00u);   /* BR 黄 */
    cv_pixel(cid, 160, 120, 0xFFFFFFu);   /* 中心 白 */

    layout_run();
    gui_mark_dirty(cid);
    gui_frame();
    uint32_t us = stat_last_frame_us();

    int bx = cb ? cb->x : 0, by = cb ? cb->y : 0;
    int bw = cb ? cb->w : 0, bh = cb ? cb->h : 0;
    int fitv = cb ? cb->fit : -1;
    int tl = gfx24_getpx(bx, by) == 0xFF0000u;
    int tr = gfx24_getpx(bx + bw - 1, by) == 0x00FF00u;
    int bl = gfx24_getpx(bx, by + bh - 1) == 0x0000FFu;
    int br = gfx24_getpx(bx + bw - 1, by + bh - 1) == 0xFFFF00u;

    int sx, sy;
    int inv = 1;
    gui_fit_inv_map(cb, 0, 0, &sx, &sy); inv &= (sx == 0   && sy == 0);
    gui_fit_inv_map(cb, bw - 1, bh - 1, &sx, &sy); inv &= (sx == 319 && sy == 239);
    gui_fit_inv_map(cb, bw / 2, bh / 2, &sx, &sy); inv &= (sx == 160 && sy == 120);

    int ok = tl && tr && bl && br && inv && us <= 8000u;
    ser_write("GUI:scaled x="); ser_dec(bx);
    ser_write(" y="); ser_dec(by);
    ser_write(" w="); ser_dec(bw);
    ser_write(" h="); ser_dec(bh);
    ser_write(" fit="); ser_dec(fitv);
    ser_write(" tl="); ser_dec(tl);
    ser_write(" tr="); ser_dec(tr);
    ser_write(" bl="); ser_dec(bl);
    ser_write(" br="); ser_dec(br);
    ser_write(" inv="); ser_dec(inv);
    ser_write(" frame_us="); ser_dec(us);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* A7.2 松开命中触发探针：正常点按（press+release 同点）必须触发；按下后移出
 * 再在块外松手不得触发。 */
static void gui_probe_clickup(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    /* 正常点按：press + release 同点 -> Notepad 激活，菜单关闭。 */
    gui_menu_open();
    layout_run();
    block *mr = btable_get(menu_root);
    block *row = 0;
    if (mr) for (block *c = mr->first_child; c; c = c->next_sibling)
        if (c->z == -1) { row = c; break; }
    int fired_normal = 0, closed_normal = 0;
    if (row) {
        gui_inject_press(row->x + row->w / 2, row->y + row->h / 2);
        gui_inject_release();
        fired_normal = gui_notepad_active();
        closed_normal = !gui_menu_is_open();
    }

    /* 按下后移出菜单外再松手：armed 重算命中落在块外，不得触发。先关掉上一例
     * 留下的记事本，避免读到陈旧激活态。 */
    gui_notepad_close();
    gui_menu_open();
    layout_run();
    block *mr2 = btable_get(menu_root);
    block *row2 = 0;
    if (mr2) for (block *c = mr2->first_child; c; c = c->next_sibling)
        if (c->z == -1) { row2 = c; break; }
    int fired_out = 0;
    if (row2) {
        int cx = row2->x + row2->w / 2, cy = row2->y + row2->h / 2;
        gui_inject_press(cx, cy);
        gui_inject_move(400, 300);
        gui_inject_release_at(400, 300);
        fired_out = gui_notepad_active();
    }

    int ok = fired_normal == 1 && closed_normal && fired_out == 0;
    ser_write("GUI:clickup normal="); ser_dec(fired_normal);
    ser_write(" closed="); ser_dec(closed_normal);
    ser_write(" out="); ser_dec(fired_out);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* D 轮交互自检：点外部关闭 / Esc 关闭 / 方向键移动选中 / 回车执行选中项。 */
void gui_probe_menu_itest(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    /* D5a 点菜单外部（桌面底色块）关闭，不下发命中。 */
    gui_menu_open();
    layout_run();
    gui_inject_press(400, 300);
    gui_inject_release();
    int outside_closed = !gui_menu_is_open();

    /* D5b Esc 经菜单键盘钩子关闭。 */
    gui_menu_open();
    layout_run();
    menu_key(27, 0);
    int esc_closed = !gui_menu_is_open();

    /* D6a 方向键下移：选中索引必须由 0 变 1。 */
    gui_menu_open();
    layout_run();
    int sel_before = menu_sel;
    menu_key(KB_DOWN, 0);
    int sel_after = menu_sel;

    /* D6b 回车执行：定位 Notepad 行（z=-1）后回车，记事本激活且菜单关闭。 */
    int np_idx = -1;
    for (int i = 0; i < menu_row_n; i++) {
        block *r = btable_get(menu_row_ids[i]);
        if (r && r->z == -1) { np_idx = i; break; }
    }
    if (np_idx >= 0) { menu_sel = np_idx; menu_paint_sel(); }
    menu_key(10, 0);
    int enter_notepad = gui_notepad_active();
    int enter_closed  = !gui_menu_is_open();

    ser_write("GUI:itest outside_closed="); ser_dec(outside_closed);
    ser_write(" esc_closed="); ser_dec(esc_closed);
    ser_write(" sel_before="); ser_dec(sel_before);
    ser_write(" sel_after="); ser_dec(sel_after);
    ser_write(" enter_notepad="); ser_dec(enter_notepad);
    ser_write(" enter_closed="); ser_dec(enter_closed);
    ser_write("\n");
    gui_end();
}

void gui_probe_presence(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    uint32_t base = stat_blocks_live();

    blk_spec s = spec_panel(LAYER_WORK, 40, 30, 1);
    block_id q = b_stack(0, &s);
    layout_run();
    uint32_t added = stat_blocks_live();

    extern volatile uint32_t timer_ticks;
    b_remove(q);
    gui_frame();                 /* leaving 第一帧：句柄仍在块表 */
    block *qb = btable_get(q);
    uint32_t leaving = qb ? 1 : 0;
    uint32_t live_leaving = stat_blocks_live();
    timer_ticks++;               /* 跨过帧节拍，避免第二帧被同 tick 去重 */
    gui_frame();                 /* 下一帧回收，句柄归还空闲链 */
    uint32_t live_after = stat_blocks_live();
    uint32_t leaked = stat_blocks_leaked();

    ser_write("GUI:presence base="); ser_dec(base);
    ser_write(" added="); ser_dec(added);
    ser_write(" leaving="); ser_dec(leaving);
    ser_write(" live_leaving="); ser_dec(live_leaving);
    ser_write(" live_after="); ser_dec(live_after);
    ser_write(" leaked="); ser_dec(leaked);
    ser_write(" result=");
    ser_dec((live_after == base && leaked == 0 && leaving == 1) ? 1 : 0);
    ser_write("\n");
    gui_end();
}

/* diff 路径两阶段销毁：第二轮描述里删掉一个块，应先进入 leaving 帧
 * （句柄仍在块表、live 暂不回落），再过一帧才由 presence_sweep 回收。 */
void gui_probe_diff(void) {
    static blk_desc d_full[2], d_trim[1];
    static blk_spec sp_root, sp_child;

    gui_begin();
    gui_root_block()->w = 800;
    gui_root_block()->h = 600;

    memset(&sp_root, 0, sizeof(sp_root));
    sp_root.req_w = 200; sp_root.req_h = 120;
    sp_root.layer = LAYER_WORK; sp_root.visible = 1; sp_root.scale = 1;
    sp_root.fg = gui_active_style()->fg; sp_root.bg = gui_active_style()->bg;
    memset(&sp_child, 0, sizeof(sp_child));
    sp_child.req_w = 60; sp_child.req_h = 40;
    sp_child.layer = LAYER_WORK; sp_child.visible = 1; sp_child.scale = 1;
    sp_child.fg = gui_active_style()->fg; sp_child.bg = gui_active_style()->bg;

    d_full[0].id = 1; d_full[0].kind = BLK_STACK; d_full[0].parent = 0;
    d_full[0].spec = &sp_root; d_full[0].text = 0; d_full[0].pix = 0;
    d_full[1].id = 2; d_full[1].kind = BLK_RECT; d_full[1].parent = 1;
    d_full[1].spec = &sp_child; d_full[1].text = 0; d_full[1].pix = 0;
    gui_commit_desc(d_full, 2);
    layout_run();
    gui_frame();
    uint32_t base = stat_blocks_live();

    /* 第二轮描述只留根：id 2 本轮 unseen，应走 leaving 两阶段。 */
    d_trim[0] = d_full[0];
    gui_commit_desc(d_trim, 1);
    layout_run();
    timer_ticks++;               /* 跨帧节拍，避免本帧被同 tick 去重 */
    gui_frame();                 /* leaving 第一帧：句柄仍在块表 */
    block *gone = btable_get(2);
    uint32_t leaving = (gone && gone->leaving) ? 1 : 0;
    uint32_t live_leaving = stat_blocks_live();

    timer_ticks++;               /* 再跨一拍，第二帧由 sweep 回收 */
    gui_frame();
    uint32_t live_after = stat_blocks_live();
    uint32_t leaked = stat_blocks_leaked();

    ser_write("GUI:diff base="); ser_dec(base);
    ser_write(" leaving="); ser_dec(leaving);
    ser_write(" live_leaving="); ser_dec(live_leaving);
    ser_write(" live_after="); ser_dec(live_after);
    ser_write(" leaked="); ser_dec(leaked);
    int ok = leaving == 1 && live_leaving == base &&
             live_after == base - 1 && leaked == 0;
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_theme(void) {
    gui_begin();
    gui_demo_build();
    layout_run();

    gui_theme_set(TOK_DARK);
    const struct style *sd = gui_active_style();
    gui_theme_set(TOK_LIGHT);
    const struct style *sl = gui_active_style();
    gui_theme_set(TOK_HIGH_CONTRAST);
    const struct style *sh = gui_active_style();

    int ok = sd->bg != sl->bg && sl->fg != sh->fg && sd->accent != sh->accent;
    ser_write("GUI:theme dark_bg="); ser_hash(sd->bg);
    ser_write(" light_bg="); ser_hash(sl->bg);
    ser_write(" hc_bg="); ser_hash(sh->bg);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_reset(void) {
    gui_begin();
    gui_demo_build();
    layout_run();
    uint32_t nodes = tree_count_nodes();
    uint32_t h = gui_tree_hash();
    uint32_t live = stat_blocks_live();

    gui_reset();
    gui_demo_build();
    layout_run();
    uint32_t nodes2 = tree_count_nodes();
    uint32_t h2 = gui_tree_hash();
    uint32_t live2 = stat_blocks_live();
    uint32_t orphan = live2 > nodes2 ? live2 - nodes2 : 0;

    int ok = nodes == nodes2 && h == h2 && orphan == 0 && live == live2;
    (void)live;
    ser_write("GUI:reset nodes="); ser_dec(nodes2);
    ser_write(" hash="); ser_hash(h2);
    ser_write(" orphan="); ser_dec(orphan);
    ser_write(" match="); ser_dec(h == h2 ? 1 : 0);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

static int gesture_clicked;
static void probe_click_consume(block_id id, int32_t lx, int32_t ly) {
    gesture_clicked = 1;   /* 模拟应用块消费点击 */
}

void gui_probe_gesture(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    blk_spec s = spec_panel(LAYER_WORK, 120, 80, 1);
    s.focusable = 1;
    block_id A = b_stack(0, &s);
    block *pa = btable_get(A);
    pa->on_click = probe_click_consume;
    layout_run();
    int x0 = pa->x, y0 = pa->y;

    /* 标题栏拖拽：即便块挂了消费型 on_click，系统拖拽手势仍必须生效。 */
    gui_inject_press(10, 10);
    gui_inject_move(90, 70);
    gui_inject_release();
    int drag_moved = (pa->x != x0 || pa->y != y0);

    /* 任务栏属于系统层：点击仍命中任务栏，不被应用块消费拦截。 */
    blk_spec tb = spec_panel(LAYER_CHROME, GFX24_W, 32, 0);
    block_id tb_id = b_row(0, &tb);
    layout_run();
    block *tbar = btable_get(tb_id);
    block_id tb_hit = gui_hit_test(tbar->x + 10, tbar->y + 16);

    int ok = drag_moved && tb_hit == tb_id && gesture_clicked == 1;
    ser_write("GUI:gesture clicked="); ser_dec(gesture_clicked);
    ser_write(" drag_moved="); ser_dec(drag_moved);
    ser_write(" taskbar_hit="); ser_dec(tb_hit);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

void gui_probe_resize(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    blk_spec col = spec_panel(LAYER_WORK, 120, 80, 1);
    block_id A = b_column(0, &col);
    blk_spec leaf = spec_panel(LAYER_CONTENT, 60, 40, 0);
    block_id B = b_rect(A, &leaf);
    blk_spec cs = spec_panel(LAYER_WORK, 60, 40, 1);
    block_id C = b_stack(0, &cs);
    layout_run();

    block *pa = btable_get(A);
    block *pc = btable_get(C);
    int c0x = pc->x, c0y = pc->y;
    int aw0 = pa->w, ah0 = pa->h;

    /* 右下角 resize 手柄：按压角落拖放，改 req_w/req_h，只重排该子树。 */
    int hx = pa->x + pa->w - 6, hy = pa->y + pa->h - 6;
    gui_inject_press(hx, hy);
    gui_inject_move(pa->x + aw0 + 40, pa->y + ah0 + 30);
    gui_inject_release();
    layout_run();

    int grew = pa->w > aw0 && pa->h > ah0;
    int sibling_same = pc->x == c0x && pc->y == c0y;
    block *pb = btable_get(B);
    int leaf_inside = pb->x >= pa->x && pb->y >= pa->y &&
                      pb->x + pb->w <= pa->x + pa->w &&
                      pb->y + pb->h <= pa->y + pa->h;
    int ok = grew && sibling_same && leaf_inside;
    ser_write("GUI:resize w="); ser_dec(pa->w);
    ser_write(" h="); ser_dec(pa->h);
    ser_write(" sibling_same="); ser_dec(sibling_same);
    ser_write(" leaf_inside="); ser_dec(leaf_inside);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* ---- v2 自由布局 / 溢出 / 交互探针 -------------------------------------- */

#define FREE_CHK(blk, ex, ey, ew, eh) \
    ((blk)->x == (ex) && (blk)->y == (ey) && \
     (blk)->w == (ew) && (blk)->h == (eh))

static void gui_probe_free(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    blk_spec fr = spec_panel(LAYER_CONTENT, GFX24_W, GFX24_H, 0);
    block_id Fr = b_free(0, &fr);

    /* A：左上锚点 + 像素偏移 20,20，120×80。 */
    blk_spec a = spec_panel(LAYER_CONTENT, 120, 80, 1);
    a.anchor = ANC_L | ANC_T; a.px_x = 20; a.px_y = 20;
    block_id A = b_free(Fr, &a);

    /* B：右上锚点，宽 25% 父框（200），高 80。 */
    blk_spec b = spec_panel(LAYER_CONTENT, 0, 80, 1);
    b.anchor = ANC_R | ANC_T; b.pct_w = 250;
    block_id B = b_free(Fr, &b);

    /* C：水平 + 垂直居中，100×60。 */
    blk_spec c = spec_panel(LAYER_CONTENT, 100, 60, 1);
    c.anchor = ANC_HCENTER | ANC_VCENTER;
    block_id C = b_free(Fr, &c);

    /* D：百分比定位 50%,50%，尺寸 30%×30%（240×180）。 */
    blk_spec d = spec_panel(LAYER_CONTENT, 0, 0, 1);
    d.pct_x = 500; d.pct_y = 500; d.pct_w = 300; d.pct_h = 300;
    block_id D = b_free(Fr, &d);

    /* E：min/max clamp：req 400 被 max_w=120 收到 120，底边锚定。 */
    blk_spec e = spec_panel(LAYER_CONTENT, 400, 60, 1);
    e.max_w = 120; e.anchor = ANC_L | ANC_B;
    block_id E = b_free(Fr, &e);

    layout_run();
    block *pa = btable_get(A), *pb = btable_get(B), *pc = btable_get(C);
    block *pd = btable_get(D), *pe = btable_get(E);

    int ok = FREE_CHK(pa, 20, 20, 120, 80) &&
             FREE_CHK(pb, 600, 0, 200, 80) &&
             FREE_CHK(pc, 350, 270, 100, 60) &&
             FREE_CHK(pd, 400, 300, 240, 180) &&
             FREE_CHK(pe, 0, 540, 120, 60);

    ser_write("GUI:free A=("); ser_dec(pa->x); ser_write(","); ser_dec(pa->y);
    ser_write(","); ser_dec(pa->w); ser_write(","); ser_dec(pa->h); ser_write(")");
    ser_write(" B=("); ser_dec(pb->x); ser_write(","); ser_dec(pb->y);
    ser_write(","); ser_dec(pb->w); ser_write(","); ser_dec(pb->h); ser_write(")");
    ser_write(" C=("); ser_dec(pc->x); ser_write(","); ser_dec(pc->y);
    ser_write(","); ser_dec(pc->w); ser_write(","); ser_dec(pc->h); ser_write(")");
    ser_write(" D=("); ser_dec(pd->x); ser_write(","); ser_dec(pd->y);
    ser_write(","); ser_dec(pd->w); ser_write(","); ser_dec(pd->h); ser_write(")");
    ser_write(" E=("); ser_dec(pe->x); ser_write(","); ser_dec(pe->y);
    ser_write(","); ser_dec(pe->w); ser_write(","); ser_dec(pe->h); ser_write(")");
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

static void gui_probe_cvdirty(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    uint32_t bgc = gui_active_style()->bg;
    uint32_t acc = gui_active_style()->accent;
    uint8_t ar = (uint8_t)((acc >> 16) & 0xFFu);
    uint8_t ag = (uint8_t)((acc >> 8) & 0xFFu);
    uint8_t ab = (uint8_t)(acc & 0xFFu);

    block_id cid = b_canvas(0, 0, 64, 48);
    block *b = btable_get(cid);
    cv_fill_rect(cid, 0, 0, 64, 48, bgc);
    cv_dirty(cid, 10, 10, 20, 20);
    cv_fill_rect(cid, 10, 10, 20, 20, acc);
    gui_frame();

    /* 子矩形外不得出现强调色像素；子矩形内必须是强调色。 */
    int outside_bad = 0, inside_miss = 0;
    uint8_t *buf = b->buf;
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t *p = buf + (y * 64 + x) * 3;
            int blue = (p[0] == ar && p[1] == ag && p[2] == ab);
            int inside = x >= 10 && x < 30 && y >= 10 && y < 30;
            if (!inside && blue) outside_bad++;
            if (inside && !blue) inside_miss++;
        }
    }
    int ok = outside_bad == 0 && inside_miss == 0;
    ser_write("GUI:cvdirty outside_bad="); ser_dec(outside_bad);
    ser_write(" inside_miss="); ser_dec(inside_miss);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

static void gui_probe_interact(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    blk_spec par = spec_panel(LAYER_WORK, 300, 200, 1);
    block_id P = b_free(0, &par);
    block *pp = btable_get(P);
    pp->x = 50; pp->y = 50; pp->moved = 1;

    /* 8 方向可缩放块：min 40，max 200。 */
    blk_spec r = spec_panel(LAYER_WORK, 100, 80, 1);
    r.resize_dir = RESIZE_L | RESIZE_R | RESIZE_T | RESIZE_B;
    r.resize_min_w = 40; r.resize_min_h = 40;
    r.resize_max_w = 200; r.resize_max_h = 200;
    block_id R = b_rect(P, &r);
    block *pr = btable_get(R);

    /* 父框约束拖拽块（右下角，避开可缩放块 R）。 */
    blk_spec g = spec_panel(LAYER_WORK, 60, 60, 1);
    g.drag_bounds = BOUND_PARENT; g.drag_mode = DRAG_FULL;
    g.anchor = ANC_R | ANC_B;
    block_id G = b_rect(P, &g);
    block *pg = btable_get(G);

    layout_run();

    /* 同构断言：解析归属与真实 mousedown/move/up 走同一 resolve_interaction。 */
    block_id ract = 0, gact = 0; uint8_t rdir = 0, gdir = 0;
    int rm = gui_interaction_at(pr->x + pr->w - 3, pr->y + pr->h / 2, &ract, &rdir);
    int gm = gui_interaction_at(pg->x + 10, pg->y + 10, &gact, &gdir);
    int resolve_r = (rm == GUI_IT_RESIZE && ract == R && (rdir & RESIZE_R));
    int resolve_g = (gm == GUI_IT_MOVE && gact == G);

    int w0 = pr->w;

    /* 右边缘 +50。 */
    gui_inject_press(pr->x + pr->w - 3, pr->y + pr->h / 2);
    gui_inject_move(pr->x + pr->w + 50, pr->y + pr->h / 2);
    gui_inject_release();
    int grew_r = pr->w > w0;

    /* 左边缘左移 30：宽增大、原点左移。 */
    int lw = pr->w, lx = pr->x;
    gui_inject_press(pr->x + 3, pr->y + pr->h / 2);
    gui_inject_move(pr->x - 30, pr->y + pr->h / 2);
    gui_inject_release();
    int grew_l = pr->w > lw && pr->x < lx;

    /* 下边缘 +30。 */
    int bh = pr->h;
    gui_inject_press(pr->x + pr->w / 2, pr->y + pr->h - 3);
    gui_inject_move(pr->x + pr->w / 2, pr->y + pr->h + 30);
    gui_inject_release();
    int grew_b = pr->h > bh;

    /* 上边缘上移 20：高增大、原点上移。 */
    int th = pr->h, ty = pr->y;
    gui_inject_press(pr->x + pr->w / 2, pr->y + 3);
    gui_inject_move(pr->x + pr->w / 2, pr->y - 20);
    gui_inject_release();
    int grew_t = pr->h > th && pr->y < ty;

    /* min clamp：右下拖到极小，宽高不得低于 40。 */
    gui_inject_press(pr->x + pr->w - 3, pr->y + pr->h - 3);
    gui_inject_move(pr->x + 5, pr->y + 5);
    gui_inject_release();
    int min_ok = pr->w >= 40 && pr->h >= 40;

    /* max clamp：右边缘继续拉宽，宽不得超过 200。 */
    gui_inject_press(pr->x + pr->w - 3, pr->y + pr->h / 2);
    gui_inject_move(pr->x + 400, pr->y + pr->h / 2);
    gui_inject_release();
    int max_ok = pr->w <= 200;

    /* 父框约束拖拽：拖向父框外，必须夹回父框内。 */
    gui_inject_press(pg->x + 10, pg->y + 10);
    gui_inject_move(pp->x - 60, pp->y - 60);
    gui_inject_release();
    int clamped = pg->x >= pp->x && pg->y >= pp->y &&
                  pg->x + pg->w <= pp->x + pp->w &&
                  pg->y + pg->h <= pp->y + pp->h;

    /* 偏移跨帧保持：再跑一轮布局，位置不得被重置。 */
    int gx = pg->x, gy = pg->y;
    layout_run();
    int persist = pg->x == gx && pg->y == gy;

    uint32_t measured = stat_layout_nodes();
    uint32_t total = tree_count_nodes();
    int ok = grew_r && grew_l && grew_b && grew_t && min_ok && max_ok &&
             clamped && persist && measured <= total && resolve_r && resolve_g;

    ser_write("GUI:interact grew_r="); ser_dec(grew_r);
    ser_write(" grew_l="); ser_dec(grew_l);
    ser_write(" grew_b="); ser_dec(grew_b);
    ser_write(" grew_t="); ser_dec(grew_t);
    ser_write(" min_ok="); ser_dec(min_ok);
    ser_write(" max_ok="); ser_dec(max_ok);
    ser_write(" clamped="); ser_dec(clamped);
    ser_write(" persist="); ser_dec(persist);
    ser_write(" resolve_r="); ser_dec(resolve_r);
    ser_write(" resolve_g="); ser_dec(resolve_g);
    ser_write(" measured="); ser_dec(measured);
    ser_write(" total="); ser_dec(total);
    ser_write(" result="); ser_dec(ok ? 1 : 0); ser_write("\n");
    gui_end();
}

/* ---- 组件层自检探针 ------------------------------------------------------ */

static void gui_probe_note(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_notepad_open("/notes.txt");
    gui_frame();
    ser_write("GUI:note lines="); ser_dec(gui_notepad_lines());
    ser_write(" col="); ser_dec(gui_notepad_col());
    ser_write(" dirty="); ser_dec(gui_notepad_dirty());
    ser_write(" full="); ser_dec(gui_notepad_full());
    ser_write(" result="); ser_dec(gui_notepad_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

/* 终端滚动自检：灌 >屏 输出；mid=1 滚轮上翻到中段留屏，mid=0 贴底跟随。
 * 串口打印 SCROLL:term top/total，供回归与截图核验。 */
static void gui_probe_term_scroll(int mid) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();

    gui_term_open();
    for (int i = 0; i < 30; i++) gui_term_run(0, "echo scroll-line");
    block *tb = btable_get(gui_term_root(0));
    int tcx = tb ? tb->x + tb->w / 2 : 0, tcy = tb ? tb->y + tb->h / 2 : 0;
    gui_frame();
    ser_write("SCROLL:term seed total="); ser_dec(gui_term_lines(0));
    ser_write(" follow="); ser_dec(gui_term_follow(0)); ser_write("\n");
    if (mid) {
        gui_inject_wheel(tcx, tcy, -1);
        gui_inject_wheel(tcx, tcy, -1);
        gui_frame();
        ser_write("SCROLL:term up top="); ser_dec(gui_term_scroll_top(0));
        ser_write(" total="); ser_dec(gui_term_lines(0));
        ser_write(" follow="); ser_dec(gui_term_follow(0)); ser_write("\n");
    } else {
        gui_frame();
        ser_write("SCROLL:term bottom top="); ser_dec(gui_term_scroll_top(0));
        ser_write(" total="); ser_dec(gui_term_lines(0));
        ser_write(" follow="); ser_dec(gui_term_follow(0)); ser_write("\n");
    }
    gui_end();
}

/* 记事本滚动自检：灌 60 行，滚轮上翻到中段留屏；串口 SCROLL:note top/total。 */
static void gui_probe_note_scroll(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();

    gui_notepad_open("/notes.txt");
    for (int i = 0; i < 59; i++) { gui_inject_key(10); gui_inject_key('x'); }
    block *nb = btable_get(gui_notepad_root());
    int ncx = nb ? nb->x + nb->w / 2 : 0, ncy = nb ? nb->y + nb->h / 2 : 0;
    gui_frame();
    ser_write("SCROLL:note seed total="); ser_dec(gui_notepad_lines()); ser_write("\n");
    gui_inject_wheel(ncx, ncy, -1);
    gui_inject_wheel(ncx, ncy, -1);
    gui_frame();
    ser_write("SCROLL:note up top="); ser_dec(gui_notepad_scroll_top());
    ser_write(" total="); ser_dec(gui_notepad_lines()); ser_write("\n");
    gui_end();
}

static void gui_probe_cv(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_canvas_demo();
    gui_frame();
    ser_write("GUI:canvas w="); ser_dec(gui_canvas_w());
    ser_write(" h="); ser_dec(gui_canvas_h());
    ser_write(" active="); ser_dec(gui_canvas_active());
    ser_write(" result="); ser_dec(gui_canvas_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

static void gui_probe_fm(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_filemgr_open("/");
    gui_frame();
    ser_write("GUI:fm entries="); ser_dec(gui_filemgr_count());
    ser_write(" active="); ser_dec(gui_filemgr_active());
    ser_write(" result="); ser_dec(gui_filemgr_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

static void gui_probe_img(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_images_open("/samples/desktop.bmp");
    gui_frame();
    ser_write("GUI:img file="); ser_write(gui_images_name());
    ser_write(" w="); ser_dec(gui_images_w());
    ser_write(" h="); ser_dec(gui_images_h());
    ser_write(" ok="); ser_dec(gui_images_ok());
    ser_write(" result="); ser_dec(gui_images_ok() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

static void gui_probe_br(void) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_browser_probe_sample();
    gui_frame();
    ser_write("GUI:br bytes="); ser_dec(gui_browser_bytes());
    ser_write(" nodes="); ser_dec(gui_browser_nodes());
    ser_write(" blocks="); ser_dec(gui_browser_blocks());
    ser_write(" active="); ser_dec(gui_browser_active());
    ser_write(" result="); ser_dec(gui_browser_active() ? 1 : 0);
    ser_write("\n");
    gui_end();
}

/* C2 浏览器取证探针族：edit 地址栏编辑 / hist 后退前进 / error 错误页 /
 * two 双实例历史独立。串口打点见 BROWSER:open/nav/hist 与 BR:* 行。 */
static void gui_probe_brcase(const char *mode) {
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;

    if (!strcmp_probe(mode, "edit")) {
        gui_browser_probe_sample();
        gui_br_type(0, "example.com/x");
        const char *a = gui_br_addr(0);
        ser_write("BR:edit addr="); ser_write(a);
        ser_write(" len="); ser_dec(tb_xlen(a));
        ser_write(" active="); ser_dec(gui_browser_active());
        ser_write("\n");
    } else if (!strcmp_probe(mode, "hist")) {
        gui_browser_probe_sample();
        gui_br_type(0, "sample://page2");
        gui_br_submit(0);
        ser_write("BR:hist after-nav len="); ser_dec(gui_br_hist_len(0));
        ser_write(" url="); ser_write(gui_br_url(0)); ser_write("\n");
        gui_br_back(0);
        ser_write("BR:back url="); ser_write(gui_br_url(0));
        ser_write(" hist="); ser_dec(gui_br_hist_len(0)); ser_write("\n");
        gui_br_fwd(0);
        ser_write("BR:fwd url="); ser_write(gui_br_url(0));
        ser_write(" hist="); ser_dec(gui_br_hist_len(0)); ser_write("\n");
    } else if (!strcmp_probe(mode, "error")) {
        gui_browser_probe_sample();
        gui_br_type(0, "http://127.0.0.1:9/nope");
        gui_br_submit(0);
        ser_write("BR:error active="); ser_dec(gui_browser_active());
        ser_write(" url="); ser_write(gui_br_url(0));
        ser_write(" hist="); ser_dec(gui_br_hist_len(0));
        ser_write("\n");
    } else if (!strcmp_probe(mode, "two")) {
        gui_browser_probe_sample();
        gui_br_open_sample("sample://page2");
        for (int i = 0; i < 2; i++) {
            ser_write("BROWSER:inst idx="); ser_dec((uint32_t)i);
            ser_write(" url="); ser_write(gui_br_url(i));
            ser_write(" hist="); ser_dec(gui_br_hist_len(i));
            ser_write(" active="); ser_dec(gui_br_active_idx() == i ? 1 : 0);
            ser_write("\n");
        }
    }
    layout_run();
    gui_frame();
    gui_end();
}

/* ---- style 档位探针（app / calm） --------------------------------------- */

static int style_contains(const char *s, const char *needle) {
    if (!s || !needle) return 0;
    for (; *s; s++) {
        const char *a = s, *b = needle;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static uint32_t probe_read_px(int x, int y) {
    volatile uint8_t *fb = gfx24_lfb_ptr();
    if (!fb || x < 0 || y < 0 || x >= GFX24_W || y >= GFX24_H) return 0;
    uint32_t pitch = gfx24_pitch();
    uint8_t stride = gfx24_stride();
    const uint8_t *p = fb + (uint32_t)y * pitch + (uint32_t)x * stride;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static block *probe_find_taskbar(void) {
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b && b->layer == LAYER_CHROME && b->bleed) return b;
    }
    return 0;
}

static block *probe_find_panel_body(void) {
    /* 选面积最小的非标题带 WORK 块：避开含标题行的面板根，落到面板体。 */
    block *best = 0; int best_area = (1 << 30);
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (!b || b->layer != LAYER_WORK) continue;
        if (b->st.role_bg == ROLE_BAR) continue;
        if (b->w < 40 || b->h < 24) continue;
        int area = b->w * b->h;
        if (area < best_area) { best = b; best_area = area; }
    }
    return best;
}

static void gui_probe_style(const char *mode) {
    int want_calm = mode && style_contains(mode, "calm");
    /* 像素读数必须在 gfx24 后端上：探针在文本控制台里直跑时 LFB 为空，
     * 读数恒 0；先进入 VBE24，收尾再强制回文本控制台。 */
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        con_set_gui_mode(0);
        ser_write("GUI:style mode="); ser_write(want_calm ? "calm" : "app");
        ser_write(" file=0 taskbar=0 panel_corner=0 residue=0 result=0\n");
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();

    gui_style_set(want_calm);
    layout_run();
    gui_frame();

    char buf[64]; buf[0] = 0;
    fs_read_text("/etc/style", buf, sizeof(buf) - 1);
    int file_ok = want_calm ? style_contains(buf, "calm")
                            : style_contains(buf, "app");

    const struct style *tok = gui_active_style();
    block *tb = probe_find_taskbar();
    int tb_ok = 0;
    if (tb) {
        uint32_t px = probe_read_px(tb->x + 4, tb->y + 4);
        tb_ok = (px == tok->bar);   /* full-bleed 任务栏满铺（取内行，避开边缘描边） */
    }

    block *panel = probe_find_panel_body();
    int corner_cut = 0;
    if (panel && want_calm) {
        uint32_t outer = probe_read_px(panel->x, panel->y);
        /* calm 外角被超椭圆裁成背景色；app 不圆角，外角不判背景。 */
        corner_cut = (outer == tok->bg);
    }

    /* 连切两次回到目标档位：内填不得残留上一档形状。 */
    gui_style_set(!want_calm);
    layout_run(); gui_frame();
    gui_style_set(want_calm);
    layout_run(); gui_frame();
    int residue = 1;
    if (panel) {
        /* 取面板体中点，避开圆角切口与边缘描边，校验内填未残留上一档。 */
        uint32_t inner = probe_read_px(panel->x + panel->w / 2, panel->y + panel->h / 2);
        residue = (inner == tok->panel);
    }

    int result = file_ok && tb_ok && residue &&
                 (!want_calm || corner_cut);
    ser_write("GUI:style mode="); ser_write(want_calm ? "calm" : "app");
    ser_write(" file="); ser_dec(file_ok);
    ser_write(" taskbar="); ser_dec(tb_ok);
    ser_write(" panel_corner="); ser_dec(corner_cut);
    ser_write(" residue="); ser_dec(residue);
    ser_write(" result="); ser_dec(result);
    ser_write("\n");
    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
}

static void gui_probe_term(void) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:term count=0 result=0\n");
        con_set_gui_mode(0);
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();

    gui_term_open();
    gui_term_run(0, "cd /");
    gui_term_run(0, "ls");
    gui_term_run(0, "help");
    gui_term_run(0, "cat /etc/lang");
    gui_term_open();
    gui_term_run(1, "mkdir /work");
    gui_term_run(1, "cd /work");
    gui_term_run(1, "ls");

    int n = gui_term_count();
    const char *c0 = gui_term_cwd(0);
    const char *c1 = gui_term_cwd(1);
    const char *l0 = gui_term_last_line(0);
    const char *l1 = gui_term_last_line(1);
    int two = n >= 2;
    int cwd_diff = strcmp_probe(c0, c1) != 0;
    int line_diff = strcmp_probe(l0, l1) != 0;

    /* shell_exec 复用证据：help 首行标题命中命令清单；cat /etc/lang 有回显。 */
    int help_ok = 0, lang_ok = 0;
    for (int k = 0; k < gui_term_lines(0); k++) {
        const char *ln = gui_term_line(0, k);
        if (style_contains(ln, "command")) help_ok = 1;
        if (style_contains(ln, "lang") || style_contains(ln, "cat")) lang_ok = 1;
    }
    int result = two && cwd_diff && line_diff && help_ok && lang_ok;

    ser_write("GUI:term count="); ser_dec(n);
    ser_write(" cwd0="); ser_write(c0);
    ser_write(" cwd1="); ser_write(c1);
    ser_write(" iso="); ser_dec(cwd_diff && line_diff);
    ser_write(" help="); ser_dec(help_ok);
    ser_write(" lang="); ser_dec(lang_ok);
    ser_write(" result="); ser_dec(result);
    ser_write("\n");

    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
}

/* R5 桌面拖拽 + 持久化确定性探针：播种桌面后，内核注入一次拖拽把条目 0
 * 从兜底位拖到 (180,180)；松手落盘后从持久层读回比对。结果串行打印：
 *   GUI:r5desk n=.. before=(x,y) after=(x,y) saved=(x,y) moved=.. saved=.. reload=.. result=..
 * 探针保留 GUI 帧供 HMP screendump 取证。 */
static void gui_probe_r5desk(void) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:r5desk n=0 result=0 reason=mode\n");
        con_set_gui_mode(0);
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();

    if (desk_n < 1) {
        ser_write("GUI:r5desk n=0 result=0 reason=empty\n");
        return;
    }
    block *b = btable_get(desk_item_blk[0]);
    int bx0 = b->x, by0 = b->y;
    gui_inject_press(b->x + 20, b->y + 20);
    gui_inject_move(180, 180);
    gui_inject_release();
    layout_run();
    gui_frame();
    int bx1 = b->x, by1 = b->y;
    int moved = (bx1 != bx0 || by1 != by0);

    desk_layout_load();   /* 从持久层重新读回，核验落盘内容 */
    int sx = -1, sy = -1, saved = desk_save_lookup(desk_names[0], &sx, &sy);
    int reload = saved && sx == bx1 && sy == by1;
    int result = moved && saved && reload;

    ser_write("GUI:r5desk n="); ser_dec((uint32_t)desk_n);
    ser_write(" before=("); ser_dec((uint32_t)bx0); ser_write(","); ser_dec((uint32_t)by0);
    ser_write(") after=("); ser_dec((uint32_t)bx1); ser_write(","); ser_dec((uint32_t)by1);
    ser_write(") saved=("); ser_dec((uint32_t)sx); ser_write(","); ser_dec((uint32_t)sy);
    ser_write(") moved="); ser_dec((uint32_t)moved);
    ser_write(" saved="); ser_dec((uint32_t)saved);
    ser_write(" reload="); ser_dec((uint32_t)reload);
    ser_write(" result="); ser_dec((uint32_t)result);
    ser_write("\n");
}

/* R5 双击探针：桌面找到首个 .ce/.exe 条目，内核注入两次快速按下/松开，
 * 触发 on_dblclick；串行回打 `GUI:desk dbl run=...`。保留帧供截图。 */
static void gui_probe_r5dbl(void) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:r5dbl result=0 reason=mode\n");
        con_set_gui_mode(0);
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();

    int idx = -1;
    for (int i = 0; i < desk_n; i++) {
        if (desk_ext_is(desk_names[i], "ce") || desk_ext_is(desk_names[i], "exe")) {
            idx = i; break;
        }
    }
    if (idx < 0) {
        ser_write("GUI:r5dbl result=0 reason=no-ce\n");
        return;
    }
    block *b = btable_get(desk_item_blk[idx]);
    int cx = b->x + 20, cy = b->y + 20;
    gui_inject_press(cx, cy); gui_inject_release();
    gui_inject_press(cx, cy); gui_inject_release();
    layout_run();
    gui_frame();
    ser_write("GUI:r5dbl idx="); ser_dec((uint32_t)idx);
    ser_write(" name="); ser_write(desk_names[idx]);
    ser_write("\n");
}

/* 多窗口隔离取证探针：`gui mw <term|fm|focus|cap|close>`。各模式自建帧并保留
 * 供 screendump；串口打点见 REPORT.md。复用真实 open/focus/close 路径。 */
static void gui_probe_mw(const char *mode) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("MW:result=0 reason=mode\n");
        con_set_gui_mode(0);
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();

    if (!strcmp_probe(mode, "term")) {
        gui_term_open();
        gui_term_run(0, "mkdir /work");
        gui_term_run(0, "cd /work");
        gui_term_open();
        gui_term_run(1, "ls");
        gui_term_type(0, "hel");              /* A 行缓冲未提交 */
        gui_term_focus(1);                     /* 真实点击聚焦路径：切 hook + raise */
        gui_inject_key('x'); gui_inject_key('y');   /* 键入应落 B */
        const char *c0 = gui_term_cwd(0);
        const char *c1 = gui_term_cwd(1);
        const char *b0 = gui_term_line_buf(0);
        const char *b1 = gui_term_line_buf(1);
        int h0 = gui_term_hist_n(0);
        int h1 = gui_term_hist_n(1);
        int iso = (strcmp_probe(c0, "/work") == 0 && strcmp_probe(c1, "/") == 0 &&
                   strcmp_probe(b0, "hel") == 0 && strcmp_probe(b1, "xy") == 0);
        ser_write("MW:term cwd0="); ser_write(c0);
        ser_write(" cwd1="); ser_write(c1);
        ser_write(" buf0="); ser_write(b0);
        ser_write(" buf1="); ser_write(b1);
        ser_write(" hist0="); ser_dec(h0);
        ser_write(" hist1="); ser_dec(h1);
        ser_write(" iso="); ser_dec(iso); ser_write("\n");
    } else if (!strcmp_probe(mode, "fm")) {
        gui_filemgr_open("/etc");
        gui_filemgr_open("/");
        int n0 = gui_fm_count_inst(0);
        if (n0 > 4) gui_fm_select(0, 4);      /* A 选中第 5 行 */
        const char *c0 = gui_fm_cwd_inst(0);
        const char *c1 = gui_fm_cwd_inst(1);
        int n1 = gui_fm_count_inst(1);
        int s0 = gui_fm_sel_inst(0);
        int s1 = gui_fm_sel_inst(1);
        int iso = (strcmp_probe(c0, "/etc") == 0 && strcmp_probe(c1, "/") == 0 && s1 == 0);
        ser_write("MW:fm cwd0="); ser_write(c0);
        ser_write(" cwd1="); ser_write(c1);
        ser_write(" n0="); ser_dec(n0);
        ser_write(" n1="); ser_dec(n1);
        ser_write(" sel0="); ser_dec(s0);
        ser_write(" sel1="); ser_dec(s1);
        ser_write(" iso="); ser_dec(iso); ser_write("\n");
    } else if (!strcmp_probe(mode, "focus")) {
        gui_term_open();
        gui_term_open();
        gui_term_focus(1);                     /* B 置前 + 标题高亮 */
        ser_write("MW:focus active="); ser_dec(gui_term_active_idx()); ser_write("\n");
    } else if (!strcmp_probe(mode, "cap")) {
        gui_term_open(); gui_term_open(); gui_term_open(); gui_term_open();
        gui_term_open();                       /* 第 5 个：到上限，回提示行不崩 */
        ser_write("MW:cap count="); ser_dec(gui_term_count()); ser_write("\n");
    } else if (!strcmp_probe(mode, "close")) {
        gui_term_open();
        gui_term_open();
        gui_term_close_inst(0);                /* 只关被点实例 A */
        int n = gui_term_count();
        const char *c1 = gui_term_count() > 0 ? gui_term_cwd(1) : "-";
        ser_write("MW:close count="); ser_dec(n);
        ser_write(" cwd1="); ser_write(c1);
        ser_write(" active="); ser_dec(gui_term_active_idx()); ser_write("\n");
    } else if (!strcmp_probe(mode, "focusnp")) {
        gui_term_open();
        gui_term_type(0, "xx");
        const char *tb = gui_term_line_buf(0);
        gui_notepad_open(0);
        block *nb = btable_get(gui_notepad_root());
        if (nb) { gui_inject_press(nb->x + nb->w / 2, nb->y + nb->h / 2); gui_inject_release(); }
        gui_inject_key('a'); gui_inject_key('b'); gui_inject_key('c');
        int nlen = gui_notepad_len();
        const char *ntext = gui_notepad_text();
        block *tb2 = btable_get(gui_term_root(0));
        if (tb2) { gui_inject_press(tb2->x + 12, tb2->y + tb2->h / 2); gui_inject_release(); }
        const char *ta = gui_term_line_buf(0);
        int iso = (nlen == 3 && strcmp_probe(ntext, "abc") == 0 &&
                   strcmp_probe(tb, ta) == 0);
        ser_write("MW:focusnp tb_before="); ser_write(tb);
        ser_write(" np_len="); ser_dec(nlen);
        ser_write(" np_text="); ser_write(ntext);
        ser_write(" tb_after="); ser_write(ta);
        ser_write(" iso="); ser_dec(iso); ser_write("\n");
    } else if (!strcmp_probe(mode, "tb")) {
        /* C1：终端 + 文件管理器各一实例，任务栏各出一个按钮。 */
        gui_term_open();
        gui_filemgr_open("/");
        tb_poll();
        tb_dump();
    } else if (!strcmp_probe(mode, "tbactive")) {
        /* C1：两个终端实例，聚焦第二个，任务栏活动项高亮口径与窗口一致。 */
        gui_term_open();
        gui_term_open();
        gui_term_focus(1);
        tb_poll();
        tb_dump();
    } else if (!strcmp_probe(mode, "tbclose")) {
        /* C1：关掉终端实例后，其任务栏按钮立即移除。 */
        gui_term_open();
        gui_filemgr_open("/");
        gui_term_close_inst(0);
        tb_poll();
        tb_dump();
    }
    layout_run();
    gui_frame();
}

static void gui_probe_r5term(void) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:r5term result=0 reason=mode\n");
        con_set_gui_mode(0);
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();
    gui_term_open();
    layout_run();
    gui_frame();
    ser_write("GUI:r5term opened=1\n");
}

/* A7.1 菜单加速键探针：Alt+字母开菜单并直达；未匹配字母吞键不漂移；菜单打开
 * 时普通字母经钩子吞键、不漏进背后输入框。菜单保留在屏上供截图（下划线可见）。 */
static void gui_probe_menu_accel(void) {
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        ser_write("GUI:menu-accel result=0 reason=mode\n");
        con_set_gui_mode(0);
        return;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W; gui_root_block()->h = GFX24_H;
    gui_demo_build();
    layout_run();
    gui_frame();

    /* Alt+N：菜单未开 -> 打开并直达 Notepad（标签 `&Notepad`，accel 'n'）。 */
    int opened = gui_menu_accel('n');
    layout_run();
    gui_frame();
    int sel_n = menu_sel;
    block *rn = (menu_sel >= 0 && menu_sel < menu_row_n)
                ? btable_get(menu_row_ids[menu_sel]) : 0;
    char an = rn ? (char)txt_accel_char(row_label(rn)) : 0;

    /* 未匹配字母 'z'：吞键，选中不得漂移。 */
    int swallowed_z = gui_menu_accel('z');
    int sel_z = menu_sel;

    /* 菜单打开态下普通字母（无 Alt）经菜单钩子：必须吞键，选中不漂移。 */
    int hook_x = menu_key('x', 0);
    int sel_x = menu_sel;

    ser_write("GUI:menu-accel opened="); ser_dec(opened);
    ser_write(" sel_n="); ser_dec(sel_n);
    ser_write(" accel="); ser_putc(an ? an : '-');
    ser_write(" swallow_z="); ser_dec(swallowed_z);
    ser_write(" sel_z="); ser_dec(sel_z);
    ser_write(" hook_x="); ser_dec(hook_x);
    ser_write(" sel_x="); ser_dec(sel_x);
    ser_write(" result=");
    ser_dec((menu_open && an == 'n' && sel_z == sel_n && sel_x == sel_n) ? 1 : 0);
    ser_write("\n");
}

/* ---- shell 主循环 ------------------------------------------------------- */

/* 生产节拍：PIT 100Hz，每 3 tick 提交一帧（≈33fps）。返回本步是否提交了帧；
 * 同 tick 内的重复提交仍由合成层去重（stat_double_commit）。 */
/* v2.80 hotfix: 上一轮 gui 留下的 chrome 静态句柄/菜单态必须清掉，
 * 否则第二次进桌面会拿着已销毁的 block_id（菜单仍"开着"、时钟块指空）。 */
void gui_chrome_reset(void) {
    menu_root = 0;
    menu_open = 0;
    clock_row = 0;
    clock_text_blk = 0;
    clock_last[0] = 0;
    tb_slot = 0;
    tb_n = 0;
    tb_sig = 0;
    s_idle_frame_tick = 0;
    gui_sak_reset();
}

static int gui_idle_step(void) {
    gui_sak_poll();
    gui_pump_input();
    tb_poll();
    clock_refresh(0);
    if (timer_ticks - s_idle_frame_tick < 3)
        return 0;
    s_idle_frame_tick = timer_ticks;
    gui_frame();
    gui_cvchan_frame_tick();          /* v2.81: 单帧 present 配额按帧重置 */
    extern void decl_frame_tick(void);
    decl_frame_tick();                 /* P3: 单帧提交配额按帧重置 */
    return 1;
}

static void gui_idle_loop(void) {
    s_idle_frame_tick = timer_ticks - 3;   /* 进入桌面后首帧尽快提交 */
    while (!gui_should_exit()) {
        gui_idle_step();
        task_sleep_ticks(1);         /* 100Hz 节拍，帧由 3-tick 门控节流 */
    }
}

void gui_shell_cmd(const char *arg) {
    if (arg && *arg) {
        if (!strcmp_probe(arg, "idem"))  { gui_probe_idempotent(); return; }
        if (!strcmp_probe(arg, "leak")) { gui_probe_leak(); return; }
        if (!strcmp_probe(arg, "geom"))  { gui_probe_geom(); return; }
        if (!strcmp_probe(arg, "bounds")) { gui_probe_bounds(); return; }
        if (!strcmp_probe(arg, "loose"))  { gui_probe_loose(); return; }
        if (!strcmp_probe(arg, "hsel"))   { gui_probe_hsel(); return; }
        if (!strcmp_probe(arg, "dirty")) { gui_probe_dirty(); return; }
        if (!strcmp_probe(arg, "drag"))  { gui_probe_drag(); return; }
        if (!strcmp_probe(arg, "deny"))  { gui_probe_deny(); return; }
        if (!strcmp_probe(arg, "hit"))   { gui_probe_hit(); return; }
        if (!strcmp_probe(arg, "rate"))  { gui_probe_rate(); return; }
        if (!strcmp_probe(arg, "idle"))  { gui_probe_idle(); return; }
        if (!strcmp_probe(arg, "home"))  { gui_probe_home(); return; }
        if (!strcmp_probe(arg, "menu"))  { gui_probe_menu(); return; }
        if (!strcmp_probe(arg, "menuclick")) { gui_probe_menuclick(); return; }
        if (!strcmp_probe(arg, "menu-itest")) { gui_probe_menu_itest(); return; }
        if (!strcmp_probe(arg, "presence")) { gui_probe_presence(); return; }
        if (!strcmp_probe(arg, "diff"))   { gui_probe_diff(); return; }
        if (!strcmp_probe(arg, "theme"))  { gui_probe_theme(); return; }
        if (!strcmp_probe(arg, "reset"))  { gui_probe_reset(); return; }
        if (!strcmp_probe(arg, "gesture")) { gui_probe_gesture(); return; }
        if (!strcmp_probe(arg, "resize"))  { gui_probe_resize(); return; }
        if (!strcmp_probe(arg, "note"))    { gui_probe_note(); return; }
        if (!strcmp_probe(arg, "scroll-term"))  { gui_probe_term_scroll(1); return; }
        if (!strcmp_probe(arg, "scroll-term-bottom")) { gui_probe_term_scroll(0); return; }
        if (!strcmp_probe(arg, "scroll-note"))  { gui_probe_note_scroll(); return; }
        if (!strcmp_probe(arg, "cv"))      { gui_probe_cv(); return; }
        if (!strcmp_probe(arg, "fm"))      { gui_probe_fm(); return; }
        if (!strcmp_probe(arg, "img"))     { gui_probe_img(); return; }
        if (!strcmp_probe(arg, "br"))      { gui_probe_br(); return; }
        {
            char bw[16]; const char *br2 = 0;
            split_word(arg, bw, sizeof(bw), &br2);
            if (!strcmp_probe(bw, "brcase")) { gui_probe_brcase(br2 ? br2 : "edit"); return; }
        }
        if (!strcmp_probe(arg, "cvchan"))  { gui_cvchan_probe(); return; }
        if (!strcmp_probe(arg, "sak"))     { gui_sak_probe(); return; }
        if (!strcmp_probe(arg, "decl"))    { decl_self_test(); return; }
        if (!strcmp_probe(arg, "blkdbg")) {
            for (int i = 0; i < 16; i++) decl_dump_pid(i);
            return;
        }
        if (!strcmp_probe(arg, "free"))    { gui_probe_free(); return; }
        if (!strcmp_probe(arg, "cvdirty")) { gui_probe_cvdirty(); return; }
        if (!strcmp_probe(arg, "interact")){ gui_probe_interact(); return; }
        if (!strcmp_probe(arg, "term"))    { gui_probe_term(); return; }
        {
            char mw[16]; const char *mr = 0;
            split_word(arg, mw, sizeof(mw), &mr);
            if (!strcmp_probe(mw, "mw")) { gui_probe_mw(mr ? mr : "term"); return; }
        }
        if (!strcmp_probe(arg, "r5desk"))  { gui_probe_r5desk(); return; }
        if (!strcmp_probe(arg, "r5dbl"))   { gui_probe_r5dbl(); return; }
        if (!strcmp_probe(arg, "r5term"))  { gui_probe_r5term(); return; }
        if (!strcmp_probe(arg, "menu-accel")) { gui_probe_menu_accel(); return; }
        if (!strcmp_probe(arg, "clickup"))    { gui_probe_clickup(); return; }
        if (!strcmp_probe(arg, "icon-audit")){ gui_probe_icon_audit(); return; }
        if (!strcmp_probe(arg, "scaled"))    { gui_probe_scaled(); return; }
        {
            char aw[16]; const char *ar = 0;
            split_word(arg, aw, sizeof(aw), &ar);
            if (!strcmp_probe(aw, "audit")) { gui_probe_audit(ar ? ar : "none"); return; }
        }
        /* cap2 能力扩展探针族：`gui cap2-<case>`（cap2probe.c 分派三包） */
        {
            int n = 0;
            while (arg[n] && n < 5) n++;
            if (n == 5 && arg[0]=='c' && arg[1]=='a' && arg[2]=='p' &&
                arg[3]=='2' && arg[4]=='-') {
                cap2_probe_cmd(arg + 5);
                return;
            }
        }
        {
            char w2[24]; const char *r2 = 0;
            split_word(arg, w2, sizeof(w2), &r2);
            if (!strcmp_probe(w2, "style")) { gui_probe_style(r2 ? r2 : "app"); return; }            if (!strcmp_probe(w2, "perf"))  { gui_perf_set(r2 ? r2 : ""); return; }
            if (!strcmp_probe(w2, "debug")) {
                char w3[16]; const char *r3 = 0;
                split_word(r2 ? r2 : "", w3, sizeof(w3), &r3);
                if (!strcmp_probe(w3, "dragfull")) {
                    extern void gui_dbg_set_dragfull(int on);
                    gui_dbg_set_dragfull(!strcmp_probe(r3 ? r3 : "", "on"));
                }
                return;
            }
        }
    }

    /* 截图专用状态：shot-menu / shot-light / shot-hc / shot-reset /
     * shot-crop（D7 超框裁剪）/ shot-drag（11b 拖拽残影）；
     * 组件态：shot-notepad / shot-canvas / shot-files / shot-images。 */
    int shot_menu = 0, shot_reset = 0, shot_crop = 0, shot_drag = 0;
    int shot_controls = 0;
    int e1_btn = 0, e1_outline = 0;
    const char *e1_state = 0;
    int e1_residue = 0;
    const char *e1_res_state = 0;
    int e3_input = 0;
    const char *e3_input_state = 0;
    int e2_z = 0;
    const char *e2_z_state = 0;
    int free_dash = 0, free_side = 0, free_float = 0, free_overflow = 0;
    int free_interact = 0;
    int deskev = 0;
    int shot_hov = 0, shot_sel = 0;   /* P1-1：截图态——悬停 / 选中桌面条目 */
    int shot_icons = 0, shot_icons_bad = 0, shot_icons_hover = 0;   /* v3：按钮图片资源截图 */
    int shot_decl_img = 0;   /* v3：声明语法 img=/icon= 按钮截图 */
    int shot_hov_gap = 0;              /* P1-1 小修：悬停条目留白（命中容器本身） */
    int shot_style = -1;   /* -1 保持档位；0 app；1 calm */
    enum gui_token shot_tok = TOK_DARK;
    enum { APP_NONE, APP_NOTEPAD, APP_CANVAS, APP_FILEMGR, APP_IMAGES, APP_BROWSER, APP_SETTINGS };
#define APP_BLKGEN 99   /* 追加：blkgen 内置页；不改动既有枚举成员 */
    int app_open = APP_NONE;
    char app_path[160]; app_path[0] = 0;
    const char *rest = 0;
    char word[24];
    split_word(arg, word, sizeof(word), &rest);
    if (!strcmp_probe(word, "shot-menu"))  { shot_menu = 1; probe_menu_raise = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-menu-geo")) { shot_menu = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-controls")) { shot_controls = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-controls-card")) { shot_controls = 1; probe_no_sample = 1; probe_card_only = 1; }
    else if (!strcmp_probe(word, "shot-e1-btn")) { e1_btn = 1; probe_no_sample = 1; e1_state = rest ? rest : "idle"; }
    else if (!strcmp_probe(word, "shot-e1-outline")) { e1_outline = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-e1-residue")) { e1_residue = 1; probe_no_sample = 1; e1_res_state = rest ? rest : "idle"; }
    else if (!strcmp_probe(word, "shot-e3-input")) { e3_input = 1; probe_no_sample = 1; e3_input_state = rest ? rest : "idle"; }
    else if (!strcmp_probe(word, "shot-e2-z")) { e2_z = 1; probe_no_sample = 1; e2_z_state = rest ? rest : "stack"; }
    else if (!strcmp_probe(word, "shot-flush-inset"))  { probe_flush_inset = 1;  probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-flush-hpair"))  { probe_flush_hpair = 1;  probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-flush-vgroup")) { probe_flush_vgroup = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-flush-bleed")) { probe_flush_bleed = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-empty")) { probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-light")) shot_tok = TOK_LIGHT;
    else if (!strcmp_probe(word, "shot-hc"))    shot_tok = TOK_HIGH_CONTRAST;
    else if (!strcmp_probe(word, "shot-reset")) shot_reset = 1;
    else if (!strcmp_probe(word, "shot-crop"))  shot_crop = 1;
    else if (!strcmp_probe(word, "shot-drag"))  shot_drag = 1;
    else if (!strcmp_probe(word, "shot-free-dash"))  free_dash = 1;
    else if (!strcmp_probe(word, "shot-free-sidebar")) free_side = 1;
    else if (!strcmp_probe(word, "shot-free-float")) free_float = 1;
    else if (!strcmp_probe(word, "shot-free-overflow")) free_overflow = 1;
    else if (!strcmp_probe(word, "shot-free-interact")) free_interact = 1;
    else if (!strcmp_probe(word, "deskev")) deskev = 1;
    else if (!strcmp_probe(word, "shot-hov"))  shot_hov = 1;
    else if (!strcmp_probe(word, "shot-hov-gap")) shot_hov_gap = 1;
    else if (!strcmp_probe(word, "shot-sel"))  shot_sel = 1;
    else if (!strcmp_probe(word, "shot-icons"))      { shot_icons = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-icons-bad")) { shot_icons_bad = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-icons-hover")) { shot_icons_hover = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-decl-img"))  { shot_decl_img = 1; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-style-calm")) shot_style = 1;
    else if (!strcmp_probe(word, "shot-style-app"))  shot_style = 0;
    else if (!strcmp_probe(word, "shot-notepad")) { app_open = APP_NOTEPAD; app_path[0] = 0; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-canvas"))  { app_open = APP_CANVAS; }
    else if (!strcmp_probe(word, "shot-files"))   { app_open = APP_FILEMGR; app_path[0] = 0; }
    else if (!strcmp_probe(word, "shot-images"))  { app_open = APP_IMAGES; app_path[0] = 0; }
    else if (!strcmp_probe(word, "shot-browser")) { app_open = APP_BROWSER; app_path[0] = 0; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "shot-settings")) { app_open = APP_SETTINGS; probe_no_sample = 1; }
    else if (!strcmp_probe(word, "notepad")) { app_open = APP_NOTEPAD; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }
    else if (!strcmp_probe(word, "canvas"))   { app_open = APP_CANVAS; }
    else if (!strcmp_probe(word, "files"))    { app_open = APP_FILEMGR; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }
    else if (!strcmp_probe(word, "images"))    { app_open = APP_IMAGES; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }
    else if (!strcmp_probe(word, "browser"))   { app_open = APP_BROWSER; if (rest) xcopy_local(app_path, sizeof(app_path), rest); }
    else if (!strcmp_probe(word, "shot-blkgen")) { app_open = APP_BLKGEN; }

    /* `gui shot-style-app blkgen` / `gui shot-style-calm blkgen`: 第二词
     * 选中 blkgen 内置页（全保真路径）。 */
    if (rest && !strcmp_probe(rest, "blkgen")) app_open = APP_BLKGEN;

    /* 第二参数档/主题：`gui shot-notepad calm dark` 这类后置词。 */
    if (rest) {
        if (style_contains(rest, "calm")) shot_style = 1;
        else if (style_contains(rest, "app")) shot_style = 0;
        if (style_contains(rest, "light")) shot_tok = TOK_LIGHT;
        else if (style_contains(rest, "hc")) shot_tok = TOK_HIGH_CONTRAST;
        if (style_contains(rest, "high")) probe_tb_high = 1;
        if (style_contains(rest, "tall")) probe_menu_tall = 1;
    }

    /* v2.80 hotfix: 进桌面先清空控制台文本并冻结控制台侧绘制/选中，
     * 否则底下的 shell 内容会在鼠标/时钟等路径上被重绘进帧缓冲。 */
    con_clear();
    con_set_gui_mode(1);
    int r = con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE);
    if (r != 0) {
        con_set_gui_mode(0);
        con_write("gui: 进入图形块桌面失败\n");
        return;
    }
    int free_sample = free_dash || free_side || free_float || free_overflow ||
                      free_interact || deskev || shot_icons || shot_icons_bad ||
                      shot_icons_hover || shot_decl_img;
    int blkgen_on = (app_open == APP_BLKGEN);
    if (blkgen_on) free_sample = 1;   /* blkgen 页不挂 demo 样本（纯追加，不改既有条件） */
    int flush_probe = probe_flush_inset || probe_flush_hpair ||
                      probe_flush_vgroup || probe_flush_bleed;
    gui_begin();
    if (!free_sample && !flush_probe) gui_demo_build();
    if (shot_reset) { gui_reset(); if (!free_sample && !flush_probe) gui_demo_build(); }
    if (shot_tok != TOK_DARK) gui_theme_set(shot_tok);
    if (shot_style >= 0) gui_style_set(shot_style);
    if (shot_menu) {
        gui_menu_open();
        if (probe_menu_tall) gui_frame();
        menu_report_rects();
    }
    if (shot_controls) { shot_controls_build(); gui_frame(); }
    if (e1_btn)       { shot_e1_btn_build(e1_state); gui_frame(); }
    if (e1_outline)   { shot_e1_outline_build(); gui_frame(); }
    if (e1_residue)   { shot_e1_residue_build(e1_res_state); gui_frame(); }
    if (e3_input)     { shot_e3_input_build(e3_input_state); gui_frame(); }
    if (e2_z)         { shot_e2_z_build(e2_z_state); gui_frame(); }
    if (probe_flush_inset)  { shot_flush_inset_build();  gui_frame(); }
    if (probe_flush_hpair)  { shot_flush_hpair_build();  gui_frame(); }
    if (probe_flush_vgroup) { shot_flush_vgroup_build(); gui_frame(); }
    if (probe_flush_bleed)  { shot_flush_bleed_build();  gui_frame(); }
    if (shot_crop) shot_crop_build();
    if (shot_drag) shot_drag_build();
    if (free_dash)  shot_free_dash_build();
    if (free_side)  shot_free_side_build();
    if (free_float) shot_free_float_build();
    if (free_overflow) shot_free_overflow_build();
    if (free_interact) shot_free_interact_build();
    if (app_open == APP_NOTEPAD) gui_notepad_open(app_path[0] ? app_path : 0);
    if (app_open == APP_CANVAS)  gui_canvas_demo();
    if (app_open == APP_FILEMGR) gui_filemgr_open(app_path[0] ? app_path : 0);
    if (app_open == APP_IMAGES)  gui_images_open(app_path[0] ? app_path : 0);
    if (app_open == APP_BROWSER) {
        if (app_path[0]) gui_browser_open(app_path);
        else gui_browser_probe_sample();   /* 无网回归：内嵌样本页 */
    }
    if (app_open == APP_BLKGEN) blkgen_build();
    if (app_open == APP_SETTINGS) {
        gui_settings_open();
        int rx, ry, rw, rh; st_layout_rects(-1, &rx, &ry, &rw, &rh);
        ser_write("GUI:settings-rows root ");
        ser_dec(rx); ser_write(","); ser_dec(ry); ser_write(",");
        ser_dec(rw); ser_write(","); ser_dec(rh);
        for (int i = 0; i < 2; i++) {
            int ox, oy, ow, oh; st_layout_rects(i, &ox, &oy, &ow, &oh);
            ser_write(" opt ");
            ser_dec(ox); ser_write(","); ser_dec(oy); ser_write(",");
            ser_dec(ow); ser_write(","); ser_dec(oh);
        }
        ser_write("\n");
    }
    if (shot_icons)      shot_icons_build(0, 0);
    if (shot_icons_bad)  shot_icons_build(1, 0);
    if (shot_icons_hover) shot_icons_build(0, 1);
    if (shot_decl_img)   shot_decl_img_build();
    if (!shot_crop && !shot_drag && !free_sample) gui_frame();
    if (shot_hov && desk_n > 0) {
        block *b0 = btable_get(desk_item_blk[0]);
        if (b0) {
            gui_inject_hover(b0->x + b0->w / 2, b0->y + b0->h / 2);
            b0->dirty_paint = 1;
        }
    }
    if (shot_hov_gap && desk_n > 0) {
        /* 条目留白处（左侧中高，避开居中的图标/标签子块）：命中叶子即容器本身，
         * 复现「停在图标与标签之间留白」的 hover 路径。 */
        block *b0 = btable_get(desk_item_blk[0]);
        if (b0) {
            gui_inject_hover(b0->x + 4, b0->y + b0->h / 2);
            b0->dirty_paint = 1;
        }
    }
    if (shot_sel && desk_n > 0) {
        desk_sel = 0;
        block *b0 = btable_get(desk_item_blk[0]);
        if (b0) b0->dirty_paint = 1;
    }
    if (shot_hov || shot_hov_gap || shot_sel) gui_frame();
    if (deskev) { layout_run(); gui_frame(); decl_dump_ce_rects(); }

    gui_idle_loop();

    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
}
