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
void      gui_theme_set(enum gui_token t);
void      gui_mark_dirty(block_id id);
void      gui_mark_dirty_union(block *b, int x, int y, int w, int h);
int32_t   layer_max_z(uint16_t layer);
void      gui_raise_to_front(block_id id);
void      stat_set_layout_nodes(uint32_t n);
void      stat_add_layout_nodes(uint32_t n);
void      stat_note_frame(uint64_t cycles, uint32_t damage_rects,
                          uint32_t damage_area);
void      stat_note_double_commit(void);
void      gui_force_next_frame(void);

/* paint.c */
void      paint_begin_frame(void);
void      paint_node(block *b);
void      paint_node_body(block *b);
void      paint_node_clipped(block *b);
void      paint_node_body_clipped(block *b);
void      paint_clip_push_rect(int x, int y, int w, int h);
void      gui_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void      gui_draw_text(int x, int y, const char *txt, uint32_t fg, uint8_t scale);
void      gui_blit24(const uint8_t *src, int x, int y, int w, int h);
void      gui_blit24_stride(const uint8_t *src, int x, int y, int w, int h,
                            int stride);
int       cv_dirty_rect(block *b, int *rx, int *ry, int *rw, int *rh);
void      cv_dirty_clear(block *b);

/* input.c 组件键盘钩子 */
void      gui_set_key_hook(int (*fn)(int code, void *ud), void *ud);

/* layout.c */
void      layout_run(void);

/* input.c（探针注入钩子，复用真实命中/拖拽路径） */
void      gui_inject_press(int x, int y);
void      gui_inject_move(int x, int y);
void      gui_inject_release(void);
void      gui_inject_hover(int x, int y);
void      gui_inject_press_block(block_id id);
void      gui_inject_hover_block(block_id id);
void      gui_inject_key(int code);
block_id  gui_hit_test(int x, int y);
int       gui_interaction_at(int x, int y, block_id *out, uint8_t *rdir);
#define GUI_IT_NONE   0
#define GUI_IT_MOVE   1
#define GUI_IT_RESIZE 2
block    *gui_hover_target(void);
block    *gui_press_target(void);
void      gui_focus_raise(block_id id);
void      gui_modal_inert(block_id overlay_id);

/* v2.80 hotfix: 进桌面时的状态归零（输入态 / chrome 静态句柄） */
void      gui_input_reset(void);
void      gui_chrome_reset(void);

/* chrome.c 开始菜单状态 */
void      gui_menu_open(void);
void      gui_menu_close(void);
int       gui_menu_is_open(void);

/* inputbox.c BLK_INPUT 单行输入框（v1 单实例） */
void      ib_attach(block_id id, void (*on_submit)(block_id id, const char *text));
void      ib_reset(void);
int       ib_is_active(void);
void      ib_get_state(int *len, int *pos);
int       ib_key(int code, void *ud);
void      ib_render(block_id id);

/* settings.c 外观设置组件 + 风格档位（app / calm） */
void      gui_settings_open(void);
void      gui_settings_close(void);
int       gui_settings_active(void);
int       gui_style_calm(void);
void      gui_style_set(int calm);
void      gui_style_load(void);

#endif /* CATOS_GUI_INT_H */
