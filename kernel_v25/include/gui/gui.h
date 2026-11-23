#ifndef CATOS_GUI_GUI_H
#define CATOS_GUI_GUI_H

/*
 * gui.h -- 块化声明式 GUI 对外接口（内核态，规格 §2 / §9 / §11）。
 *
 * 块树、布局、脏标记、合成全部在 kernel_v25 内核态完成；不引入独立
 * Shell 进程、不引入显示服务进程、不做 IPC；v1 只有 A 档同树隔离。
 *
 * 内容协议 v1（规格 §7）：BLK_SURFACE 为 24bpp 不透明位图，1:1 blit、
 * 越界裁剪、不缩放不做 alpha，供给方只有内核内模块。CE 接入槽位
 *   gui_surface_attach(w,h) -> ptr
 *   gui_surface_dirty(rect)
 * 留到 v2，v1 不实现，结构体与注释预留。
 */
#include <stdint.h>
#include "block.h"

/* ---- 生命周期 ---- */
void      gui_begin(void);                 /* 进入图形块桌面（保存文本侧状态由渲染层负责） */
void      gui_end(void);                   /* 干净退回文本控制台 */
void      gui_reset(void);                 /* 清空块表重建（含根），孤儿数 0 */

/* ---- 声明式建块 API ---- */
block_id  b_column (block_id parent, const blk_spec *s);
block_id  b_row    (block_id parent, const blk_spec *s);
block_id  b_stack  (block_id parent, const blk_spec *s);
block_id  b_text   (block_id parent, const blk_spec *s, const char *txt);
block_id  b_rect   (block_id parent, const blk_spec *s);
block_id  b_image  (block_id parent, const blk_spec *s, const uint8_t *pix, int w, int h);
block_id  b_surface(block_id parent, const blk_spec *s);
block_id  b_space  (block_id parent, int w, int h);
void      b_set_props(block_id id, const blk_spec *s);
void      b_remove(block_id id);           /* 两阶段销毁 */

/* ---- 帧与投影 ---- */
void      gui_frame(void);                 /* 一帧：measure→layout→paint→flush */
void      gui_commit_desc(const blk_desc *d, int n);  /* 按 id diff 提交 */
void      gui_rebuild(const blk_desc *d, int n);      /* 全量重建 */

/* ---- 输入泵（shell 主循环调用） ---- */
void      gui_pump_input(void);
int       gui_should_exit(void);

/* ---- 探针计数（规格 §9，真实计数，不许写死） ---- */
uint32_t  stat_frames(void);
uint32_t  stat_layout_nodes(void);
uint32_t  stat_blocks_live(void);
uint32_t  stat_blocks_leaked(void);
uint32_t  stat_double_commit(void);
uint32_t  stat_damage_rects(void);
uint32_t  stat_damage_area(void);
uint64_t  stat_frame_cycles(void);
uint32_t  stat_last_frame_us(void);

/* ---- 自检探针（串口 GUI: 前缀行） ---- */
uint32_t  gui_tree_hash(void);
void      gui_probe_idempotent(void);
void      gui_probe_leak(void);

/* ---- shell 命令入口 ---- */
void      gui_shell_cmd(const char *arg);

#endif /* CATOS_GUI_GUI_H */
