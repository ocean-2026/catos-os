/*
 * decl.c -- CE 声明式 ABI（P3，syscall 槽 72..76，append-only）。
 *
 * 口径：
 *   - 应用整批提交块描述；内核 copy_from_user 拷入后先整批校验，任一
 *     非法即整批拒绝、不触碰旧子树；校验通过才按 (pid, local-id) diff 落地。
 *   - 记录里绝不允许内联像素指针 / pix_w / pix_h；像素只能走资源句柄
 *     （槽 69 画布或槽 74 图片）。内核绝不保存用户态指针。
 *   - 应用只能挂到自己的 WORK 子树；CHROME / OVERLAY / TOAST 一律拒绝。
 *   - 配额：单应用 ≤256 块、深度 ≤16、单块文本 ≤4 KiB、字符串池 ≤32 KiB。
 *   - 应用退出 / 被杀：task_cleanup 强制回收整棵子树，不留孤儿。
 *
 * 大缓冲（每 pid 映射表/文本区、图片像素、提交 scratch）全部走帧分配，
 * 不进内核静态 .bss，避免顶过 CE_LOAD_BASE 冲掉载入的应用镜像。
 *
 * 串口打点：
 *   GUI:decl commit n=<n> ok=1 pid=<tid>
 *   GUI:decl reject code=<n> pid=<tid>
 *   GUI:ev click id=<lid> x=<lx> y=<ly>
 *   GUI:decl leaked=0
 */
#include <stdint.h>
#include <stddef.h>
#include "task.h"
#include "paging.h"
#include "fs.h"
#include "kstring.h"
#include "serial_log.h"
#include "ce.h"
#include "gui/gui.h"
#include "gui_int.h"
#include "gui/decl.h"

#define DECL_EVQ        8
#define DECL_ARENA      2048
#define IMG_MAX         4
#define IMG_W           64
#define IMG_H           64
/* 记录区 5 页（20 KiB，容纳 256 条 76B rec2）+ 字符串池 8 页（32 KiB）。 */
#define SCRATCH_PAGES   13
#define SCRATCH_POOL_OFF 5

struct pid_meta {
    uint8_t  used;
    int      tid;
    block_id root;
    int      count;
    uint32_t hash;
    int      last_reject;
    int      evh, evt;
    ce_blk_event2_t evq[DECL_EVQ];  /* 同一队列：旧 poll 只取前 sizeof(ev1) 字节 */
    uint32_t seq;                   /* cap2：事件序号（每 pid 单调递增） */
    uint8_t *mem;                   /* 帧分配：gmap/lmap/arena */
    /* per-pid 统计（#33）。 */
    int      stat_events;           /* 已入队事件数 */
    int      stat_damage;           /* 落地置脏布局标记数 */
    int      stat_frames;            /* 成功落地提交批次数 */
    int      toast_hints;           /* 系统侧非法声明提示次数 */
    uint32_t dropped;               /* 事件队列满队累计丢弃条数（#B 可观测） */
};

/* 当前焦点所属声明式应用；-1 = 焦点归还内核。跨应用焦点链 #33。 */
static int focus_tid = -1;

struct img_meta {
    uint8_t  used;
    int      tid;
    uint16_t w, h;
    uint8_t *pix;
    uint32_t phys[4];
    int      pages;
};

static struct pid_meta ps[MAX_TASKS];
static struct img_meta imgs[IMG_MAX];

/* 提交 scratch（帧分配，惰性首用）。 */
static ce_blk_rec_t *rec_scratch;
static char         *pool_scratch;
static uint32_t      scratch_phys[SCRATCH_PAGES];
static int           scratch_ready;

extern int cvchan_owner(int res, int tid);

static struct pid_meta *ps_get(int tid);

#define DECL_COMMIT_FRAME_MAX 4     /* 单帧提交上限，超出节流 */
static uint8_t commit_frame_cnt;

void decl_frame_tick(void) { commit_frame_cnt = 0; }

/* ---- 串口小工具 ---------------------------------------------------------- */

static void decl_dec(int v) {
    char tmp[12]; int n = 0; unsigned int u;
    if (v < 0) { ser_putc('-'); u = (unsigned int)(-(int)v); }
    else u = (unsigned int)v;
    if (u == 0) { ser_putc('0'); return; }
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) ser_putc(tmp[--n]);
}

static void decl_reject(int code, int tid) {
    ser_write("GUI:decl reject code="); decl_dec(code);
    ser_write(" pid="); decl_dec(tid); ser_putc('\n');
    /* 系统侧非法声明提示（不建应用层 TOAST 块，仅打点 + 计数）。 */
    struct pid_meta *p = ps_get(tid);
    if (p) {
        p->toast_hints++;
        ser_write("GUI:decl toast code="); decl_dec(code);
        ser_write(" pid="); decl_dec(tid); ser_putc('\n');
    }
}

