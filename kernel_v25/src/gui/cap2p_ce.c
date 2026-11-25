/*
 * cap2p_ce.c -- CE 通道包探针（rec2 全字段镜像 / kind 扩权 / 事件回传）。
 * 消费 `gui cap2-<case>` 的 case 名（不含 "cap2-" 前缀）；返回 1 = 已消费。
 *
 * 探针在内核态直接调用 decl.c 的探针入口（不经 copy_from_user），断言真实
 * 落地字段与回传事件；每条打点 `GUI:cap2-<case> PASS k=v` / `FAIL detail`。
 */
#include <stdint.h>
#include <stddef.h>
#include "gui/block.h"
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/decl.h"
#include "ce.h"
#include "serial_log.h"
#include "kstring.h"
#include "render.h"
#include "gfx24.h"

extern int memcmp(const void *a, const void *b, unsigned int n);

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ---- decl.c 探针专用入口（内核自检使用） ---------------------------------- */
extern int      decl_ce_probe_commit(int tid, const void *recs, int n,
                                    const char *strings, int slen,
                                    uint32_t rec_size);
extern int      decl_ce_probe_poll2(int tid, ce_blk_event2_t *ev);
extern void     decl_ce_probe_cleanup(int tid);
extern block_id decl_ce_probe_local_to_global(int tid, int lid);
extern int      decl_ce_probe_count(int tid);
extern int      decl_ce_probe_dropped(int tid);

/* ---- 串口小工具 ---------------------------------------------------------- */

static void p_dec(int v) {
    char tmp[12]; int n = 0; unsigned int u;
    if (v < 0) { ser_putc('-'); u = (unsigned int)(-(int)v); }
    else u = (unsigned int)v;
    if (u == 0) { ser_putc('0'); return; }
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) ser_putc(tmp[--n]);
}

static void p_hex(uint32_t v) {
    ser_write("0x");
    char dig[] = "0123456789abcdef";
    for (int s = 28; s >= 0; s -= 4) ser_putc(dig[(v >> s) & 0xF]);
}

static void verdict(const char *c, int ok, const char *detail) {
    ser_write("GUI:cap2-"); ser_write(c);
    ser_write(ok ? " PASS " : " FAIL ");
    ser_write(detail); ser_putc('\n');
}

