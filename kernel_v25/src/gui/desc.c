/*
 * desc.c -- 声明式建块 API 与描述 diff 提交（规格 §1 / §2 / §3）。
 *
 * build() 产出 blk_desc 数组；gui_commit_desc 按 id 与上帧 diff：
 * 同 id 更新变化属性并打脏位，新增插入，描述里消失的进 leaving。
 * gui_rebuild 全量重建；两条路径必须得到同一棵块树（验收 §9.1）。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "kstring.h"

#define INHERIT_COLOR 0xFFFFFFFFu

static void apply_spec(block *b, const blk_spec *s) {
    if (!b) return;
    if (!s) return;

    int geo_changed = 0;
    if (b->req_w != s->req_w || b->req_h != s->req_h) geo_changed = 1;
    if (b->pad_t != s->pad_t || b->pad_r != s->pad_r ||
        b->pad_b != s->pad_b || b->pad_l != s->pad_l || b->gap != s->gap)
        geo_changed = 1;

    b->req_w = s->req_w; b->req_h = s->req_h;
    b->pad_t = s->pad_t; b->pad_r = s->pad_r;
    b->pad_b = s->pad_b; b->pad_l = s->pad_l;
    b->gap = s->gap;
    b->align_main = s->align_main;
    b->align_cross = s->align_cross;
    b->align_self = s->align_self;
    b->layer = s->layer;
    b->z = s->z;
    b->visible = s->visible;
    b->focusable = s->focusable;
    b->inert = s->inert;

    if (s->fg != INHERIT_COLOR)   b->st.fg = s->fg;
    if (s->bg != INHERIT_COLOR)   b->st.bg = s->bg;
    if (s->accent != INHERIT_COLOR) b->st.accent = s->accent;
    b->st.scale = s->scale ? s->scale : 1;
    b->st.border_w = s->border_w;

    b->text = s->text;
    b->pix = s->pix;
    b->pix_w = s->pix_w;
    b->pix_h = s->pix_h;
    b->on_click = s->on_click;
    b->on_drag  = s->on_drag;

    if (geo_changed) b->dirty_layout = 1;
    else             b->dirty_paint = 1;
}

static block *create_block(blk_kind kind, block_id parent_id,
                           const blk_spec *s) {
    block *b = btable_alloc(kind);
    if (!b) return 0;
    block *p = btable_get(parent_id);
    apply_spec(b, s);
    tree_attach(p ? p : gui_root_block(), b);
    b->dirty_layout = 1;
    return b;
}

block_id b_column(block_id parent, const blk_spec *s)  { block *b = create_block(BLK_COLUMN, parent, s);  return b ? b->id : 0; }
block_id b_row  (block_id parent, const blk_spec *s)  { block *b = create_block(BLK_ROW,    parent, s);  return b ? b->id : 0; }
block_id b_stack(block_id parent, const blk_spec *s)  { block *b = create_block(BLK_STACK,  parent, s);  return b ? b->id : 0; }
block_id b_rect (block_id parent, const blk_spec *s)  { block *b = create_block(BLK_RECT,   parent, s);  return b ? b->id : 0; }
block_id b_surface(block_id parent, const blk_spec *s){ block *b = create_block(BLK_SURFACE,parent,s);  return b ? b->id : 0; }

static blk_spec spec_or_inherit(block_id parent, const blk_spec *s) {
    blk_spec local;
    if (s) local = *s; else memset(&local, 0, sizeof(local));
    if (!s) {
        block *p = btable_get(parent);
        if (p) local.layer = p->layer;  /* 叶子缺省继承父层，避免被父底色覆盖 */
        local.visible = 1;              /* 缺省可见，否则叶子被整棵跳过 */
    }
    return local;
}

block_id b_text(block_id parent, const blk_spec *s, const char *txt) {
    blk_spec local = spec_or_inherit(parent, s);
    local.text = txt;
    block *b = create_block(BLK_TEXT, parent, &local);
    return b ? b->id : 0;
}