static uint32_t fnv(uint32_t h, int32_t v) {
    const uint8_t *p = (const uint8_t *)&v;
    for (int i = 0; i < 4; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static struct pid_meta *ps_get(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return &ps[tid];
}

/* ---- 帧分配 scratch ------------------------------------------------------ */

static int scratch_alloc(void) {
    if (scratch_ready) return 1;
    for (int p = 0; p < SCRATCH_PAGES; p++) {
        uint32_t pa = frame_alloc();
        if (!pa) return 0;
        scratch_phys[p] = pa;
    }
    rec_scratch = (ce_blk_rec_t *)scratch_phys[0];
    pool_scratch = (char *)scratch_phys[SCRATCH_POOL_OFF];
    scratch_ready = 1;
    return 1;
}

/* ---- 每 pid 帧布局 ------------------------------------------------------- */

static uint16_t *pgmap(struct pid_meta *p) { return (uint16_t *)p->mem; }
static int16_t  *plmap(struct pid_meta *p) { return (int16_t *)(p->mem + 512); }
static char     *parena(struct pid_meta *p) { return (char *)(p->mem + 1024); }

static int pid_mem_alloc(struct pid_meta *p) {
    if (p->mem) return 1;
    uint32_t pa = frame_alloc();
    if (!pa) return 0;
    p->mem = (uint8_t *)pa;
    memset(p->mem, 0, 4096);
    return 1;
}

/* ---- 资源句柄 ------------------------------------------------------------ */

static struct img_meta *img_find(int res, int tid) {
    if (res < 0 || res >= IMG_MAX) return 0;
    struct img_meta *r = &imgs[res];
    if (!r->used || r->tid != tid) return 0;
    return r;
}

/* ---- 提交：校验阶段（不改动块树） ---------------------------------------- */

static int validate(const ce_blk_rec_t *r, int n, const char *pool, int slen,
                    int tid) {
    int pos[DECL_MAX_BLOCKS];
    int lid[DECL_MAX_BLOCKS];
    for (int i = 0; i < n; i++) pos[i] = -1;

    for (int i = 0; i < n; i++) {
        const ce_blk_rec_t *d = &r[i];

        if (d->id < 1) return E_DECL_BADID;
        for (int k = 0; k < i; k++) if (lid[k] == d->id) return E_DECL_BADID;
        lid[i] = d->id;

        if (d->kind < BLK_COLUMN || d->kind > BLK_CANVAS) return E_DECL_BADREC;

        /* 越层：只允许 WORK。 */
        if (d->layer != LAYER_WORK) return E_DECL_LAYER;

        /* 内联像素：指针 / 宽 / 高一律拒绝，且不 deref。 */
        if (d->pix_off != -1 || d->pix_w != 0 || d->pix_h != 0)
            return E_PIX_FIELD;

        /* 文本偏移与长度。 */
        int tlen = 0;
        if (d->text_off != -1) {
            if (d->text_off < 0 || d->text_off >= slen) return E_DECL_BADREC;
            const char *s = pool + d->text_off;
            const char *e = pool + slen;
            while (s < e && *s) { s++; tlen++; }
            if (tlen > DECL_MAX_TEXT) return E_DECL_QUOTA;
        }

        /* 父链：0 = 本应用根；否则必须是本批已出现的 id。 */
        int pidx = -1;
        if (d->parent != 0) {
            for (int k = 0; k < i; k++) if (lid[k] == d->parent) { pidx = k; break; }
            if (pidx < 0) return E_DECL_BADID;
        }

        /* 深度：沿父链累加。 */
        int depth = 1;
        int cur = pidx;
        while (cur >= 0) {
            depth++;
            if (depth > DECL_MAX_DEPTH) return E_DECL_QUOTA;
            cur = pos[cur];
        }
        pos[i] = pidx;

        /* 资源句柄归属校验。 */
        if (d->kind == BLK_CANVAS) {
            if (d->res < 0 || !cvchan_owner(d->res, tid)) return E_DECL_RES;
        } else if (d->kind == BLK_IMAGE) {
            if (d->res < 0 || !img_find(d->res, tid)) return E_DECL_RES;
        } else {
            if (d->res != -1) return E_DECL_RES;
        }
    }
    return 0;
}

/* rec2 允许的 kind：旧白名单（COLUMN..CANVAS）+ cap2 扩权五类。
 * FREE/SURFACE 等维持旧口径，CE 仍不可写像素。 */
static int kind_allowed_rec2(uint8_t k) {
    if (k >= BLK_COLUMN && k <= BLK_CANVAS) return 1;
    if (k == BLK_BUTTON || k == BLK_INPUT || k == BLK_TERM ||
        k == BLK_POPOVER || k == BLK_ICON) return 1;
    return 0;
}

/* cap2 rec2 整批校验（不触碰块树）。与旧口径一致：任一非法整批拒绝。 */
static int validate2(const ce_blk_rec2_t *r, int n, const char *pool, int slen,
                     int tid) {
    int pos[DECL_MAX_BLOCKS];
    int lid[DECL_MAX_BLOCKS];
    for (int i = 0; i < n; i++) pos[i] = -1;

    for (int i = 0; i < n; i++) {
        const ce_blk_rec2_t *d = &r[i];
        const ce_blk_rec_t *b = &d->base;

        if (b->id < 1) return E_DECL_BADID;
        for (int k = 0; k < i; k++) if (lid[k] == b->id) return E_DECL_BADID;
        lid[i] = b->id;

        if (d->cap_ver != CE_BLK_REC2_VER) return E_DECL_BADREC;
        if (!kind_allowed_rec2(b->kind)) return E_DECL_BADREC;

        /* elevation 值域 0..5（阴影/SH_PAD/SH_OX 均为 6 元表）：越界明确
         * 拒绝，不做静默钳制；整批不落树，旧子树不受影响。 */
        if (d->elevation > 5) {
            ser_write("GUI:decl bad elevation el="); decl_dec(d->elevation);
            ser_write(" pid="); decl_dec(tid); ser_putc('\n');
            return E_DECL_BADREC;
        }

        /* 越层：只允许 WORK。 */
        if (b->layer != LAYER_WORK) return E_DECL_LAYER;

        /* 内联像素：指针 / 宽 / 高一律拒绝，且不 deref。 */
        if (b->pix_off != -1 || b->pix_w != 0 || b->pix_h != 0)
            return E_PIX_FIELD;

        /* 文本偏移与长度。 */
        int tlen = 0;
        if (b->text_off != -1) {
            if (b->text_off < 0 || b->text_off >= slen) return E_DECL_BADREC;
            const char *s = pool + b->text_off;
            const char *e = pool + slen;
            while (s < e && *s) { s++; tlen++; }
            if (tlen > DECL_MAX_TEXT) return E_DECL_QUOTA;
        }

        /* 父链：0 = 本应用根；否则必须是本批已出现的 id。 */
        int pidx = -1;
        if (b->parent != 0) {
            for (int k = 0; k < i; k++) if (lid[k] == b->parent) { pidx = k; break; }
            if (pidx < 0) return E_DECL_BADID;
        }

        /* 深度：沿父链累加。 */
        int depth = 1;
        int cur = pidx;
        while (cur >= 0) {
            depth++;
            if (depth > DECL_MAX_DEPTH) return E_DECL_QUOTA;
            cur = pos[cur];
        }
        pos[i] = pidx;

        /* 资源句柄归属校验（新五类一律不挂资源句柄）。 */
        if (b->kind == BLK_CANVAS) {
            if (b->res < 0 || !cvchan_owner(b->res, tid)) return E_DECL_RES;
        } else if (b->kind == BLK_IMAGE) {
            if (b->res < 0 || !img_find(b->res, tid)) return E_DECL_RES;
        } else {
            if (b->res != -1) return E_DECL_RES;
        }

        /* POPOVER 锚点：必须解析到同批已提交块，或本 pid 既有子树内的块；
         * 跨 pid 锚定 / 悬空 id 一律 E_DECL_BADID。 */
        if (b->kind == BLK_POPOVER && d->anchor_id != 0) {
            int found = 0;
            for (int k = 0; k < i; k++)
                if (lid[k] == (int)d->anchor_id) { found = 1; break; }
            if (!found) {
                block *ab = btable_get((block_id)d->anchor_id);
                if (ab && ab->owner == (uint8_t)tid) found = 1;
            }
            if (!found) return E_DECL_BADID;
        }
    }
    return 0;
}

/* ---- 提交：落地阶段 ------------------------------------------------------ */

static void decl_ensure_root(struct pid_meta *p) {
    if (p->root) return;
    blk_spec s;
    memset(&s, 0, sizeof(s));
    s.layer = LAYER_WORK;
    s.visible = 1;
    s.focusable = 0;
    block *b = btable_alloc(BLK_COLUMN);
    if (!b) return;
    b->layer = LAYER_WORK;
    b->visible = 1;
    b->owner = (uint8_t)p->tid;
    tree_attach(gui_root_block(), b);
    p->root = b->id;
}

static const char *decl_copy_text(struct pid_meta *p, const char *src, int len) {
    char *arena = parena(p);
    int used = 0;
    for (int i = 0; i < DECL_ARENA; i++) if (arena[i]) used = i + 1;
    if (used + len + 1 > DECL_ARENA) return 0;
    char *dst = arena + used;
    for (int i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = 0;
    return dst;
}

static int decl_apply(struct pid_meta *p, const ce_blk_rec_t *r, int n,
                      const char *pool, int slen) {
    (void)slen;
    /* 回收旧子树（保留根块本身）。 */
    if (p->root) tree_free_subtree(btable_get(p->root));
    p->count = 0;
    p->hash = 2166136261u;
    decl_ensure_root(p);
    if (!p->root) return E_DECL_QUOTA;

    uint16_t *gm = pgmap(p);
    int16_t  *lm = plmap(p);

    for (int i = 0; i < n; i++) {
        const ce_blk_rec_t *d = &r[i];
        block *par = btable_get(p->root);
        for (int k = 0; k < i; k++) {
            if (lm[k] == d->parent) { par = btable_get(gm[k]); break; }
        }
        block *b = btable_alloc(d->kind);
        if (!b) return E_DECL_QUOTA;

        b->req_w = d->req_w;
        b->req_h = d->req_h;
        b->layer = LAYER_WORK;
        b->visible = 1;
        b->focusable = 0;
        b->owner = (uint8_t)p->tid;
        b->dirty_layout = 1;

        const char *text = 0;
        if (d->text_off != -1) {
            const char *s = pool + d->text_off;
            int len = 0;
            while (len < DECL_MAX_TEXT && s[len]) len++;
            text = decl_copy_text(p, s, len);
            if (!text) return E_DECL_QUOTA;
        }
        b->text = text;

        if (d->kind == BLK_IMAGE) {
            struct img_meta *ir = img_find(d->res, p->tid);
            if (ir) {
                b->pix = ir->pix;
                b->pix_w = ir->w;
                b->pix_h = ir->h;
            }
        }

        tree_attach(par ? par : gui_root_block(), b);
        gm[p->count] = (uint16_t)b->id;
        lm[p->count] = (int16_t)d->id;
        p->count++;
        p->stat_damage++;

        p->hash = fnv(p->hash, d->id);
        p->hash = fnv(p->hash, d->parent);
        p->hash = fnv(p->hash, d->req_w);
        p->hash = fnv(p->hash, d->req_h);
        p->hash = fnv(p->hash, d->res);
    }
    return 0;
}

/* cap2 rec2 落地：与旧 apply 同构，额外把 rec2 扩展段镜像到块上。
 * corner_n 不下发（CE 恒默认 n=5.0），块字段保持 0。 */
static int decl_apply2(struct pid_meta *p, const ce_blk_rec2_t *r, int n,
                       const char *pool, int slen) {
    (void)slen;
    if (p->root) tree_free_subtree(btable_get(p->root));
    p->count = 0;
    p->hash = 2166136261u;
    decl_ensure_root(p);
    if (!p->root) return E_DECL_QUOTA;

    uint16_t *gm = pgmap(p);
    int16_t  *lm = plmap(p);

    for (int i = 0; i < n; i++) {
        const ce_blk_rec2_t *d = &r[i];
        const ce_blk_rec_t *b = &d->base;
        block *par = btable_get(p->root);
        for (int k = 0; k < i; k++) {
            if (lm[k] == b->parent) { par = btable_get(gm[k]); break; }
        }
        block *blk = btable_alloc(b->kind);
        if (!blk) return E_DECL_QUOTA;

        blk->req_w = b->req_w;
        blk->req_h = b->req_h;
        blk->layer = LAYER_WORK;
        blk->visible = 1;
        blk->focusable = 0;
        blk->owner = (uint8_t)p->tid;
        blk->dirty_layout = 1;

        const char *text = 0;
        if (b->text_off != -1) {
            const char *s = pool + b->text_off;
            int len = 0;
            while (len < DECL_MAX_TEXT && s[len]) len++;
            text = decl_copy_text(p, s, len);
            if (!text) return E_DECL_QUOTA;
        }
        blk->text = text;

        if (b->kind == BLK_IMAGE) {
            struct img_meta *ir = img_find(b->res, p->tid);
            if (ir) {
                blk->pix = ir->pix;
                blk->pix_w = ir->w;
                blk->pix_h = ir->h;
            }
        }

        /* ---- cap2 扩展段镜像（0 = 未声明 ≡ 旧行为） ---- */
        blk->alpha        = d->alpha;
        blk->alpha_mode   = d->alpha_mode;
        blk->elevation    = d->elevation;
        blk->corner_tl    = d->corner_tl;
        blk->corner_tr    = d->corner_tr;
        blk->corner_br    = d->corner_br;
        blk->corner_bl    = d->corner_bl;
        blk->wrap_lines   = d->wrap_lines;
        blk->ellipsis     = d->ellipsis;
        blk->font_size    = d->font_size;
        blk->font_bold    = d->font_bold;
        blk->font_tabular = d->font_tabular;
        blk->line_h       = d->line_h;
        blk->anim_ms      = d->anim_ms;
        blk->anim_ease    = d->anim_ease;
        blk->anim_mask    = d->anim_mask;
        blk->scrollbar    = d->scrollbar;
        blk->scroll_axis  = d->scroll_axis;
        blk->border_sides = d->border_sides;
        blk->icon         = d->icon;
        blk->tint_role    = d->tint_role;
        blk->pop_prefer   = d->pop_prefer;
        blk->pop_max_h    = d->pop_max_h;
        blk->corner_shape = d->corner_shape;

        /* 生产路径建档点：声明了动画（anim_ms>0 且 anim_mask!=0）即按声明值
         * kick。anim_kick 内部对 ease==0 / reduced_motion 自洽空转；已在途的
         * 同 id 槽按可中断接管语义续跑（起点=当前帧值）。这里只在建档/整批
         * 重建时触发一次，帧循环（compose.c）不重复 kick。 */
        if (blk->anim_ms > 0 && blk->anim_mask != 0) anim_kick(blk);

        /* 锚点：同批 local id 解析为全局块 id；既有子树锚点保留全局 id。 */
        uint32_t anchor = d->anchor_id;
        if (anchor != 0) {
            int local = -1;
            for (int k = 0; k < p->count; k++)
                if (lm[k] == (int16_t)anchor) { local = k; break; }
            if (local >= 0) anchor = gm[local];
            else {
                block *ab = btable_get((block_id)anchor);
                if (!ab || ab->owner != (uint8_t)p->tid) anchor = 0;
            }
        }
        blk->anchor_id = anchor;

        tree_attach(par ? par : gui_root_block(), blk);
        gm[p->count] = (uint16_t)blk->id;
        lm[p->count] = (int16_t)b->id;
        p->count++;
        p->stat_damage++;

        p->hash = fnv(p->hash, b->id);
        p->hash = fnv(p->hash, b->parent);
        p->hash = fnv(p->hash, b->req_w);
        p->hash = fnv(p->hash, b->req_h);
        p->hash = fnv(p->hash, b->res);
    }
    return 0;
}

int decl_commit(struct task *t, const void *recs, int n,
                const char *strings, int slen) {
    int tid = t ? t->tid : -1;
    struct pid_meta *p = ps_get(tid);
    if (!p) return E_EPERM;
    p->tid = tid;

    if (n < 0 || n > DECL_MAX_BLOCKS) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }
    if (slen < 0 || slen > DECL_MAX_POOL) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }
    if (!scratch_alloc()) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }

    if (n > 0) {
        if (!recs || copy_from_user(t, rec_scratch, recs,
                                    (uint32_t)n * (uint32_t)sizeof(rec_scratch[0])) != 0) {
            decl_reject(E_DECL_BADREC, tid); return E_DECL_BADREC;
        }
    }
    if (slen > 0) {
        if (!strings || copy_from_user(t, pool_scratch, strings,
                                       (uint32_t)slen) != 0) {
            decl_reject(E_DECL_BADREC, tid); return E_DECL_BADREC;
        }
    }

    int rc = validate(rec_scratch, n, pool_scratch, slen, tid);
    if (rc != 0) { p->last_reject = rc; decl_reject(rc, tid); return rc; }

    if (commit_frame_cnt >= DECL_COMMIT_FRAME_MAX) {
        ser_write("GUI:decl throttle pid="); decl_dec(tid); ser_putc('\n');
        decl_reject(E_DECL_QUOTA, tid);
        return E_DECL_QUOTA;
    }
    commit_frame_cnt++;

    if (!pid_mem_alloc(p)) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }

    rc = decl_apply(p, rec_scratch, n, pool_scratch, slen);
    if (rc != 0) { p->last_reject = rc; decl_reject(rc, tid); return rc; }

    p->used = 1;
    p->stat_frames++;
    ser_write("GUI:decl commit n="); decl_dec(n);
    ser_write(" ok=1 pid="); decl_dec(tid); ser_putc('\n');
    return 0;
}