static void rec2_init(ce_blk_rec2_t *r, int id, int parent, uint8_t kind,
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

/* ---- ce-caps：能力位探测 -------------------------------------------------- */

static int case_caps(void) {
    uint32_t caps = decl_caps();
    if (caps == DECL_CAP2_ALL) {
        ser_write("GUI:cap2-ce-caps PASS caps="); p_hex(caps); ser_putc('\n');
        return 1;
    }
    ser_write("GUI:cap2-ce-caps FAIL caps="); p_hex(caps);
    ser_write(" want="); p_hex(DECL_CAP2_ALL); ser_putc('\n');
    return 1;
}

/* ---- ce-button：提交 BUTTON → 点击 → 回传 id + local 坐标 ------------------ */

static int case_button(void) {
    int tid = 11;
    decl_ce_probe_cleanup(tid);

    ce_blk_rec2_t recs[2];
    rec2_init(&recs[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&recs[1], 2, 1, BLK_BUTTON, 0);
    const char pool[] = "OK";

    int rc = decl_ce_probe_commit(tid, recs, 2, pool, sizeof(pool),
                                  (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0) {
        ser_write("GUI:cap2-ce-button FAIL commit rc="); p_dec(rc); ser_putc('\n');
        return 1;
    }

    block_id gid = decl_ce_probe_local_to_global(tid, 2);
    block *b = btable_get(gid);
    if (!b) { verdict("ce-button", 0, "button block missing"); return 1; }

    decl_post_click(b, 3, 5);

    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    int got = decl_ce_probe_poll2(tid, &ev);
    if (got != 1) { verdict("ce-button", 0, "no event back"); return 1; }

    if (ev.id != 2 || ev.x != 3 || ev.y != 5 || ev.target_id != gid ||
        ev.ev_kind != 0 || ev.type != DECL_EV_CLICK) {
        ser_write("GUI:cap2-ce-button FAIL id="); p_dec(ev.id);
        ser_write(" x="); p_dec(ev.x); ser_write(" y="); p_dec(ev.y);
        ser_write(" kind="); p_dec(ev.ev_kind); ser_putc('\n');
        return 1;
    }
    ser_write("GUI:cap2-ce-button PASS id=2 x=3 y=5"); ser_putc('\n');
    return 1;
}

/* ---- ce-input：提交 INPUT → 中文文本回传，不切断多字节 -------------------- */

static int utf8_len(const uint8_t *s) { int n = 0; while (s[n]) n++; return n; }

static int case_input(void) {
    int tid = 12;
    decl_ce_probe_cleanup(tid);

    ce_blk_rec2_t recs[2];
    rec2_init(&recs[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&recs[1], 2, 1, BLK_INPUT, -1);

    int rc = decl_ce_probe_commit(tid, recs, 2, "", 0,
                                  (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0) {
        ser_write("GUI:cap2-ce-input FAIL commit rc="); p_dec(rc); ser_putc('\n');
        return 1;
    }

    block_id gid = decl_ce_probe_local_to_global(tid, 2);
    block *b = btable_get(gid);
    if (!b) { verdict("ce-input", 0, "input block missing"); return 1; }

    /* 短文本：字节级一致。 */
    const char *short_txt = "\xe4\xbd\xa0\xe5\xa5\xbd" "CatOS"; /* 你好CatOS */
    decl_post_input(b, short_txt);
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    if (decl_ce_probe_poll2(tid, &ev) != 1) {
        verdict("ce-input", 0, "no short event"); return 1;
    }
    int slen = utf8_len((const uint8_t *)short_txt);
    if (ev.ev_kind != 3 || ev.target_id != gid ||
        memcmp(ev.utext, short_txt, (unsigned int)slen + 1) != 0) {
        verdict("ce-input", 0, "short text mismatch"); return 1;
    }

    /* 长文本：30B（10 个汉字）→ 截断到 21B（7 个整汉字），不切断末字符。 */
    const char *long_txt =
        "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94"
        "\xe5\x85\xad\xe4\xb8\x83\xe5\x85\xab\xe4\xb9\x9d\xe5\x8d\x81";
    decl_post_input(b, long_txt);
    memset(&ev, 0, sizeof(ev));
    if (decl_ce_probe_poll2(tid, &ev) != 1) {
        verdict("ce-input", 0, "no long event"); return 1;
    }
    /* 21B = 7 个整汉字；第 22 字节必须为 NUL，首字节不得是续字节。 */
    if (ev.utext[21] != 0 || (ev.utext[0] & 0xC0) == 0x80) {
        verdict("ce-input", 0, "multibyte split"); return 1;
    }
    if (memcmp(ev.utext, long_txt, 21) != 0) {
        verdict("ce-input", 0, "long prefix mismatch"); return 1;
    }

    ser_write("GUI:cap2-ce-input PASS bytes="); p_dec(slen);
    ser_write(" clip=21"); ser_putc('\n');
    return 1;
}

/* ---- ce-popover-anchor：跨 pid 锚定拒绝 / 同批锚定通过 ------------------- */

static int case_popover(void) {
    int tid = 13, tid2 = 14;
    decl_ce_probe_cleanup(tid);
    decl_ce_probe_cleanup(tid2);

    /* 合法：锚到同批已提交的 BUTTON（local id 2）。 */
    ce_blk_rec2_t ok[3];
    rec2_init(&ok[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&ok[1], 2, 1, BLK_BUTTON, -1);
    rec2_init(&ok[2], 3, 1, BLK_POPOVER, -1);
    ok[2].anchor_id = 2;
    int rc = decl_ce_probe_commit(tid, ok, 3, "", 0,
                                  (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0) {
        ser_write("GUI:cap2-ce-popover-anchor FAIL legal rc=");
        p_dec(rc); ser_putc('\n'); return 1;
    }

    /* tid2 一棵独立子树，取其全局块 id 做跨 pid 锚定。 */
    ce_blk_rec_t r2;
    memset(&r2, 0, sizeof(r2));
    r2.id = 1; r2.parent = 0; r2.kind = BLK_RECT; r2.layer = LAYER_WORK;
    r2.text_off = -1; r2.pix_off = -1; r2.res = -1;
    if (decl_ce_probe_commit(tid2, &r2, 1, "", 0, 40) != 0) {
        verdict("ce-popover-anchor", 0, "setup pid2 failed"); return 1;
    }
    block_id foreign = decl_ce_probe_local_to_global(tid2, 1);

    /* 非法①：跨 pid 锚定。 */
    ce_blk_rec2_t bad[2];
    rec2_init(&bad[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&bad[1], 2, 1, BLK_POPOVER, -1);
    bad[1].anchor_id = foreign;
    rc = decl_ce_probe_commit(tid, bad, 2, "", 0,
                              (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != E_DECL_BADID) {
        ser_write("GUI:cap2-ce-popover-anchor FAIL cross-pid rc=");
        p_dec(rc); ser_putc('\n'); return 1;
    }

    /* 非法②：悬空锚点 id。 */
    bad[1].anchor_id = 9999;
    rc = decl_ce_probe_commit(tid, bad, 2, "", 0,
                              (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != E_DECL_BADID) {
        ser_write("GUI:cap2-ce-popover-anchor FAIL dangling rc=");
        p_dec(rc); ser_putc('\n'); return 1;
    }

    decl_ce_probe_cleanup(tid2);
    ser_write("GUI:cap2-ce-popover-anchor PASS legal=1 badid=2"); ser_putc('\n');
    return 1;
}

/* ---- ce-rec1-compat：40B 旧记录与 rec2 混合提交互不影响 ------------------ */

static int case_rec1_compat(void) {
    int tid = 15;
    decl_ce_probe_cleanup(tid);

    /* 第一批：旧 40B 记录。 */
    ce_blk_rec_t old[2];
    memset(old, 0, sizeof(old));
    old[0].id = 1; old[0].parent = 0; old[0].kind = BLK_COLUMN;
    old[0].layer = LAYER_WORK; old[0].text_off = -1;
    old[0].pix_off = -1; old[0].res = -1;
    old[1].id = 2; old[1].parent = 1; old[1].kind = BLK_TEXT;
    old[1].layer = LAYER_WORK; old[1].text_off = 0;
    old[1].pix_off = -1; old[1].res = -1;
    const char pool[] = "Hi";
    int rc = decl_ce_probe_commit(tid, old, 2, pool, sizeof(pool), 40);
    if (rc != 0 || decl_ce_probe_count(tid) != 2) {
        ser_write("GUI:cap2-ce-rec1-compat FAIL rec1 rc="); p_dec(rc);
        ser_write(" n="); p_dec(decl_ce_probe_count(tid)); ser_putc('\n');
        return 1;
    }

    /* 第二批：rec2 记录，扩展段落地。 */
    ce_blk_rec2_t r2[3];
    rec2_init(&r2[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&r2[1], 2, 1, BLK_BUTTON, -1);
    rec2_init(&r2[2], 3, 1, BLK_ICON, -1);
    r2[2].alpha = 200; r2[2].corner_tl = 10; r2[2].icon = 3;
    rc = decl_ce_probe_commit(tid, r2, 3, "", 0,
                              (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0 || decl_ce_probe_count(tid) != 3) {
        ser_write("GUI:cap2-ce-rec1-compat FAIL rec2 rc="); p_dec(rc);
        ser_write(" n="); p_dec(decl_ce_probe_count(tid)); ser_putc('\n');
        return 1;
    }
    block_id i3 = decl_ce_probe_local_to_global(tid, 3);
    block *bi = btable_get(i3);
    if (!bi || bi->alpha != 200 || bi->corner_tl != 10 || bi->icon != 3) {
        verdict("ce-rec1-compat", 0, "rec2 fields not landed"); return 1;
    }

    /* 第三批：再回 40B 旧记录；重建块必须不带 rec2 扩展段残留。 */
    rc = decl_ce_probe_commit(tid, old, 2, pool, sizeof(pool), 40);
    if (rc != 0 || decl_ce_probe_count(tid) != 2) {
        ser_write("GUI:cap2-ce-rec1-compat FAIL rec1b rc="); p_dec(rc);
        ser_write(" n="); p_dec(decl_ce_probe_count(tid)); ser_putc('\n');
        return 1;
    }
    block_id t2 = decl_ce_probe_local_to_global(tid, 2);
    block *bt = btable_get(t2);
    if (!bt || bt->alpha != 0 || bt->corner_tl != 0 || bt->icon != 0) {
        verdict("ce-rec1-compat", 0, "rec2 residue after rec1"); return 1;
    }

    ser_write("GUI:cap2-ce-rec1-compat PASS rec1=2 rec2=3 rec1b=2");
    ser_putc('\n');
    return 1;
}

/* ---- elev-reject：elevation 6 / 255 越界整批拒绝，旧子树不受影响 ---------- */

static int case_elev_reject(void) {
    int tid = 6;
    decl_ce_probe_cleanup(tid);

    /* 基线：一棵合法子树（2 块，elevation=3）；后续拒绝不得触碰它。 */
    ce_blk_rec2_t base[2];
    rec2_init(&base[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&base[1], 2, 1, BLK_RECT, -1);
    base[1].elevation = 3;
    int rc = decl_ce_probe_commit(tid, base, 2, "", 0,
                                  (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0) {
        ser_write("GUI:cap2-elev-reject FAIL baseline rc="); p_dec(rc);
        ser_putc('\n');
        return 1;
    }
    int base_count = decl_ce_probe_count(tid);

    /* 非法①：elevation=6（越出 0..5 值域）。 */
    ce_blk_rec2_t bad[2];
    rec2_init(&bad[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&bad[1], 2, 1, BLK_RECT, -1);
    bad[1].elevation = 6;
    rc = decl_ce_probe_commit(tid, bad, 2, "", 0,
                              (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != E_DECL_BADREC) {
        ser_write("GUI:cap2-elev-reject FAIL el=6 rc="); p_dec(rc); ser_putc('\n');
        return 1;
    }

    /* 非法②：elevation=255。 */
    bad[1].elevation = 255;
    rc = decl_ce_probe_commit(tid, bad, 2, "", 0,
                              (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != E_DECL_BADREC) {
        ser_write("GUI:cap2-elev-reject FAIL el=255 rc="); p_dec(rc); ser_putc('\n');
        return 1;
    }

    /* 拒绝后：块数不变、旧块仍在且档位未被改写；未落地即不渲染新块。 */
    int after = decl_ce_probe_count(tid);
    block *old = btable_get(decl_ce_probe_local_to_global(tid, 2));
    int ok = (after == base_count) && old && old->elevation == 3;
    ser_write("GUI:cap2-elev-reject ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" base="); p_dec(base_count);
    ser_write(" after="); p_dec(after);
    ser_write(" old_el="); p_dec(old ? old->elevation : -1);
    ser_putc('\n');
    return 1;
}

/* ---- evq-overflow：满队丢最旧，dropped 计数可读，第 9 条去向可观测 ------ */

static int case_evq_overflow(void) {
    int tid = 7;
    decl_ce_probe_cleanup(tid);

    ce_blk_rec2_t recs[2];
    rec2_init(&recs[0], 1, 0, BLK_COLUMN, -1);
    rec2_init(&recs[1], 2, 1, BLK_BUTTON, -1);
    int rc = decl_ce_probe_commit(tid, recs, 2, "", 0,
                                  (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0) {
        verdict("evq-overflow", 0, "commit failed");
        return 1;
    }
    block *b = btable_get(decl_ce_probe_local_to_global(tid, 2));
    if (!b) {
        verdict("evq-overflow", 0, "block missing");
        return 1;
    }

    /* 同一 pid 连造 9 条事件、全程不 poll：队列深 8，第 9 条必须触发溢出。 */
    for (int i = 0; i < 9; i++) decl_post_click(b, i, 0);

    int dropped = decl_ce_probe_dropped(tid);

    /* 回读剩余事件：收到条数 + 丢弃条数必须等于 9，第 9 条去向可观测。 */
    int received = 0;
    ce_blk_event2_t ev;
    while (decl_ce_probe_poll2(tid, &ev) == 1) received++;

    int ok = dropped >= 1 && received + dropped == 9;
    ser_write("GUI:cap2-evq-overflow ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" dropped="); p_dec(dropped);
    ser_write(" received="); p_dec(received);
    ser_write(" total=9");
    ser_putc('\n');
    return 1;
}

/* ---- anim-prod：走 CE rec2 提交建档，生产路径 kick，x 真的在动 ---------- */

static int case_anim_prod(void) {
    /* 进 GUI 模式：需要真实布局把目标几何算出来（建档 kick 在 apply2 内）。 */
    con_set_gui_mode(1);
    if (con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE) != 0) {
        con_set_gui_mode(0);
        verdict("anim-prod", 0, "mode enter failed");
        return 1;
    }
    gui_begin();
    gui_root_block()->w = GFX24_W;
    gui_root_block()->h = GFX24_H;

    int tid = 8;
    decl_ce_probe_cleanup(tid);

    /* ROW：60px 静态 spacer + 40px 动画块，动画块目标 x=60。动画声明完全
     * 走 CE rec2 提交，探针不直接调 anim_kick。 */
    ce_blk_rec2_t recs[3];
    rec2_init(&recs[0], 1, 0, BLK_ROW, -1);
    rec2_init(&recs[1], 2, 1, BLK_RECT, -1);
    recs[1].base.req_w = 60; recs[1].base.req_h = 40;
    rec2_init(&recs[2], 3, 1, BLK_RECT, -1);
    recs[2].base.req_w = 40; recs[2].base.req_h = 40;
    recs[2].anim_ms = 192; recs[2].anim_ease = 1; recs[2].anim_mask = 1;

    int rc = decl_ce_probe_commit(tid, recs, 3, "", 0,
                                  (uint32_t)sizeof(ce_blk_rec2_t));
    if (rc != 0) {
        ser_write("GUI:cap2-anim-prod FAIL commit rc="); p_dec(rc); ser_putc('\n');
        decl_ce_probe_cleanup(tid);
        gui_end(); con_set_gui_mode(0); con_force_text(); con_clear();
        return 1;
    }
    block *b = btable_get(decl_ce_probe_local_to_global(tid, 3));
    if (!b) {
        verdict("anim-prod", 0, "anim block missing");
        decl_ce_probe_cleanup(tid);
        gui_end(); con_set_gui_mode(0); con_force_text(); con_clear();
        return 1;
    }

    layout_run();
    int target = b->x;

    int frames[12];
    int distinct = 1;
    for (int f = 0; f < 12; f++) {
        anim_tick(16);
        anim_apply(b);
        frames[f] = b->x;
        int seen = 0;
        for (int k = 0; k < f; k++) if (frames[k] == b->x) seen = 1;
        if (!seen) distinct++;
    }
    int ok = distinct >= 3 && b->x == target;
    ser_write("GUI:cap2-anim-prod ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" target="); p_dec(target);
    ser_write(" distinct="); p_dec(distinct);
    ser_write(" last="); p_dec(b->x);
    ser_write(" frames=");
    for (int f = 0; f < 12; f++) { if (f) ser_putc(','); p_dec(frames[f]); }
    ser_putc('\n');

    decl_ce_probe_cleanup(tid);
    gui_end();
    con_set_gui_mode(0);
    con_force_text();
    con_clear();
    return 1;
}

int cap2_probe_ce(const char *arg) {
    if (!arg) return 0;
    if (streq(arg, "ce-caps"))          return case_caps();
    if (streq(arg, "ce-button"))        return case_button();
    if (streq(arg, "ce-input"))         return case_input();
    if (streq(arg, "ce-popover-anchor")) return case_popover();
    if (streq(arg, "ce-rec1-compat"))   return case_rec1_compat();
    if (streq(arg, "elev-reject"))       return case_elev_reject();
    if (streq(arg, "evq-overflow"))     return case_evq_overflow();
    if (streq(arg, "anim-prod"))        return case_anim_prod();
    return 0;
}
