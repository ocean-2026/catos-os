# 隔离式画布通道 ABI（v2.81，规格 §8 配套）

应用唯一合法像素出口。CE 侧 `gfx_info/gfx_rect/gfx_blit` 与 `display_mode(3)`
一律门禁 `-E_EPERM`；应用只能通过本通道拿到属于自己画布块的可写缓冲。

## 1. 槽位（CE ABI append-only，0–68 不动）

| 槽号 | 签名 | 说明 |
| ---- | ---- | ---- |
| 69 | `int blk_canvas_create(int w, int h, ce_canvas_t *out)` | 建画布；成功 `res >= 0`，失败返回负错误码 |
| 70 | `int blk_canvas_present(const cv_present_t *r)` | 拷脏子矩形进内核 backing 并标脏；只传描述符指针（thunk 仅转发 4 参数） |
| 71 | `int blk_canvas_destroy(int res)` | 立即释放；应用退出/被杀由 `task_cleanup` 强制回收 |

> `gui_surface_attach/dirty` 保持空桩，不允许实现成“应用挂自己的缓冲”。

## 2. 结构（`include/ce_abi.h` / `include/ce.h` 镜像一致）

```c
typedef struct {
    int32_t  res;        /* >=0 内核资源句柄；失败 <0 */
    int32_t  w, h, stride;   /* stride = w*3，RGB24 每行字节数 */
    void    *buf;        /* 应用 arena 匿名 mmap 缓冲（ring3 可写） */
} ce_canvas_t;

typedef struct {
    int32_t  res;
    int32_t  x, y, w, h;     /* 画布内脏子矩形 */
} cv_present_t;
```

`buf` 在应用扩展 arena 匿名 mmap：**不是 VRAM、不是合成器 surface、不是内核
backing 指针**。内核另有私有 backing 缓冲（帧分配页，身份映射），present 时
按子矩形从应用缓冲拷入。

## 3. 错误码（内核与探针共用稳定编号）

| 编号 | 数值 | 含义 |
| ---- | ---- | ---- |
| `E_EPERM` | -1 | 命令式绘制被门禁拒绝（CE 调 gfx_* / `display_mode(3)`） |
| `E_CANVAS_KIND` | -30 | 画布对象/类别不合法（预留，#32 记录校验用） |
| `E_CANVAS_QUOTA` | -31 | 配额超限：单应用块数 / 总面积 / 单帧 present 次数 |
| `E_CANVAS_BOUNDS` | -32 | 尺寸或子矩形越界 |
| `E_PIX_FIELD` | -33 | 提交记录携带内联像素指针 / `pix_w` / `pix_h`（#32 整批拒绝，不 deref） |
| `E_CANVAS_BADH` | -34 | 未知 / 跨属主资源句柄 |

沿用既有负值：-8 ENOSYS、-12 ENOMEM、-14 EFAULT、-16 EBUSY、-22 EINVAL。

## 4. 配额（保守初值，实测后再调）

| 项 | 上限 |
| ---- | ---- |
| 单应用画布块数 | ≤ 2 |
| 单块尺寸 | ≤ 320 × 240 |
| 通道总面积 | ≤ 384 KiB |
| 单次 present 矩形面积 | ≤ 全屏 1/4（800×600 / 4 = 120 000 px） |
| 单帧 present 次数 | ≤ 8（按帧由 `gui_cvchan_frame_tick()` 重置） |

超限回专用错误码，不做静默截断。

## 5. 串口打点键名

| 打点 | 字段 |
| ---- | ---- |
| `GUI:cv_create` | `res=<r> w=<w> h=<h> pid=<tid>` |
| `GUI:cv_present` | `res=<r> x=<x> y=<y> w=<w> h=<h> ms=<ms>` |
| `GUI:cv_destroy` | `res=<r>` |
| `GUI:cv_reject` | `code=<n> pid=<tid>` |
| `GUI:deny` | `<slot> pid=<tid>`；`<slot>` ∈ `display_mode` / `gfx_info` / `gfx_rect` / `gfx_blit` |

自检子命令：`gui cvchan`（建画布 → 写棋盘 backing → present → 销毁一轮；
负路径校验未知句柄回 `E_CANVAS_BADH`）。注意既有 `gui cv` 是内核演示组件探针，
二者不共用槽位。

## 6. 待接线（#32 CE 声明式 ABI）

- `blk_commit` 的 `BLK_CANVAS` 记录必须带 `res`；携带内联像素指针 / `pix_w` /
  `pix_h` 的记录整批拒绝（`E_PIX_FIELD`），且不 deref 用户指针。
- backing 缓冲到块树脏区的合并上屏，随声明式提交通道落地。
- CE 侧 arena 缓冲往返（create→写→present→destroy）真机用例，待 CE ABI 测试
  程序补齐；当前 ring0 自检不经用户 VMA 回拷。