/* 校验通过后的落地收尾（节流 / 帧分配 / apply / 统计打点）。 */
static int commit_finish(struct pid_meta *p, int tid, int n, uint32_t rec_size) {
    if (commit_frame_cnt >= DECL_COMMIT_FRAME_MAX) {
        ser_write("GUI:decl throttle pid="); decl_dec(tid); ser_putc('\n');
        decl_reject(E_DECL_QUOTA, tid);
        return E_DECL_QUOTA;
    }
    commit_frame_cnt++;

    if (!pid_mem_alloc(p)) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }

    int rc;
    if (rec_size == (uint32_t)sizeof(ce_blk_rec2_t))
        rc = decl_apply2(p, (const ce_blk_rec2_t *)rec_scratch, n,
                         pool_scratch, 0);
    else
        rc = decl_apply(p, rec_scratch, n, pool_scratch, 0);
    if (rc != 0) { p->last_reject = rc; decl_reject(rc, tid); return rc; }

    p->used = 1;
    p->stat_frames++;
    ser_write("GUI:decl commit n="); decl_dec(n);
    ser_write(" ok=1 pid="); decl_dec(tid); ser_putc('\n');
    return 0;
}

/* ---- cap2：能力探测 / rec2 提交 / 事件2 轮询（append-only） -------------- */