block_id b_image(block_id parent, const blk_spec *s,
                 const uint8_t *pix, int w, int h) {
    blk_spec local = spec_or_inherit(parent, s);
    local.pix = pix; local.pix_w = w; local.pix_h = h;
    block *b = create_block(BLK_IMAGE, parent, &local);
    return b ? b->id : 0;
}

block_id b_space(block_id parent, int w, int h) {
    blk_spec local = spec_or_inherit(parent, 0);
    local.req_w = w; local.req_h = h;
    block *b = create_block(BLK_SPACER, parent, &local);
    return b ? b->id : 0;
}

void b_set_props(block_id id, const blk_spec *s) {
    block *b = btable_get(id);
    if (!b) return;
    apply_spec(b, s);
}

void b_remove(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    /* 两阶段销毁：v1 先摘树归还块表；leaving 帧计数留给 M3。 */
    tree_free_subtree(b);
}

/* ---- diff 提交 --------------------------------------------------------- */

static void mark_unseen_leaving(block *b) {
    if (!b) return;
    block *child = b->first_child;
    while (child) {
        block *nxt = child->next_sibling;
        mark_unseen_leaving(child);
        child = nxt;
    }
    if (b->desc_seen == 0) {
        tree_free_subtree(b);
    }
}

void gui_commit_desc(const blk_desc *d, int n) {
    if (!d || n <= 0) return;

    /* 1. 本轮未见标记清零。 */
    for (uint32_t id = 1; id <= btable_capacity(); id++) {
        block *b = btable_get(id);
        if (b) b->desc_seen = 0;
    }

    /* 2. 逐项匹配 / 插入。 */
    for (int i = 0; i < n; i++) {
        const blk_desc *dd = &d[i];
        block *b = btable_get(dd->id);
        if (b) {
            block *want_p = btable_get(dd->parent);
            if (b->parent != want_p) {
                tree_detach(b);
                tree_attach(want_p ? want_p : gui_root_block(), b);
            }
            apply_spec(b, dd->spec);
            if (dd->text) b->text = dd->text;
            if (dd->pix) { b->pix = dd->pix; b->pix_w = dd->pix_w; b->pix_h = dd->pix_h; }
        } else {
            b = btable_alloc_at(dd->kind, dd->id);
            if (!b) b = btable_alloc(dd->kind);
            if (b) {
                block *p = btable_get(dd->parent);
                apply_spec(b, dd->spec);
                tree_attach(p ? p : gui_root_block(), b);
                b->dirty_layout = 1;
                if (dd->text) b->text = dd->text;
                if (dd->pix) { b->pix = dd->pix; b->pix_w = dd->pix_w; b->pix_h = dd->pix_h; }
            }
        }
        if (b) b->desc_seen = 1;
    }

    /* 3. 描述里消失的节点进 leaving（v1 直接回收）。 */
    block *child = gui_root_block()->first_child;
    while (child) {
        block *nxt = child->next_sibling;
        mark_unseen_leaving(child);
        child = nxt;
    }
}

void gui_rebuild(const blk_desc *d, int n) {
    tree_reset();
    gui_commit_desc(d, n);
}

/* ---- 结构哈希（验收 §9.1：id/kind/父子/几何） ------------------------- */

static void hash_walk(block *b, uint32_t *h) {
    if (!b) return;
    uint32_t x = b->id;
    x = x * 131u + (uint32_t)b->kind;
    x = x * 131u + (b->parent ? b->parent->id : 0);
    x = x * 131u + (uint32_t)(b->x & 0xFFFFu);
    x = x * 131u + (uint32_t)(b->y & 0xFFFFu);
    x = x * 131u + (uint32_t)(b->w & 0xFFFFu);
    x = x * 131u + (uint32_t)(b->h & 0xFFFFu);
    *h ^= x;
    *h *= 2654435761u;
    block *child = b->first_child;
    while (child) {
        hash_walk(child, h);
        child = child->next_sibling;
    }
}

uint32_t gui_tree_hash(void) {
    uint32_t h = 0x811C9DC5u;
    hash_walk(gui_root_block(), &h);
    return h;
}
