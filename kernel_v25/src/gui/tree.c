/*
 * tree.c -- 块树挂载 / 摘除 / 子树销毁（规格 §1 投影规则）。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "kstring.h"
#include "task.h"

#define ROOT_ID 1000u

static block   root_node;
static uint32_t n_tree;                 /* 含根节点 */

block *gui_root_block(void) { return &root_node; }

void tree_attach(block *parent, block *child) {
    if (!parent || !child) return;
    child->parent = parent;
    if (!parent->first_child) {
        parent->first_child = child;
    } else {
        block *c = parent->first_child;
        while (c->next_sibling) c = c->next_sibling;
        c->next_sibling = child;
    }
    n_tree++;
}

void tree_detach(block *b) {
    if (!b || !b->parent) return;
    block *p = b->parent;
    if (p->first_child == b) {
        p->first_child = b->next_sibling;
    } else {
        block *c = p->first_child;
        while (c && c->next_sibling != b) c = c->next_sibling;
        if (c) c->next_sibling = b->next_sibling;
    }
    b->parent = 0;
    b->next_sibling = 0;
    if (n_tree) n_tree--;
}

void tree_free_subtree(block *b) {
    if (!b) return;
    block *child = b->first_child;
    while (child) {
        block *nxt = child->next_sibling;
        tree_free_subtree(child);
        child = nxt;
    }
    tree_detach(b);
    btable_free(b);
}

uint32_t tree_count_nodes(void) { return n_tree; }

static uint32_t subtree_walk(block *b) {
    if (!b) return 0;
    uint32_t n = 1;
    for (block *c = b->first_child; c; c = c->next_sibling)
        n += subtree_walk(c);
    return n;
}

uint32_t tree_count_subtree(block *b) { return subtree_walk(b); }

void tree_reset(void) {
    while (root_node.first_child)
        tree_free_subtree(root_node.first_child);
    memset(&root_node, 0, sizeof(root_node));
    root_node.id = ROOT_ID;
    root_node.kind = BLK_ROOT;
    root_node.layer = LAYER_BG;
    n_tree = 1;
}

/* 保留子树：整棵标记重排，清掉控制台阶段留下的旧伤害矩形/离场态，
 * 避免首帧拿过期 old 坐标算脏区。 */
static void keep_walk(block *b) {
    if (!b) return;
    b->dirty_layout = 1;
    b->has_old = 0;
    b->old_x = b->old_y = b->old_w = b->old_h = 0;
    b->leaving = 0;
    for (block *c = b->first_child; c; c = c->next_sibling)
        keep_walk(c);
}

/* 会话重建（gui_begin）专用：owner != 0 的 CE 顶层子树不回收，摘链暂存后
 * 重新挂回根链；owner == 0 的内核会话块按 tree_reset 原口径整树销毁。
 * 控制台阶段已提交声明的 ring3 应用子树因此跨进桌面存活，点击/输入事件
 * 仍能命中其块。块 id、锚点映射、文本/图像指针与事件队列原样保留。 */
void tree_reset_keep_ce(void) {
    block *kept[MAX_TASKS];
    int n_kept = 0;

    while (root_node.first_child) {
        block *c = root_node.first_child;
        block *nxt = c->next_sibling;
        if (c->owner != 0) {
            /* 摘链但不回收：从 root 兄弟链脱开，挂入暂存数组。 */
            root_node.first_child = nxt;
            c->next_sibling = 0;
            c->parent = 0;
            if (n_kept < MAX_TASKS) kept[n_kept++] = c;
            else tree_free_subtree(c);   /* 超上限兜底：不悬空、不泄漏 */
        } else {
            tree_free_subtree(c);
        }
    }

    memset(&root_node, 0, sizeof(root_node));
    root_node.id = ROOT_ID;
    root_node.kind = BLK_ROOT;
    root_node.layer = LAYER_BG;
    n_tree = 1;

    for (int i = 0; i < n_kept; i++) {
        tree_attach(&root_node, kept[i]);
        keep_walk(kept[i]);
    }
    /* tree_attach 每挂一棵 +1；这里按子树实有节点重算，避免根节点重复计数。 */
    n_tree = 1;
    for (int i = 0; i < n_kept; i++)
        n_tree += tree_count_subtree(kept[i]);
}
