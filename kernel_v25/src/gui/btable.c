/*
 * btable.c -- 块表：内核唯一真源（规格 §1）。
 *
 * 定长数组 + 空闲链；block_id = 数组下标 + 1。owner 字段预留
 * （0 = 内核，>0 = pid），v1 恒为 0，不做跨进程归属。
 */
#include "gui/gui.h"
#include "gui_int.h"
#include "kstring.h"

#define GUI_MAX_BLOCKS 256

static block     slots[GUI_MAX_BLOCKS];
static block    *free_head;
static uint32_t  n_live;          /* 已分配、尚未归还块表的节点数 */

block *btable_get(block_id id) {
    if (id == 0 || id > GUI_MAX_BLOCKS) return 0;
    block *b = &slots[id - 1];
    return b->id == id ? b : 0;
}

static void free_unlink(block *target) {
    block **pp = &free_head;
    while (*pp) {
        if (*pp == target) { *pp = target->free_next; return; }
        pp = &(*pp)->free_next;
    }
}

/* 按描述里的显式 id 落槽；id 槽位被占用时返回 0，由调用方退到空闲链。 */
block *btable_alloc_at(blk_kind kind, block_id id) {
    if (id == 0 || id > GUI_MAX_BLOCKS) return 0;
    block *b = &slots[id - 1];
    if (b->id != 0) return 0;
    free_unlink(b);
    memset(b, 0, sizeof(*b));
    b->id = id;
    b->kind = kind;
    n_live++;
    return b;
}

block *btable_alloc(blk_kind kind) {
    block *b = 0;
    if (free_head) {
        b = free_head;
        free_head = b->free_next;
    } else {
        for (int i = 0; i < GUI_MAX_BLOCKS; i++) {
            if (slots[i].id == 0) { b = &slots[i]; break; }
        }
    }
    if (!b) return 0;
    memset(b, 0, sizeof(*b));
    b->id = (block_id)(b - slots + 1);
    b->kind = kind;
    n_live++;
    return b;
}

void btable_free(block *b) {
    if (!b || b->id == 0) return;
    b->id = 0;
    b->parent = b->first_child = b->next_sibling = 0;
    b->free_next = free_head;
    free_head = b;
    if (n_live) n_live--;
}

void btable_reset(void) {
    free_head = 0;
    n_live = 0;
    memset(slots, 0, sizeof(slots));
}

uint32_t btable_live(void) { return n_live; }
uint32_t btable_capacity(void) { return GUI_MAX_BLOCKS; }
