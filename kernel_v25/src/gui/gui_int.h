#ifndef CATOS_GUI_INT_H
#define CATOS_GUI_INT_H

/*
 * gui_int.h -- GUI 子系统内部跨模块声明（不对外暴露）。
 */
#include "gui/block.h"

/* btable.c */
block    *btable_get(block_id id);
block    *btable_alloc(blk_kind kind);
block    *btable_alloc_at(blk_kind kind, block_id id);
void      btable_free(block *b);
void      btable_reset(void);
uint32_t  btable_live(void);
uint32_t  btable_capacity(void);

/* tree.c */
block    *gui_root_block(void);
void      tree_attach(block *parent, block *child);
void      tree_detach(block *b);
void      tree_free_subtree(block *b);
uint32_t  tree_count_nodes(void);
uint32_t  tree_count_subtree(block *b);
void      tree_reset(void);

/* compose.c */
void      gui_presence_sweep(void);
void      stat_set_layout_nodes(uint32_t n);
void      stat_add_layout_nodes(uint32_t n);
void      stat_note_frame(uint64_t cycles, uint32_t damage_rects,
                          uint32_t damage_area);
void      stat_note_double_commit(void);

/* paint.c */
void      paint_begin_frame(void);
void      paint_node(block *b);

/* layout.c */
void      layout_run(void);

/* input.c（探针注入钩子，复用真实命中/拖拽路径） */
void      gui_inject_press(int x, int y);
void      gui_inject_move(int x, int y);
void      gui_inject_release(void);
block_id  gui_hit_test(int x, int y);
void      gui_focus_raise(block_id id);

/* chrome.c 开始菜单状态 */
void      gui_menu_open(void);
void      gui_menu_close(void);
int       gui_menu_is_open(void);

#endif /* CATOS_GUI_INT_H */