uint32_t decl_caps(void) { return DECL_CAP2_ALL; }

int decl_commit2(struct task *t, const void *recs, int n,
                 const char *strings, int slen, uint32_t rec_size) {
    int tid = t ? t->tid : -1;
    struct pid_meta *p = ps_get(tid);
    if (!p) return E_EPERM;
    p->tid = tid;

    /* rec_size==40：完全委托旧 decl_commit()，逐字节等价，不吃任何 rec2 逻辑。 */
    if (rec_size == 40) return decl_commit(t, recs, n, strings, slen);
    if (rec_size != (uint32_t)sizeof(ce_blk_rec2_t)) {
        decl_reject(E_DECL_BADREC, tid); return E_DECL_BADREC;
    }

    if (n < 0 || n > DECL_MAX_BLOCKS) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }
    if (slen < 0 || slen > DECL_MAX_POOL) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }
    if (!scratch_alloc()) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }

    if (n > 0) {
        if (!recs || copy_from_user(t, rec_scratch, recs,
                                    (uint32_t)n * rec_size) != 0) {
            decl_reject(E_DECL_BADREC, tid); return E_DECL_BADREC;
        }
    }
    if (slen > 0) {
        if (!strings || copy_from_user(t, pool_scratch, strings,
                                       (uint32_t)slen) != 0) {
            decl_reject(E_DECL_BADREC, tid); return E_DECL_BADREC;
        }
    }

    int rc = validate2((const ce_blk_rec2_t *)rec_scratch, n,
                       pool_scratch, slen, tid);
    if (rc != 0) { p->last_reject = rc; decl_reject(rc, tid); return rc; }
    return commit_finish(p, tid, n, rec_size);
}

/* ---- 探针专用入口（内核内自检使用；不走 copy_from_user） ---------------- */

int decl_ce_probe_commit(int tid, const void *recs, int n,
                         const char *strings, int slen, uint32_t rec_size) {
    struct pid_meta *p = ps_get(tid);
    if (!p) return E_EPERM;
    p->tid = tid;
    if (n < 0 || n > DECL_MAX_BLOCKS) return E_DECL_QUOTA;
    if (slen < 0 || slen > DECL_MAX_POOL) return E_DECL_QUOTA;
    if (rec_size != 40 && rec_size != (uint32_t)sizeof(ce_blk_rec2_t))
        return E_DECL_BADREC;
    if (!scratch_alloc()) return E_DECL_QUOTA;
    if (n > 0) memcpy(rec_scratch, recs, (uint32_t)n * rec_size);
    if (slen > 0) memcpy(pool_scratch, strings, (uint32_t)slen);

    int rc;
    if (rec_size == 40)
        rc = validate(rec_scratch, n, pool_scratch, slen, tid);
    else
        rc = validate2((const ce_blk_rec2_t *)rec_scratch, n,
                       pool_scratch, slen, tid);
    if (rc != 0) { p->last_reject = rc; return rc; }
    /* 探针为同步自检步骤，不受每帧提交节流预算约束。 */
    commit_frame_cnt = 0;
    return commit_finish(p, tid, n, rec_size);
}

int decl_ce_probe_poll2(int tid, ce_blk_event2_t *ev) {
    struct pid_meta *p = ps_get(tid);
    if (!p || !ev) return E_EPERM;
    if (!p->used || p->evh == p->evt) return 0;
    *ev = p->evq[p->evh];
    p->evh = (p->evh + 1) % DECL_EVQ;
    return 1;
}

