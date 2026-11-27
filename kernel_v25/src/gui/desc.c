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

static void apply_spec(block *b, const blk_spec *s) {
    if (!b) return;
    if (!s) return;

    int geo_changed = 0;
    if (b->req_w != s->req_w || b->req_h != s->req_h) geo_changed = 1;
    if (b->pad_t != s->pad_t || b->pad_r != s->pad_r ||
        b->pad_b != s->pad_b || b->pad_l != s->pad_l || b->gap != s->gap)
        geo_changed = 1;
    if (b->pct_w != s->pct_w || b->pct_h != s->pct_h ||
        b->min_w != s->min_w || b->min_h != s->min_h ||
        b->max_w != s->max_w || b->max_h != s->max_h ||
        b->anchor != s->anchor || b->overflow != s->overflow)
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

    /* v2 自由布局 / 溢出 / 交互属性镜像。 */
    b->overflow = s->overflow;
    b->anchor = s->anchor;
    b->pct_x = s->pct_x; b->pct_y = s->pct_y;
    b->pct_w = s->pct_w; b->pct_h = s->pct_h;
    b->px_x = s->px_x; b->px_y = s->px_y;
    b->min_w = s->min_w; b->min_h = s->min_h;
    b->max_w = s->max_w; b->max_h = s->max_h;
    b->m_t = s->m_t; b->m_r = s->m_r; b->m_b = s->m_b; b->m_l = s->m_l;
    b->drag_mode = s->drag_mode; b->drag_bounds = s->drag_bounds;
    b->resize_dir = s->resize_dir; b->resize_edge = s->resize_edge;
    b->resize_min_w = s->resize_min_w ? s->resize_min_w : 24;
    b->resize_min_h = s->resize_min_h ? s->resize_min_h : 24;
    b->resize_max_w = s->resize_max_w; b->resize_max_h = s->resize_max_h;
    b->resize_step = s->resize_step;
    b->scroll_x = s->scroll_x; b->scroll_y = s->scroll_y;

    /* ---- cap2 镜像（append-only）：声明 0 一律视为旧行为 ---- */
    if (b->wrap_lines != s->wrap_lines || b->font_size != s->font_size ||
        b->font_bold != s->font_bold || b->font_tabular != s->font_tabular ||
        b->line_h != s->line_h || b->border_sides != s->border_sides ||
        b->elevation != s->elevation)
        geo_changed = 1;
    b->alpha = s->alpha;            /* 0 = 未声明，消费点按 255 处理 */
    b->alpha_mode = s->alpha_mode;
    b->elevation = s->elevation;
    b->corner_tl = s->corner_tl; b->corner_tr = s->corner_tr;
    b->corner_br = s->corner_br; b->corner_bl = s->corner_bl;
    b->wrap_lines = s->wrap_lines;
    b->ellipsis = s->ellipsis;
    b->font_size = s->font_size;
    b->font_bold = s->font_bold;
    b->font_tabular = s->font_tabular;
    b->line_h = s->line_h;
    b->anim_ms = s->anim_ms;
    b->anim_ease = s->anim_ease;
    b->anim_mask = s->anim_mask;
    b->scrollbar = s->scrollbar;
    b->scroll_axis = s->scroll_axis;
    b->border_sides = s->border_sides;
    b->hc_force = s->hc_force;
    b->anchor_id = s->anchor_id;
    b->pop_prefer = s->pop_prefer;
    b->pop_max_h = s->pop_max_h;
    b->icon = s->icon;
    b->tint_role = s->tint_role;
    /* cap3：角形态（整块统一；显式非 SQUIRCLE 才参与形态切换） */
    if (b->corner_shape != s->corner_shape || b->corner_n != s->corner_n)
        geo_changed = 1;
    b->corner_shape = s->corner_shape;
    b->corner_n = s->corner_n;

    if (s->fg != INHERIT_COLOR)     b->st.fg = s->fg;
    if (s->bg != INHERIT_COLOR)     b->st.bg = s->bg;
    if (s->accent != INHERIT_COLOR) b->st.accent = s->accent;
    /* 角色跟随活动令牌；角色生效时不保留任何固化 RGB。 */
    b->st.role_fg = s->role_fg;
    b->st.role_bg = s->role_bg;
    b->st.role_accent = s->role_accent;
    if (b->st.role_fg)    b->st.fg = 0;
    if (b->st.role_bg)    b->st.bg = 0;
    if (b->st.role_accent) b->st.accent = 0;
    b->st.scale = s->scale ? s->scale : 1;
    b->st.border_w = s->border_w;
    b->stroke = s->stroke;
    if (!b->st.border_w) b->st.border_w = b->stroke;

    b->text = s->text;
    b->pix = s->pix;
    b->pix_w = s->pix_w;
    b->pix_h = s->pix_h;
    b->on_click = s->on_click;
    b->on_drag  = s->on_drag;
    b->on_dblclick = s->on_dblclick;
    b->render   = s->render;
    b->usr      = s->usr;

    if (geo_changed) b->dirty_layout = 1;
    else             b->dirty_paint = 1;
}

