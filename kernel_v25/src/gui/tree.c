/*
 * tree.c -- 块树挂载 / 摘除 / 子树销毁（规格 §1 投影规则）。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "kstring.h"

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