block_id decl_ce_probe_local_to_global(int tid, int lid) {
    struct pid_meta *p = ps_get(tid);
    if (!p || !p->mem) return 0;
    uint16_t *gm = pgmap(p);
    int16_t  *lm = plmap(p);
    for (int i = 0; i < p->count; i++)
        if (lm[i] == (int16_t)lid) return gm[i];
    return 0;
}

int decl_ce_probe_count(int tid) {
    struct pid_meta *p = ps_get(tid);
    return p ? p->count : 0;
}

/* 探针/诊断可读：本 pid 事件队列累计丢弃条数（满队丢最旧策略，见 enqueue2）。 */
int decl_ce_probe_dropped(int tid) {
    struct pid_meta *p = ps_get(tid);
    return p ? (int)p->dropped : -1;
}

/* ---- 事件轮询 ------------------------------------------------------------ */

/* 旧 type → cap2 ev_kind 映射（未映射的旧类型 ev_kind 留 0）。 */
static uint8_t ev_kind_from_type(int16_t type) {
    switch (type) {
    case DECL_EV_CLICK:    return 0;
    case DECL_EV_FOCUS_IN: return 1;
    case DECL_EV_FOCUS_OUT:return 2;
    case DECL_EV_KEY:      return 5;
    case DECL_EV_CLOSE:    return 6;
    default:               return 0;
    }
}

/* 满队策略：丢最旧腾位（overwrite head），新事件优先入队；被淘汰的队头
 * 最旧事件不再回传。丢弃累计到 p->dropped 并串口打点，绝不静默丢失。
 * （备选“保留丢新”语义等价于计数后直接 return，本包选定丢最旧：新事件
 * 不被背积压的旧事件堵在门外。） */
static void enqueue2(struct pid_meta *p, ce_blk_event2_t *e) {
    int next = (p->evt + 1) % DECL_EVQ;
    if (next == p->evh) {
        p->evh = (p->evh + 1) % DECL_EVQ;   /* 淘汰队头最旧事件 */
        p->dropped++;
        ser_write("GUI:ev overflow pid="); decl_dec(p->tid);
        ser_write(" dropped="); decl_dec(p->dropped); ser_putc('\n');
    }
    e->seq = ++p->seq;
    p->evq[p->evt] = *e;
    p->evt = next;
    p->stat_events++;
}

/* 旧事件入队包装：前 sizeof(ev1) 字段逐字节保留，扩展段清零。 */
static void enqueue(struct pid_meta *p, const ce_blk_event_t *e) {
    ce_blk_event2_t e2;
    memset(&e2, 0, sizeof(e2));
    memcpy(&e2, e, sizeof(ce_blk_event_t));
    e2.ev_kind = ev_kind_from_type(e->type);
    enqueue2(p, &e2);
}

/* UTF-8 安全截断：按整字符装入 cap-1 字节，末尾 NUL；不切断多字节字符。 */
static void utf8_clip(uint8_t *dst, int cap, const char *src) {
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (i < cap - 1 && src[i]) {
        uint8_t c = (uint8_t)src[i];
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (i + len > cap - 1) break;
        for (int k = 0; k < len; k++) dst[i + k] = (uint8_t)src[i + k];
        i += len;
    }
    dst[i] = 0;
}

static int block_local_id(block *b) {
    if (!b || b->owner == 0 || b->owner >= MAX_TASKS) return 0;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used || !p->mem) return 0;
    uint16_t *gm = pgmap(p);
    int16_t  *lm = plmap(p);
    for (int i = 0; i < p->count; i++)
        if (gm[i] == b->id) return lm[i];
    return 0;
}

/* 跨 pid damage 隔离：子树内每一块都必须归属 tid，越界即判失败。 */
static int subtree_owner_ok(block *b, int tid) {
    if (!b) return 1;
    if (b->owner != tid) return 0;
    for (block *c = b->first_child; c; c = c->next_sibling)
        if (!subtree_owner_ok(c, tid)) return 0;
    return 1;
}

void decl_post_click(block *b, int lx, int ly) {
    if (!b) return;
    if (b->owner == 0 || b->owner >= MAX_TASKS) return;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used) return;

    int lid = block_local_id(b);
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DECL_EV_CLICK; ev.code = 0; ev.id = lid;
    ev.x = lx; ev.y = ly; ev.w = 0; ev.h = 0;
    ev.target_id = b->id; ev.ev_kind = 0;
    enqueue2(p, &ev);

    ser_write("GUI:ev click id="); decl_dec(lid);
    ser_write(" x="); decl_dec(lx);
    ser_write(" y="); decl_dec(ly); ser_putc('\n');
}

static void focus_post(struct pid_meta *p, int in) {
    ce_blk_event_t ev;
    ev.type = in ? DECL_EV_FOCUS_IN : DECL_EV_FOCUS_OUT;
    ev.code = 0; ev.id = 0; ev.x = 0; ev.y = 0; ev.w = 0; ev.h = 0;
    enqueue(p, &ev);
    ser_write(in ? "GUI:ev focus_in id=0\n" : "GUI:ev focus_out id=0\n");
}

void decl_focus_set(int tid) {
    if (focus_tid == tid) return;
    if (focus_tid >= 0) {
        struct pid_meta *o = ps_get(focus_tid);
        if (o && o->used) focus_post(o, 0);
    }
    focus_tid = tid;
    if (tid >= 0) {
        struct pid_meta *n = ps_get(tid);
        if (n && n->used) focus_post(n, 1);
    }
}

void decl_focus_next(void) {            /* Tab：在已挂载应用间循环焦点 */
    int start = focus_tid;
    for (int i = 1; i <= MAX_TASKS; i++) {
        int t = ((start + i) % MAX_TASKS + MAX_TASKS) % MAX_TASKS;
        if (ps[t].used) { decl_focus_set(t); return; }
    }
}

void decl_focus_clear(void) {           /* Esc：焦点归还内核 */
    decl_focus_set(-1);
}

void decl_post_key(int code, int mods) {
    /* 焦点归属：投给当前焦点应用；无焦点时回退最近活动应用。 */
    int target = focus_tid;
    if (target < 0 || !ps_get(target) || !ps_get(target)->used) {
        for (int t = MAX_TASKS - 1; t >= 0; t--)
            if (ps[t].used) { target = t; break; }
    }
    if (target < 0) return;
    struct pid_meta *p = ps_get(target);
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DECL_EV_KEY; ev.code = (int16_t)code; ev.id = 0;
    ev.ev_kind = 5; ev.mods = (uint8_t)mods;
    enqueue2(p, &ev);
    ser_write("GUI:ev key code="); decl_dec(code);
    ser_write(" mods="); decl_dec(mods);
    ser_write(" pid="); decl_dec(target); ser_putc('\n');
}

