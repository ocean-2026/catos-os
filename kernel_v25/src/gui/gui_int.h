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
void      tree_reset_keep_ce(void);

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
void      layout_popover(block *b);

/* ---- cap2 跨模块接线（签名冻结；实现分散在各自 .c，任何模块只许 append） ---- */
/* textlayout.c：文本度量 / 折行 / 省略 / 字号 / 伪粗 / 等宽数字 */
int       txt_font_h(uint8_t font_size, uint8_t scale);
int       txt_measure_w(const char *s, uint8_t font_size, uint8_t scale,
                        uint8_t tabular);
int       txt_wrapped_h(const char *s, uint8_t font_size, uint8_t scale,
                        uint8_t tabular, int max_w, uint8_t wrap_lines,
                        uint8_t ellipsis, uint8_t line_h_px);
void      txt_draw_block(block *b, uint32_t fg);
/* 旧像素口径原语（paint.c 导出）：8px/字、scale 1|2、超宽 "..." 截断 */
int       gui_text_trim(const char *txt, int max_w, uint8_t scale,
                        char *out, int outsz);

/* icons.c：内置单色图标（16/32 两档） + tint 令牌着色 */
void      icon_draw(block *b, uint32_t fg, uint32_t accent);

/* sb.c：标准滚动条（track + thumb + 滚轮 + 拖 thumb） */
int       sb_visible(block *b);
void      sb_paint(block *b);
int       sb_wheel(block *b, int dy);
/* phase: 0=press 1=drag 2=release；返回 1 表示事件被滚动条吃掉 */
int       sb_mouse(block *b, int x, int y, uint8_t phase);

/* anim.c：固定缓动枚举 + 16ms tick 预生成偏移表 + 可中断 */
void      anim_tick(uint16_t dt_ms);
void      anim_apply(block *b);
void      anim_kick(block *b);
void      anim_reduced(int on);
int       anim_busy(void);

/* popover.c：锚定 -> 收缩 -> 钳制(8px) -> 翻转 -> 超 max_h 内部滚动 */
void      pop_layout(block *b);
int       pop_dismiss_at(int x, int y);
int       pop_esc(void);

/* paint.c：elevation 九宫阴影（预生成 9-slice，右下光源） */
void      sh_paint(block *b, uint8_t elevation);
/* 子树乘法继承后的有效不透明度（返回 1..255） */
uint8_t   blk_alpha_of(block *b);
/* 该像素的有效覆盖率 0..255：会话 alpha(ordered16 棋盘量化) × 圆角覆盖率 ×
 * 命中区域；alpha==未声明且无圆角时返回 255（旧路径零开销）。 */
uint8_t   blk_cov(block *b, int x, int y);
/* 以覆盖率合成一个像素（内部走 blend_px；cov>=255 直接写）。文本/图标共用。 */
void      blk_px_blend(int x, int y, uint32_t color, int cov);

/* cap2 探针族：`gui cap2-<case>`；cap2probe.c 分派到三个包的实现 */
int       cap2_probe_cmd(const char *arg);
int       cap2_probe_render(const char *arg);
int       cap2_probe_text(const char *arg);
int       cap2_probe_interact(const char *arg);
int       cap2_probe_ce(const char *arg);

/* CE 事件回传：由输入/组件路径调用，decl.c 负责入队与结构拼装（cap2）。
 * 旧 decl_post_* 语义不变；新事件种类走这三个入口。 */
void      decl_post_wheel(struct block *b, int dx, int dy);
void      decl_post_input(struct block *b, const char *utf8);
void      decl_post_dismiss(struct block *b);
void      decl_dump_ce_rects(void);

/* blkgen.c: built-in page built from a .blk.json fixture (full-fidelity) */
int       blkgen_build(void);

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
void      ib_activate(block_id id);
void      ib_reset(void);
void      ib_reset_if_owner(int tid);
int       ib_is_active(void);
void      ib_get_state(int *len, int *pos);
int       ib_key(int code, void *ud);
void      ib_render(block_id id);

/* paint.c 边框绘制（settings.c / shadows.c 复用；此前仅 shadows.c 本地 extern） */
void      gui_draw_border(int x, int y, int w, int h, uint32_t color, uint8_t bw);

/* settings.c 外观设置组件 + 风格档位（app / calm） */
void      gui_settings_open(void);
void      gui_settings_close(void);
int       gui_settings_active(void);
void      st_layout_rects(int i, int *x, int *y, int *w, int *h);
int       gui_style_calm(void);
void      gui_style_set(int calm);
void      gui_style_load(void);

#endif /* CATOS_GUI_INT_H */