static block *create_block(blk_kind kind, block_id parent_id,
                           const blk_spec *s) {
    block *b = btable_alloc(kind);
    if (!b) return 0;
    b->click_on_up = (kind == BLK_BUTTON) ? 1 : 0;   /* 按钮默认松开命中触发 */
    b->icon_fit = 0;                                /* 默认整格 2× 居中；桌面条目显式置 1 */
    block *p = btable_get(parent_id);
    apply_spec(b, s);
    tree_attach(p ? p : gui_root_block(), b);
    b->dirty_layout = 1;
    return b;
}

block_id b_column(block_id parent, const blk_spec *s)  { block *b = create_block(BLK_COLUMN, parent, s);  return b ? b->id : 0; }
block_id b_row  (block_id parent, const blk_spec *s)  { block *b = create_block(BLK_ROW,    parent, s);  return b ? b->id : 0; }
block_id b_stack(block_id parent, const blk_spec *s)  { block *b = create_block(BLK_STACK,  parent, s);  return b ? b->id : 0; }
block_id b_free (block_id parent, const blk_spec *s)  { block *b = create_block(BLK_FREE,   parent, s);  return b ? b->id : 0; }
block_id b_rect (block_id parent, const blk_spec *s)  { block *b = create_block(BLK_RECT,   parent, s);  return b ? b->id : 0; }
block_id b_surface(block_id parent, const blk_spec *s){ block *b = create_block(BLK_SURFACE,parent,s);  return b ? b->id : 0; }
block_id b_divider(block_id parent, const blk_spec *s) { block *b = create_block(BLK_DIVIDER, parent, s); return b ? b->id : 0; }
block_id b_icon (block_id parent, const blk_spec *s, uint8_t ic) {
    block *b = create_block(BLK_ICON, parent, s);
    if (!b) return 0;
    b->icon = (uint8_t)(ic & 7);
    return b->id;
}

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

block_id b_button(block_id parent, const blk_spec *s, const char *txt) {
    blk_spec local = spec_or_inherit(parent, s);
    local.text = txt;
    block *b = create_block(BLK_BUTTON, parent, &local);
    return b ? b->id : 0;
}

block_id b_input(block_id parent, const blk_spec *s,
                 void (*on_submit)(block_id id, const char *text)) {
    blk_spec local = spec_or_inherit(parent, s);
    if (!local.focusable) local.focusable = 1;   /* 输入框必须可聚焦接管键盘 */
    block *b = create_block(BLK_INPUT, parent, &local);
    if (!b) return 0;
    ib_attach(b->id, on_submit);
    return b->id;
}

void b_set_props(block_id id, const blk_spec *s) {
    block *b = btable_get(id);
    if (!b) return;
    apply_spec(b, s);
}

static void mark_leaving(block *b) {
    if (!b) return;
    b->leaving = 1;
    b->dirty_layout = 0;
    b->dirty_paint = 0;
    for (block *c = b->first_child; c; c = c->next_sibling)
        mark_leaving(c);
}

void b_remove(block_id id) {
    block *b = btable_get(id);
    if (!b) return;
    block *parent = b->parent;
    /* 两阶段销毁：先摘出树并标 leaving，保留句柄一帧；
     * 下一帧由 gui_presence_sweep 回收并归还块表空闲链。 */
    tree_detach(b);
    mark_leaving(b);
    /* v2.80 hotfix: 摘掉的子树不再参与绘制，但它占过的像素没有任何节点
     * 负责重绘 -> 关闭菜单/组件后原地留一整块旧像素（看起来像"穿模/关不掉"）。
     * 把父块整棵标脏，让底层在同帧把该区域重画掉。 */
    if (parent) gui_mark_dirty(parent->id);
    else        gui_mark_dirty(gui_root_block()->id);
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
        /* 与 b_remove 同口径：摘树并标 leaving，保留句柄一帧，
         * 下一帧由 gui_presence_sweep 计数回收，不经本函数直接释放。 */
        tree_detach(b);
        mark_leaving(b);
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