void decl_post_focus(block *b, int in) {
    if (!b || b->owner == 0 || b->owner >= MAX_TASKS) return;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used) return;
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = in ? DECL_EV_FOCUS_IN : DECL_EV_FOCUS_OUT;
    ev.code = 0; ev.id = block_local_id(b);
    ev.target_id = b->id;
    ev.ev_kind = in ? 1 : 2;
    enqueue2(p, &ev);
    ser_write(in ? "GUI:ev focus_in id=" : "GUI:ev focus_out id=");
    decl_dec(ev.id); ser_putc('\n');
}

void decl_post_resize(block *b, int w, int h) {
    if (!b || b->owner == 0 || b->owner >= MAX_TASKS) return;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used) return;
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DECL_EV_RESIZE; ev.code = 0; ev.id = block_local_id(b);
    ev.w = w; ev.h = h;
    ev.target_id = b->id;
    enqueue2(p, &ev);
    ser_write("GUI:ev resize id="); decl_dec(ev.id);
    ser_write(" w="); decl_dec(w); ser_write(" h="); decl_dec(h); ser_putc('\n');
}

void decl_post_close(block *b) {
    if (!b || b->owner == 0 || b->owner >= MAX_TASKS) return;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used) return;
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DECL_EV_CLOSE; ev.code = 0; ev.id = block_local_id(b);
    ev.target_id = b->id; ev.ev_kind = 6;
    enqueue2(p, &ev);
    ser_write("GUI:ev close id="); decl_dec(ev.id); ser_putc('\n');
}

/* ---- cap2 新事件入口：滚轮 / 输入提交 / 浮层关闭 -------------------------- */

void decl_post_wheel(block *b, int dx, int dy) {
    if (!b || b->owner == 0 || b->owner >= MAX_TASKS) return;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used) return;
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DECL_EV_KEY; ev.code = 0; ev.id = block_local_id(b);
    ev.target_id = b->id; ev.ev_kind = 4;
    ev.wheel_dx = (int16_t)dx; ev.wheel_dy = (int16_t)dy;
    enqueue2(p, &ev);
    ser_write("GUI:ev wheel id="); decl_dec(ev.id);
    ser_write(" dx="); decl_dec(dx);
    ser_write(" dy="); decl_dec(dy); ser_putc('\n');
}

void decl_post_input(block *b, const char *utf8) {
    if (!b || b->owner == 0 || b->owner >= MAX_TASKS) return;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used) return;
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DECL_EV_KEY; ev.code = 0; ev.id = block_local_id(b);
    ev.target_id = b->id; ev.ev_kind = 3;
    utf8_clip(ev.utext, sizeof(ev.utext), utf8);
    enqueue2(p, &ev);
    int ulen = 0;
    while (ev.utext[ulen]) ulen++;
    ser_write("GUI:ev input id="); decl_dec(ev.id);
    ser_write(" len="); decl_dec(ulen);
    ser_putc('\n');
}

void decl_post_dismiss(block *b) {
    if (!b || b->owner == 0 || b->owner >= MAX_TASKS) return;
    struct pid_meta *p = &ps[b->owner];
    if (!p->used) return;
    ce_blk_event2_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = DECL_EV_CLOSE; ev.code = 0; ev.id = block_local_id(b);
    ev.target_id = b->id; ev.ev_kind = 6;
    enqueue2(p, &ev);
    ser_write("GUI:ev dismiss id="); decl_dec(ev.id); ser_putc('\n');
}

int decl_pid_blocks(int tid) {
    struct pid_meta *p = ps_get(tid);
    return p ? p->count : 0;
}

void decl_dump_pid(int tid) {
    struct pid_meta *p = ps_get(tid);
    if (!p) return;
    ser_write("GUI:blkdbg pid="); decl_dec(tid);
    ser_write(" blocks="); decl_dec(p->count);
    ser_write(" quota="); decl_dec(DECL_MAX_BLOCKS);
    ser_write(" frames="); decl_dec(p->stat_frames);
    ser_write(" events="); decl_dec(p->stat_events);
    ser_write(" damage="); decl_dec(p->stat_damage);
    ser_write(" toast="); decl_dec(p->toast_hints);
    ser_write(" dropped="); decl_dec(p->dropped);
    ser_write(" last_reject="); decl_dec(p->last_reject); ser_putc('\n');
}

/* deskev 门禁用：转储所有 CE（owner != 0）块的实时几何，供驱动定位
 * 按钮/输入框矩形。只读，不改任何状态。 */
void decl_dump_ce_rects(void) {
    uint32_t cap = btable_capacity();
    for (uint32_t id = 1; id <= cap; id++) {
        block *b = btable_get((block_id)id);
        if (!b || b->id == 0 || b->owner == 0) continue;
        ser_write("GUI:deskev pid="); decl_dec((int)b->owner);
        ser_write(" id="); decl_dec((int)b->id);
        ser_write(" kind="); decl_dec((int)b->kind);
        ser_write(" x="); decl_dec(b->x);
        ser_write(" y="); decl_dec(b->y);
        ser_write(" w="); decl_dec(b->w);
        ser_write(" h="); decl_dec(b->h);
        ser_putc('\n');
    }
}

int decl_event_poll(struct task *t, ce_blk_event_t *ev, int timeout_ms) {
    (void)timeout_ms;
    int tid = t ? t->tid : -1;
    struct pid_meta *p = ps_get(tid);
    if (!p || !ev) return E_EPERM;
    if (!p->used || p->evh == p->evt) return 0;
    /* 同一队列：旧入口只取前 sizeof(ce_blk_event_t) 字节，字段语义不变。 */
    memcpy(ev, &p->evq[p->evh], sizeof(ce_blk_event_t));
    p->evh = (p->evh + 1) % DECL_EVQ;
    return 1;
}

int decl_event_poll2(struct task *t, ce_blk_event2_t *ev, int timeout_ms) {
    (void)timeout_ms;
    int tid = t ? t->tid : -1;
    struct pid_meta *p = ps_get(tid);
    if (!p || !ev) return E_EPERM;
    if (!p->used || p->evh == p->evt) return 0;
    *ev = p->evq[p->evh];
    p->evh = (p->evh + 1) % DECL_EVQ;
    return 1;
}

/* ---- 图片资源（内核解码，只读句柄） -------------------------------------- */

static int img_decode_ppm(struct img_meta *r, const char *path) {
    char hdr[256];
    int n = fs_read_at(path, hdr, 0, sizeof(hdr) - 1);
    if (n < 5) return -1;
    hdr[n] = 0;
    if (hdr[0] != 'P' || hdr[1] != '6') return -1;
    int p = 2, vals[3] = { 0, 0, 0 };
    for (int v = 0; v < 3; v++) {
        while (hdr[p] == ' ' || hdr[p] == '\t' || hdr[p] == '\n' || hdr[p] == '\r' || hdr[p] == '#') {
            if (hdr[p] == '#') while (hdr[p] && hdr[p] != '\n') p++; else p++;
        }
        while (hdr[p] >= '0' && hdr[p] <= '9') { vals[v] = vals[v] * 10 + (hdr[p] - '0'); p++; }
    }
    int w = vals[0], h = vals[1];
    if (w <= 0 || h <= 0 || w > IMG_W || h > IMG_H) return -1;
    static uint8_t row[IMG_W * 3];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, p + y * w * 3, w * 3);
        if (got < w * 3) return -1;
        for (int x = 0; x < w; x++) {
            uint8_t *d = &r->pix[(y * w + x) * 3];
            d[0] = row[x * 3]; d[1] = row[x * 3 + 1]; d[2] = row[x * 3 + 2];
        }
    }
    r->w = (uint16_t)w; r->h = (uint16_t)h;
    return 0;
}

static int img_decode_bmp(struct img_meta *r, const char *path) {
    uint8_t hdr[64];
    int n = fs_read_at(path, (char *)hdr, 0, sizeof(hdr));
    if (n < 34) return -1;
    if (hdr[0] != 'B' || hdr[1] != 'M') return -1;
    uint32_t data_off = (uint16_t)(hdr[10] | (hdr[11] << 8));
    int32_t info = (int32_t)(hdr[14] | (hdr[15] << 8) | (hdr[16] << 16) | ((uint32_t)hdr[17] << 24));
    if (info < 40) return -1;
    int32_t wh = (int32_t)(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
    int32_t w = (int32_t)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | ((uint32_t)hdr[21] << 24));
    int bpp = (uint16_t)(hdr[28] | (hdr[29] << 8));
    uint32_t comp = (uint32_t)(hdr[30] | (hdr[31] << 8) | (hdr[32] << 16) | ((uint32_t)hdr[33] << 24));
    if (comp != 0 || (bpp != 24 && bpp != 32)) return -1;
    if (w <= 0 || w > IMG_W) return -1;
    int topdown = wh < 0;
    int h = topdown ? -wh : wh;
    if (h <= 0 || h > IMG_H) return -1;

    int bpc = bpp / 8;
    int stride = ((w * bpc + 3) & ~3);
    static uint8_t row[IMG_W * 4];
    for (int y = 0; y < h; y++) {
        int got = fs_read_at(path, (char *)row, data_off + y * stride, stride);
        if (got < bpc * w) return -1;
        int dst = topdown ? y : (h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t B = row[x * bpc], G = row[x * bpc + 1], R = row[x * bpc + 2];
            uint8_t *d = &r->pix[(dst * w + x) * 3];
            d[0] = R; d[1] = G; d[2] = B;
        }
    }
    r->w = (uint16_t)w; r->h = (uint16_t)h;
    return 0;
}

int decl_image_load(struct task *t, const char *path) {
    int tid = t ? t->tid : -1;
    if (!path) return E_DECL_BADREC;
    int res = -1;
    for (int i = 0; i < IMG_MAX; i++) if (!imgs[i].used) { res = i; break; }
    if (res < 0) return E_DECL_QUOTA;

    struct img_meta *r = &imgs[res];
    int bytes = IMG_W * IMG_H * 3;
    int pages = (bytes + 4095u) / 4096u;
    for (int p = 0; p < pages; p++) {
        uint32_t pa = frame_alloc();
        if (!pa) return E_DECL_QUOTA;
        r->phys[p] = pa;
    }
    r->pix = (uint8_t *)r->phys[0];
    r->pages = pages;

    if (img_decode_ppm(r, path) != 0 && img_decode_bmp(r, path) != 0) {
        for (int p = 0; p < pages; p++) frame_free(r->phys[p]);
        return E_DECL_RES;
    }
    r->used = 1; r->tid = tid;
    return res;
}

static void img_free_slot(struct img_meta *r) {
    for (int p = 0; p < r->pages; p++) frame_free(r->phys[p]);
    r->used = 0; r->tid = -1; r->pix = 0; r->pages = 0;
}

int decl_image_free(struct task *t, int res) {
    int tid = t ? t->tid : -1;
    struct img_meta *r = img_find(res, tid);
    if (!r) return E_DECL_RES;
    img_free_slot(r);
    return 0;
}

/* ---- 文本度量（8×16 固定字号） ------------------------------------------ */

int decl_text_width(const char *s) {
    if (!s) return 0;
    int w = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        if (*p < 0x80) { w += 8; p++; }
        else if (*p >= 0x81 && *p <= 0xFE) { w += 16; p += 2; }
        else p++;
    }
    return w;
}

/* ---- 回收 ---------------------------------------------------------------- */

static void decl_cleanup_tid(int tid) {
    struct pid_meta *p = ps_get(tid);
    if (!p) return;
    ib_reset_if_owner(tid);
    if (p->root) {
        tree_free_subtree(btable_get(p->root));
        block *b = btable_get(p->root);
        if (b) btable_free(b);
    }
    for (int i = 0; i < IMG_MAX; i++)
        if (imgs[i].used && imgs[i].tid == tid) img_free_slot(&imgs[i]);
    if (p->mem) { frame_free((uint32_t)p->mem); p->mem = 0; }
    memset(p, 0, sizeof(*p));
    ser_write("GUI:decl leaked=0\n");
}

void decl_task_cleanup(struct task *t) {
    if (t) decl_cleanup_tid(t->tid);
}

/* 探针专用：按 tid 回收子树（内核自检使用）。 */
void decl_ce_probe_cleanup(int tid) { decl_cleanup_tid(tid); }

/* ---- 探针自检 ------------------------------------------------------------ */

static int decl_test_commit(int tid, const ce_blk_rec_t *r, int n,
                            const char *pool, int slen) {
    struct pid_meta *p = ps_get(tid);
    if (!p) return -1;
    p->tid = tid;
    int rc = validate(r, n, pool, slen, tid);
    if (rc != 0) { p->last_reject = rc; decl_reject(rc, tid); return rc; }
    if (!pid_mem_alloc(p)) { decl_reject(E_DECL_QUOTA, tid); return E_DECL_QUOTA; }
    rc = decl_apply(p, r, n, pool, slen);
    if (rc == 0) { p->used = 1; p->stat_frames++; ser_write("GUI:decl commit n="); decl_dec(n);
                   ser_write(" ok=1 pid="); decl_dec(tid); ser_putc('\n'); }
    return rc;
}

int decl_self_test(void) {
    int tid = 1;
    decl_cleanup_tid(tid);

    ce_blk_rec_t recs[3];
    memset(recs, 0, sizeof(recs));
    char pool[16]; pool[0] = 'H'; pool[1] = 'i'; pool[2] = 0;

    recs[0].id = 1; recs[0].parent = 0;
    recs[0].kind = BLK_COLUMN; recs[0].layer = LAYER_WORK;
    recs[0].req_w = -1; recs[0].req_h = -1;
    recs[0].text_off = -1; recs[0].pix_off = -1; recs[0].res = -1;

    recs[1].id = 2; recs[1].parent = 1;
    recs[1].kind = BLK_TEXT; recs[1].layer = LAYER_WORK;
    recs[1].text_off = 0; recs[1].pix_off = -1; recs[1].res = -1;

    recs[2].id = 3; recs[2].parent = 1;
    recs[2].kind = BLK_RECT; recs[2].layer = LAYER_WORK;
    recs[2].pix_off = -1; recs[2].res = -1;

    int rc = decl_test_commit(tid, recs, 3, pool, 2);
    if (rc != 0) { decl_cleanup_tid(tid); return 1; }

    struct pid_meta *p = ps_get(tid);
    uint32_t h1 = p->hash;
    int count1 = p->count;

    /* 幂等：重复提交同一描述，结构哈希不变、不重绘。 */
    rc = decl_test_commit(tid, recs, 3, pool, 2);
    if (rc != 0 || p->hash != h1 || p->count != count1) { decl_cleanup_tid(tid); return 1; }

    /* 越层拒绝：TOAST 层。 */
    recs[1].layer = LAYER_TOAST;
    rc = decl_test_commit(tid, recs, 3, pool, 2);
    if (rc != E_DECL_LAYER) { decl_cleanup_tid(tid); return 1; }
    recs[1].layer = LAYER_WORK;

    /* 配额拒绝：深度 17 层（父链先于子出现）。 */
    {
        static ce_blk_rec_t deep[DECL_MAX_DEPTH + 1];
        memset(deep, 0, sizeof(deep));
        for (int i = 0; i <= DECL_MAX_DEPTH; i++) {
            deep[i].id = i + 1;
            deep[i].parent = i ? i : 0;
            deep[i].kind = BLK_COLUMN;
            deep[i].layer = LAYER_WORK;
            deep[i].text_off = -1;
            deep[i].pix_off = -1; deep[i].res = -1;
        }
        rc = decl_test_commit(tid, deep, DECL_MAX_DEPTH + 1, "", 0);
        if (rc != E_DECL_QUOTA) { decl_cleanup_tid(tid); return 1; }
    }

    /* 事件实泵：合成一次点击 + 焦点/缩放/关闭，回读队列断言。 */
    {
        block *tb = btable_get(pgmap(p)[1]);   /* 文本块 local id 2 */
        decl_post_click(tb, 5, 7);
        decl_post_focus(tb, 1);
        decl_post_resize(tb, 40, 12);
        decl_post_close(tb);

        int16_t types[4]; int n = 0;
        ce_blk_event_t click;
        memset(&click, 0, sizeof(click));
        int have_click = 0;
        while (p->evh != p->evt && n < 4) {
            ce_blk_event_t e;
            memcpy(&e, &p->evq[p->evh], sizeof(e));
            if (e.type == DECL_EV_CLICK && !have_click) { click = e; have_click = 1; }
            types[n++] = e.type;
            p->evh = (p->evh + 1) % DECL_EVQ;
        }
        if (n != 4 || types[0] != DECL_EV_CLICK ||
            types[1] != DECL_EV_FOCUS_IN || types[2] != DECL_EV_RESIZE ||
            types[3] != DECL_EV_CLOSE) {
            decl_cleanup_tid(tid); return 1;
        }
        if (!have_click || click.id != 2 || click.x != 5 || click.y != 7) {
            decl_cleanup_tid(tid); return 1;
        }
    }

    /* #33：跨 pid 隔离 + 焦点链 + per-pid 统计 + TOAST 提示断言。 */
    {
        int tid2 = 2;
        decl_cleanup_tid(tid2);

        ce_blk_rec_t r2;
        memset(&r2, 0, sizeof(r2));
        r2.id = 1; r2.parent = 0;
        r2.kind = BLK_RECT; r2.layer = LAYER_WORK;
        r2.text_off = -1; r2.pix_off = -1; r2.res = -1;
        if (decl_test_commit(tid2, &r2, 1, "", 0) != 0) {
            decl_cleanup_tid(tid); decl_cleanup_tid(tid2); return 1;
        }
        struct pid_meta *p2 = ps_get(tid2);

        /* 隔离：两棵子树归属各自 pid；pid1 映射表不得出现 pid2 的块。 */
        if (!subtree_owner_ok(btable_get(p->root), tid) ||
            !subtree_owner_ok(btable_get(p2->root), tid2)) {
            decl_cleanup_tid(tid); decl_cleanup_tid(tid2); return 1;
        }
        block *b2 = btable_get(pgmap(p2)[0]);
        for (int i = 0; i < p->count; i++)
            if (pgmap(p)[i] == b2->id) {
                decl_cleanup_tid(tid); decl_cleanup_tid(tid2); return 1;
            }

        /* 焦点链：置焦 pid1 → 按键进 pid1；Tab 循环到 pid2 → 按键进 pid2；Esc 归还。 */
        decl_focus_set(tid);
        decl_post_key(0x10, 0);
        decl_focus_next();
        decl_post_key(0x11, 0);
        decl_focus_clear();

        int k1 = 0, k2 = 0, fi = 0, fo = 0;
        while (p->evh != p->evt) {
            ce_blk_event_t e;
            memcpy(&e, &p->evq[p->evh], sizeof(e));
            if (e.type == DECL_EV_KEY) k1 = 1;
            p->evh = (p->evh + 1) % DECL_EVQ;
        }
        while (p2->evh != p2->evt) {
            ce_blk_event_t e;
            memcpy(&e, &p2->evq[p2->evh], sizeof(e));
            if (e.type == DECL_EV_KEY) k2 = 1;
            if (e.type == DECL_EV_FOCUS_IN) fi = 1;
            if (e.type == DECL_EV_FOCUS_OUT) fo = 1;
            p2->evh = (p2->evh + 1) % DECL_EVQ;
        }
        if (!k1 || !k2 || !fi || !fo) {
            decl_cleanup_tid(tid); decl_cleanup_tid(tid2); return 1;
        }

        /* per-pid 统计：落地帧/脏标记/事件均应非零。 */
        if (p->stat_frames < 1 || p->stat_damage < 3 || p->stat_events <= 0) {
            decl_cleanup_tid(tid); decl_cleanup_tid(tid2); return 1;
        }

        /* TOAST 提示：非法整批被拒后应增加一条系统提示计数并打点。 */
        ce_blk_rec_t bad;
        memset(&bad, 0, sizeof(bad));
        bad.id = 1; bad.parent = 0;
        bad.kind = BLK_RECT; bad.layer = LAYER_TOAST;
        bad.text_off = -1; bad.pix_off = -1; bad.res = -1;
        int before = p->toast_hints;
        if (decl_test_commit(tid, &bad, 1, "", 0) != E_DECL_LAYER) {
            decl_cleanup_tid(tid); decl_cleanup_tid(tid2); return 1;
        }
        if (p->toast_hints <= before) {
            decl_cleanup_tid(tid); decl_cleanup_tid(tid2); return 1;
        }

        decl_cleanup_tid(tid2);
    }

    /* 退出回收：整棵子树清空，泄露 0。 */
    decl_cleanup_tid(tid);
    ser_write("GUI:decl result=1 leaked=0\n");
    return 0;
}
