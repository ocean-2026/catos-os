# CatOS「HTML/JSON → 块树」转换规范

> 基线：`feat/blkhtml`，从 `main`=`89584c4` 开出；worktree `/home/user/work/gui-tools`。
> 冻结契约：`tools/blkhtml/schema.json`（字段唯一契约）、`tools/blkhtml/FROZEN.md`（已核实内核事实）。
> 本文是后续所有写 UI 的人使用 html2blk / blk2bin / preview / validate 时的权威依据。
> 本文每条「内核有什么」的断言都指回只读内核源码 `kernel_v25/` 的 `文件:行号`；源码未覆盖的内容一律标「未核实」，不写推测。

## 0. 读法与口径

### 0.1 本文不做什么

- 不改内核 ABI、不向 `blk_spec` / `ce_blk_rec_t` 增删字段；发现内核缺口时按第 4、5、9 章如实标注为「设计态规则 / 内核缺口」。
- 不替代 `schema.json`：字段名、枚举值、白名单取值以 `schema.json` 为准；本文只解释「为什么这样映射、落到哪条内核路径上」。
- 不描述系统字体、抗锯齿、子像素、DPI 缩放等内核未实现的能力；字模唯一来源是 `kernel_v25/src/font_ascii.inc`。

### 0.2 两类目标产物

| 产物 | 走哪条路径 | 保真度 |
|---|---|---|
| CE 应用（`.ce`，经槽 72 提交） | `blk2bin` → `recs.bin` + `strings.bin` → `blk_commit` | 只保树形 / 种类 / 层 / 声明尺寸 / 文本 / 位图句柄（第 1、2 章） |
| 内核内置页（`blk_spec` 全字段，`--emit-c`） | `blk2bin --emit-c` 产出 C 片段，由内核内置建块 API 建块 | 几何与样式可全保真（第 2 章结论） |

### 0.3 术语

- **块树**：运行时挂在内核根块之下的 `block` 节点树（`kernel_v25/include/gui/block.h:153-203`）。
- **CE 记录**：`ce_blk_rec_t`，用户态可经槽 72 提交的扁平二进制记录（`kernel_v25/include/ce_abi.h:87-98`）。
- **声明超集**：`.blk.json` 里除 `ce` 镜像字段外的几何 / 样式字段；它们只对内置页路径有意义。
- **设计态**：本文档与 `.blk.json` 表达的目标布局；**内核态**：89584c4 源码实际行为。两者不一致处见第 4、5、9 章。

---

## 1. 块树的真实格式：为什么它不是 JSON

### 1.1 运行时格式是「扁平二进制记录数组 + 字符串池」

CE 通道提交的不是 JSON，也不是嵌套树，而是一批等长记录 + 一个字节串池。记录结构 `ce_blk_rec_t` 在线上的镜像定义于 `kernel_v25/include/ce_abi.h:87-98`：

```c
typedef struct {
    int32_t  id;
    int32_t  parent;
    uint8_t  kind;
    uint8_t  pad0;
    uint16_t layer;
    int32_t  req_w, req_h;
    int32_t  text_off;
    int32_t  pix_off;
    int32_t  pix_w, pix_h;
    int32_t  res;
} ce_blk_rec_t;
```

> id 口径区分：`.blk.json` 树内根节点 `id=0`、其余 `>=1`（`schema.json :: node.id`）；经槽 72 提交时，内核自动创建应用根块（`decl.c:226-240`），每条 CE 记录 `id` 都必须 `>=1`，`parent=0` 表示挂到该自动根块之下（`decl.c:172,197-200`）。`blk2bin` 负责这层 id 映射。

### 1.2 九字段（实际十二成员）逐字段表

结构共 12 个成员、**40 字节/条**（无内部空洞）；`schema.json :: ce_blk_rec_t.fields` 与下表一致。

| # | 成员 | 类型 | 偏移 | 语义 | 内核校验依据 |
|---|---|---|---|---|---|
| 1 | `id` | int32 | 0 | 块 id，必须 `>=1`；本批内唯一 | `kernel_v25/src/gui/decl.c:172-173`：`id < 1` 或重复 → `E_DECL_BADID(-37)` |
| 2 | `parent` | int32 | 4 | 父块 id；`0` 表示挂到本应用内核自动创建的根块之下；非 0 必须是本批更早出现过的 id | `kernel_v25/src/gui/decl.c:197-200`：父链断裂 → `E_DECL_BADID(-37)` |
| 3 | `kind` | uint8 | 8 | `blk_kind` 枚举值，见 1.5 | `kernel_v25/src/gui/decl.c:176`：`< BLK_COLUMN(1)` 或 `> BLK_CANVAS(10)` → `E_DECL_BADREC(-39)` |
| 4 | `pad0` | uint8 | 9 | 对齐填充，必须写 0；内核不消费 | 结构布局见 `kernel_v25/include/ce_abi.h:90`；`schema.json :: ce_blk_rec_t.fields.pad0` |
| 5 | `layer` | uint16 | 10 | 层枚举；**经槽 72 只接受 200（`LAYER_WORK`）** | `kernel_v25/src/gui/decl.c:179`：`layer != LAYER_WORK` → `E_DECL_LAYER(-36)` |
| 6 | `req_w` | int32 | 12 | 声明宽；`-1` = auto | 落地见 `kernel_v25/src/gui/decl.c:275-276` |
| 7 | `req_h` | int32 | 16 | 声明高；`-1` = auto | 同上 |
| 8 | `text_off` | int32 | 20 | 文本在串池中的起始偏移；`-1` = 无文本；否则必须 `0 <= text_off < slen` | `kernel_v25/src/gui/decl.c:187-193`：越界 → `E_DECL_BADREC(-39)`；文本长 > 4096 → `E_DECL_QUOTA(-35)` |
| 9 | `pix_off` | int32 | 24 | 位图在串池 / 位图区的偏移；线上恒为 `-1`，CE 通道内联位图一律拒绝 | `kernel_v25/src/gui/decl.c:182-183`：`pix_off != -1` 或 `pix_w/pix_h != 0` → `E_PIX_FIELD(-33)` |
| 10 | `pix_w` | int32 | 28 | 位图宽（CE 记录内必须为 0；位图宽高由资源句柄解析） | 同上；IMAGE 资源解析见 `kernel_v25/src/gui/decl.c:293-300` |
| 11 | `pix_h` | int32 | 32 | 位图高（同上，必须为 0） | 同上 |
| 12 | `res` | int32 | 36 | 槽 74 载入的图像 / 画布资源句柄；`-1` = 无 | `kernel_v25/src/gui/decl.c:213-219`：CANVAS / IMAGE 必须持有归属本应用的句柄；其余 kind 带句柄 → `E_DECL_RES(-38)` |

### 1.3 40 字节布局与对齐

按成员声明顺序与偏移表（1.2 节）：`id@0(4) + parent@4(4) + kind@8(1) + pad0@9(1) + layer@10(2) + req_w@12(4) + req_h@16(4) + text_off@20(4) + pix_off@24(4) + pix_w@28(4) + pix_h@32(4) + res@36(4) = 40` 字节，无内部空洞。
`schema.json :: ce_blk_rec_t.byte_layout` 与此一致。内核拷贝长度按 `n * sizeof(ce_blk_rec_t)` 计算（`kernel_v25/src/gui/decl.c:329-330`，`sizeof = 40`）；`blk2bin` 产出的 `recs.bin` 长度必须为 `40 * n`（校验规则 P1，见第 8 章）。

### 1.4 `text_off` 语义与字符串池

- 串池由提交调用的 `strings` / `slen` 参数带入（`kernel_v25/src/gui/decl.c:317-318`）；内核 `copy_from_user` 拷入内核临时池后再校验（`kernel_v25/src/gui/decl.c:334-339`）。
- `text_off == -1`：该块无文本，`text_off` 不参与池内定位。
- `text_off >= 0`：从 `pool + text_off` 起读到 NUL 为止；长度计入单块文本配额 4096 B（`kernel_v25/src/gui/decl.c:186-193`）。
- 内核不保存用户态指针：文本由 `decl_copy_text` 复制进本应用 arena（`kernel_v25/src/gui/decl.c:242-251`）；ABI 注释明确「内核绝不保存任何用户态指针」（`kernel_v25/include/gui/decl.h:7-10`）。
- 单池总配额 32768 B（见 1.7）。
- 串池按 NUL 分隔多段文本；`text_off` 指向段首，段长由读到的第一个 NUL 决定（`decl.c:189-191,285-287`）。池尾多余 NUL 不影响校验，但 `slen` 不得超过 32768 B（`decl.c:325`）。

### 1.5 `kind` 枚举与 CE 通道可提交范围

枚举定义于 `kernel_v25/include/gui/block.h:19-35`，与 `schema.json :: kind_enum` 逐值一致：

| 值 | 名称 | CE 槽 72 可提交？ |
|---|---|---|
| 0 | `BLK_ROOT` | 否（应用根块由内核 `decl_ensure_root` 自动创建，`kernel_v25/src/gui/decl.c:226-240`） |
| 1 | `BLK_COLUMN` | 是 |
| 2 | `BLK_ROW` | 是 |
| 3 | `BLK_STACK` | 是 |
| 4 | `BLK_TEXT` | 是 |
| 5 | `BLK_RECT` | 是 |
| 6 | `BLK_IMAGE` | 是（需归属本应用的图像句柄） |
| 7 | `BLK_SURFACE` | 是 |
| 8 | `BLK_SPACER` | 是 |
| 9 | `BLK_DIVIDER` | 是 |
| 10 | `BLK_CANVAS` | 是（需归属本应用的画布句柄） |
| 11 | `BLK_FREE` | **否**：被 `decl.c:176` 的上限 `BLK_CANVAS` 排除 |
| 12 | `BLK_BUTTON` | **否**：同上 |
| 13 | `BLK_INPUT` | **否**：同上 |
| 14 | `BLK_TERM` | **否**：同上 |

> 注意：`BLK_FREE / BUTTON / INPUT / TERM` 在内核内置页路径中可以通过 `desc.c:94-150` 的建块 API 创建；只是不能经 CE 记录提交。

### 1.6 槽位分工（72–76）

槽位表见 `kernel_v25/include/ce_abi.h:320-325`，与 `schema.json :: single_source_of_truth.ce_slots` 一致：

| 槽 | 签名 | 职责 |
|---|---|---|
| 72 | `blk_commit(const void *recs, int n, const char *strings, int slen)` | 整批提交块记录 + 串池；校验失败整批拒绝 |
| 73 | `blk_event_poll(ce_blk_event_t *ev, int timeout_ms)` | 轮询一个本应用事件（点击 / 按键 / 焦点 / 尺寸 / 关闭） |
| 74 | `blk_image_load(const char *path)` | 载入位图资源，返回句柄；失败返回 `<0` |
| 75 | `blk_image_free(int res)` | 释放句柄 |
| 76 | `blk_text_width(const char *s)` | 文本度量（ASCII 8px / 双字节 16px，见 7.2） |

事件类型枚举见 `kernel_v25/include/gui/decl.h:29-36`（CLICK / KEY / FOCUS_IN / FOCUS_OUT / RESIZE / CLOSE）；事件结构 `ce_blk_event_t` 见 `kernel_v25/include/ce_abi.h:100-105`。

`ce_blk_event_t` 字段（`ce_abi.h:100-105`）：

| 成员 | 类型 | 语义 |
|---|---|---|
| `type` | int16 | 事件类型，取 `decl.h:30-35` 枚举 |
| `code` | int16 | 按键码 / 事件副码 |
| `id` | int32 | 命中块的本地 id（提交批内 id） |
| `x, y` | int32 | 块内局部坐标；ABI 不给绝对屏幕坐标、不给鼠标原始流（`decl.h:28` 注释） |
| `w, h` | int32 | 事件附带尺寸（RESIZE 等） |

事件类型表（`kernel_v25/include/gui/decl.h:29-36`）：

| 值 | 名称 | 触发路径 |
|---|---|---|
| 0 | `DECL_EV_CLICK` | 命中块点击，`decl.c` 经 `decl_post_click` 入队 |
| 1 | `DECL_EV_KEY` | 键盘输入，`decl_post_key` |
| 2 | `DECL_EV_FOCUS_IN` | 焦点进入，`decl_post_focus(..., 1)` |
| 3 | `DECL_EV_FOCUS_OUT` | 焦点离开，`decl_post_focus(..., 0)` |
| 4 | `DECL_EV_RESIZE` | 尺寸变化，`decl_post_resize` |
| 5 | `DECL_EV_CLOSE` | 关闭请求，`decl_post_close` |

### 1.7 提交语义：整批校验、整批拒绝、配额

`decl_commit` 流程见 `kernel_v25/src/gui/decl.c:317-361`：

1. 配额预检：`n < 0 || n > 256`、`slen < 0 || slen > 32768` → `E_DECL_QUOTA(-35)` 并拒绝（`decl.c:324-325`）。
2. `copy_from_user` 拷入记录区与串池；拷贝失败 → `E_DECL_BADREC(-39)`（`decl.c:329-339`）。
3. `validate()` 全批静态校验（`decl.c:163-222`）；任一记录非法 → `decl_reject(rc)`，**整批拒绝，不触碰旧子树**（`decl.c:341-342`）。
4. 校验通过后才进入 `decl_apply` 落地：先回收旧子树（保留根块本身），再逐条建块挂树（`decl.c:253-315`）。

单应用配额（`kernel_v25/include/gui/decl.h:22-26`，与 `schema.json :: quotas` 一致）：

| 配额 | 值 | 拒绝码 |
|---|---|---|
| 最大块数 `DECL_MAX_BLOCKS` | 256 | `E_DECL_QUOTA(-35)` |
| 最大深度 `DECL_MAX_DEPTH` | 16 | `E_DECL_QUOTA(-35)`（`decl.c:203-209`） |
| 单块最大文本 `DECL_MAX_TEXT` | 4096 B | `E_DECL_QUOTA(-35)`（`decl.c:192`） |
| 串池最大 `DECL_MAX_POOL` | 32768 B | `E_DECL_QUOTA(-35)`（`decl.c:325`） |

拒绝码表见 `kernel_v25/include/gui/decl.h:16-20`：`-35` 配额、`-36` 越层、`-37` id、`-38` 资源、`-39` 记录结构。

### 1.8 最小 C 提交范例（dektest.c 风格）

参考现成探针 `kernel_v25/extapp/dektest.c:34-64`：3 条记录（根 COLUMN + TEXT + RECT），串池 `"Hi"`，提交后轮询事件、度量文本、载入并释放位图。

```c
#include "ce_app.h"

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *api = ce_api();

    ce_blk_rec_t r[3];
    char pool[4];
    pool[0] = 'H'; pool[1] = 'i'; pool[2] = 0; pool[3] = 0;

    /* r0: root column（parent=0 挂到应用自动根块） */
    r[0].id = 1; r[0].parent = 0;
    r[0].kind = 1; r[0].layer = 200;              /* BLK_COLUMN / LAYER_WORK */
    r[0].req_w = -1; r[0].req_h = -1;
    r[0].text_off = -1; r[0].pix_off = -1;
    r[0].pix_w = 0; r[0].pix_h = 0; r[0].res = -1;

    /* r1: text child，text_off=0 指向串池 "Hi" */
    r[1].id = 2; r[1].parent = 1;
    r[1].kind = 4; r[1].layer = 200;              /* BLK_TEXT */
    r[1].req_w = -1; r[1].req_h = -1;
    r[1].text_off = 0; r[1].pix_off = -1;
    r[1].pix_w = 0; r[1].pix_h = 0; r[1].res = -1;

    /* r2: rect child */
    r[2].id = 3; r[2].parent = 1;
    r[2].kind = 5; r[2].layer = 200;             /* BLK_RECT */
    r[2].req_w = -1; r[2].req_h = -1;
    r[2].text_off = -1; r[2].pix_off = -1;
    r[2].pix_w = 0; r[2].pix_h = 0; r[2].res = -1;

    int rc = api->blk_commit(r, 3, pool, 2);

    ce_blk_event_t ev;
    int polled = api->blk_event_poll(&ev, 0);
    int tw = api->blk_text_width("Hi");
    int img = api->blk_image_load("/samples/desktop.bmp");
    if (img >= 0) api->blk_image_free(img);

    return (rc == 0 && tw == 16) ? 0 : 1;
}
```

要点：所有记录 `layer=200`；无图块 `pix_off/pix_w/pix_h/res` 按 1.2 表置位；文本偏移在提交前由 `blk2bin` 重新计算（`schema.json :: node.ce`：blk2bin 可写回 `text_off`）。

---

## 2. 两层模型：CE 可表达层 vs 声明超集层

### 2.1 结论先行

`ce_blk_rec_t` 里**没有几何、没有 padding/gap/align、没有 anchor/pct、没有 overflow、没有颜色、没有 radius/shadow**。这条结论直接由 1.2 的成员表得出：记录成员止于 1.2 表内十二项，对照 `blk_spec` 全字段（`kernel_v25/include/gui/block.h:103-140`）可逐项核对。

因此 `.blk.json` 必须是两层：

| 层 | 字段 | 去向 |
|---|---|---|
| `ce` 镜像层 | `id / parent / kind / layer(仅 work) / req_w / req_h / text / pix / res` | `blk2bin` → `recs.bin` + `strings.bin` → 槽 72（第 1 章） |
| 声明超集层 | `rect / padding / gap / layout / anchor / px / pct / min_max / margin / overflow / scroll / drag / resize / bleed / z / fill / fg / accent / stroke / radius / radius_mode / shadow / font / visible / focusable / inert` | 只进 `blk2bin --emit-c` 产出的内核内置页（`blk_spec` 全字段），**不能**经槽 72 |

### 2.2 CE 可表达字段

`schema.json :: ce_projection.representable` 列出：`id`、`parent`、`kind`、`layer(仅 work)`、`req_w`、`req_h`、`text`、`pix/res`。
对应到记录成员即 1.2 表的全部成员；`pix` 在 CE 通道以资源句柄 `res` 表达，内联位图被 `decl.c:182-183` 拒绝。

### 2.3 过不了槽 72 的声明字段

`schema.json :: ce_projection.not_representable` 逐条列出（下表按 schema 原文分组，共 16 组）：

| 组 | 字段 | 内核内置页里的对应位置（`block.h:103-140`） | 消费路径依据 |
|---|---|---|---|
| 1 | `rect` 绝对几何 | `block.x/y/w/h`（`block.h:159`），布局结果 | `layout.c:261-290` |
| 2 | `padding` | `pad_t / pad_r / pad_b / pad_l` | `desc.c:28-29`；`layout.c:263-266` |
| 3 | `gap` | `gap` | `desc.c:30`；`layout.c:79,132,162` |
| 4 | `align_main` | `align_main` | `desc.c:31`；`layout.c:137-145,167-175` |
| 5 | `align_cross` | `align_cross` | `desc.c:32`；`layout.c:114-122` |
| 6 | `anchor / px / pct / min / max / margin` | `anchor / px_x,y / pct_x,y,w,h / min_w,h,max_w,h / m_t,r,b,l` | `desc.c:42-48`；`layout.c:207-248` |
| 7 | `overflow / scroll` | `overflow / scroll_x,scroll_y` | `desc.c:41,55`；`paint.c:414-417,427` |
| 8 | `drag / drag_bounds` | `drag_mode / drag_bounds` | `desc.c:49`；`input.c:119,203` |
| 9 | `resize` | `resize_dir / edge / min_w / min_h / max_w / max_h / step` | `desc.c:50-54`；`input.c:102-114` |
| 10 | `bleed` | `block.bleed`（注：`blk_spec` 无此成员，见 2.5） | `layout.c:105`；`paint.c:228` |
| 11 | `z` | `z` | `desc.c:35`；`compose.c:88-96` |
| 12 | `fill / fg / accent / stroke` | `fg / bg / accent / role_fg / role_bg / role_accent / border_w` | `desc.c:57-68`；`paint.c:153-172,346-347` |
| 13 | `radius / radius_mode` | 内核仅 `st.corner_radius / corner_mode`（`tokens.h:30-31`），无逐角半径字段 | 第 5 章；`paint.c:226-240` |
| 14 | `shadow` | 内核无阴影档位字段；由 `paint_halo` 按 layer 固定产出 | `paint.c:296-312` |
| 15 | `font.scale` | `st.scale` | `desc.c:67`；`paint.c:99-117` |
| 16 | `visible / focusable / inert` | `visible / focusable / inert` | `desc.c:36-38`；`input.c:65,137,189-190` |

### 2.4 全保真只能走内核内置页 `blk_spec`

`schema.json :: ce_projection.consequence` 的结论：槽 72 只能还原「树形 + 种类 + 层 + 声明尺寸 + 文本 + 位图」；几何与样式全部丢失。
需要几何 / 样式全保真的页面，路径是 `blk2bin --emit-c` 产出内置页 C 片段，经由 `desc.c:94-150` 的建块 API（`b_column / b_row / b_text / b_button / b_input …`）把 `blk_spec` 全字段建进内核块树；不是 CE 应用。

内置页追加方式见 `FROZEN.md §5`：在内核 `gui_shell_cmd()` 追加分支，append-only，不动既有枚举。

### 2.5 两个字段口径提醒

- **`bleed` 不在 `blk_spec` 里**：`block.h:171` 的 `block.bleed` 是运行时块字段，`blk_spec`（`block.h:103-140`）无对应成员；内置页建块后需按 `chrome.c:445,453` 的模式直接置位。CE 通道无法表达。
- **`align_self` 无布局消费者**：`blk_spec` / `block` 均有该成员（`block.h:107,162`），`desc.c:33` 会拷贝，但 `layout.c` 全文未读取 `align_self`；生成器不要为它声明期望行为。

---

## 3. HTML/CSS → 块树映射表

### 3.1 标签 → `kind`

依据 `schema.json :: html_subset.tags`；`kind` 值对照 `block.h:19-35`。

| HTML 标签 | 块 `kind` | 枚举值 | 备注 / 依据 |
|---|---|---|---|
| `<div>` | 容器：`ROW` / `COLUMN` / `FREE` | 2 / 1 / 11 | `display:flex` + `flex-direction` 定向；`position:absolute` 的父容器取 `FREE`；无 flex 的 `block` 落 `COLUMN` |
| `<span>` | `TEXT` | 4 | 行内即文本块；文本单行，不折行（7.2） |
| `<button>` | `BUTTON` | 12 | 内置页路径可用；CE 通道不可提交（1.5） |
| `<input type="text">` | `INPUT` | 13 | 同上；建块时 `desc.c:145` 自动置 `focusable=1` |
| `<img>` | `IMAGE` | 6 | 位图经槽 74 载入取句柄；`pix.{path,w,h,res}` |
| `<hr>` | `DIVIDER` | 9 | 绘制为 1px `line` 角色色横线，`paint.c:370-372` |
| `<br>` / 空白占位 | `SPACER` | 8 | 尺寸取 `req_w/req_h`，`layout.c:29,57` |
| （无直接 HTML 标签） | `STACK` | 3 | 层叠容器：子块按先序叠放、居中对齐；HTML 子集不直接产出，仅内置页 / `data-blk` 覆盖路径使用。求解见 `layout.c:186-202` |

### 3.2 容器流与对齐 → `layout` / `align_*`

| CSS | `.blk.json` 字段 | 枚举 / 取值 | 内核求解依据 |
|---|---|---|---|
| `display:flex; flex-direction:row` | `layout.flow = "row"`，`kind=2` (`BLK_ROW`) | — | `layout.c:156-184` |
| `display:flex; flex-direction:column` | `layout.flow = "column"`，`kind=1` (`BLK_COLUMN`) | — | `layout.c:126-154` |
| `display:block`（无 flex） | `layout.flow = "column"`，`kind=1` | — | `schema.json :: html_subset.tags.div` |
| `display:none` | 不生成节点；记入 diagnostics | — | `schema.json :: css_properties.supported.display` |
| `justify-content:flex-start` | `layout.align_main = "start"` | `ALN_START=0`（`block.h:78`） | `layout.c:139,169` |
| `justify-content:center` | `layout.align_main = "center"` | `ALN_CENTER=1`（`block.h:79`） | `layout.c:137,167` |
| `justify-content:flex-end` | `layout.align_main = "end"` | `ALN_END=2`（`block.h:80`） | `layout.c:138,168` |
| `justify-content:space-between` | `layout.align_main = "space_between"` | `ALN_SPACE_BETWEEN=3`（`block.h:81`） | `layout.c:142-145,172-175` |
| `align-items:flex-start` | `layout.align_cross = "start"` | `XALN_START=0`（`block.h:86`） | `layout.c:115,119` |
| `align-items:center` | `layout.align_cross = "center"` | `XALN_CENTER=1`（`block.h:87`） | `layout.c:116,120` |
| `align-items:flex-end` | `layout.align_cross = "end"` | `XALN_END=2`（`block.h:88`） | `layout.c:117,121` |
| `align-items:stretch` | `layout.align_cross = "stretch"` | `XALN_STRETCH=3`（`block.h:89`） | `layout.c:115,119` |
| `gap: <px>` | `layout.gap` | 取值属 `allowed_spacing.gap`：`0/4/8/12/16/24` | `layout.c:79,132,162`；`schema.json :: allowed_spacing` |

### 3.3 尺寸、外边距、内边距

| CSS | `.blk.json` 字段 | 内核字段 | 依据 |
|---|---|---|---|
| `padding: <px>` | `padding`（int 或 `{t,r,b,l}`） | `pad_t/pad_r/pad_b/pad_l` | `desc.c:28-29`；`layout.c:263-266`；取值属 `0/2/4/8/12/16/24` |
| `padding-top/right/bottom/left` | `padding.{t,r,b,l}` | 同上 | 同上 |
| `margin: <px>` | `margin`（int 或 `{t,r,b,l}`） | `m_t/m_r/m_b/m_l` | `layout.c:208-210`；取值属 `0/2/4/8/12/16/24` |
| `width: <px>` | `req.w` | `req_w` | `decl.c:275`；`layout.c:91,95,216` |
| `height: <px>` | `req.h` | `req_h` | 同上 |
| `width:auto` / `height:auto` | `req.w/h = -1` | `-1 = auto` | `block.h:104`；`schema.json :: node.req` |
| `width:100%` | `pct.w = 1000` | `pct_w`（1/1000 父框） | `layout.c:215`；`block.h:128` |
| `height:100%` | `pct.h = 1000` | `pct_h` | `layout.c:217`；`block.h:128` |
| `min-width / min-height` | `min_max.min_w / min_h` | `min_w / min_h`（0 = 不设） | `layout.c:221-222`；`block.h:130` |
| `max-width / max-height` | `min_max.max_w / max_h` | `max_w / max_h`（0 = 不设） | `layout.c:223-224`；`block.h:130` |

> 百分比换算：`pct = round(子尺寸 / 父尺寸 * 1000)`；内核求解是整数运算 `aw * pct_w / 1000`（`layout.c:215,217,234,240`），千分位取整产生的误差见 7.3 #3。

### 3.3.1 自然尺寸（`auto` 回落口径）

`req_w/req_h = -1` 时，叶子块的自然尺寸由 `layout.c:20-60` 算出；生成器在 `.blk.json` 里写 `req=-1` 即按此口径，不要自行估算叶子宽高。

| 叶子 kind | 自然宽 | 自然高 | 依据 |
|---|---|---|---|
| `TEXT` / `BUTTON` | `8 * scale * 文本长度` | `16 * scale`；calm 档非文本控件抬高到 24 | `layout.c:21-23,44-51` |
| `INPUT` | 80 | `16 * scale`；calm 档抬高到 24 | `layout.c:25,45-51` |
| `IMAGE` / `SURFACE` / `CANVAS` | `pix_w` | `pix_h` | `layout.c:26-28,54-56` |
| `SPACER` | `req_w` | `req_h` | `layout.c:29,57` |
| `DIVIDER` | 0 | 1 | `layout.c:30,58` |

容器自然尺寸：COLUMN 宽 = padding + 子块最大宽、高 = padding + 子块高之和 + gap；ROW 宽 = padding + 子块宽之和 + gap、高 = padding + 子块最大高；STACK 取最大宽 / 最大高（`layout.c:71-90`）。

### 3.4 定位：`position:absolute` → `FREE` + `anchor` + `px` / `pct`

| CSS | `.blk.json` 字段 | 内核字段 | 依据 |
|---|---|---|---|
| `position:absolute` | 父容器 `kind=11` (`BLK_FREE`)；子块 `layout.flow="free"` | `BLK_FREE` | `block.h:31`；`layout.c:250-259` |
| `position:relative` / `static` | 留在流式容器，不生成 `anchor` | — | `schema.json :: css_properties.supported.position` |
| `left: <px>` | `px.x`；`anchor` 含 `L`（默认左锚） | `px_x` / `ANC_L=0x1` | `layout.c:233`；`block.h:46` |
| `top: <px>` | `px.y`；`anchor` 含 `T`（默认上锚） | `px_y` / `ANC_T=0x4` | `layout.c:239`；`block.h:48` |
| `right: <px>` | `anchor` 含 `R`；`px.x` 为右锚回退偏移 | `ANC_R=0x2` / `px_x` | `layout.c:230`；`block.h:47` |
| `bottom: <px>` | `anchor` 含 `B`；`px.y` 为下锚回退偏移 | `ANC_B=0x8` / `px_y` | `layout.c:236`；`block.h:49` |
| 水平居中 | `anchor` 含 `HCENTER` | `ANC_HCENTER=0x10` | `layout.c:231`；`block.h:50` |
| 垂直居中 | `anchor` 含 `VCENTER` | `ANC_VCENTER=0x20` | `layout.c:237`；`block.h:51` |
| `left: <%>` | `pct.x`（1/1000） | `pct_x` | `layout.c:234`；`block.h:127` |
| `top: <%>` | `pct.y` | `pct_y` | `layout.c:240`；`block.h:127` |

锚点 / 百分比 / 外边距 / min-max 的求解顺序固定：父框扣外边距 → 锚点 / 百分比落基准点 → min/max 夹尺寸（不回写坐标）→ 落矩形；依据 `layout.c:207-248` 与规格 §2.2（`docs/块化GUI-v2-规格.md:50-69`）。

### 3.5 溢出与滚动

| CSS | `.blk.json` 字段 | 内核枚举 | 依据 |
|---|---|---|---|
| `overflow:hidden` | `overflow = "clip"` | `OVF_CLIP=0`（默认） | `block.h:39`；`paint.c:414-415` |
| `overflow:visible` | `overflow = "visible"` | `OVF_VISIBLE=1` | `block.h:40`；`paint.c:420-422` |
| `overflow:auto` / `overflow:scroll` | `overflow = "scroll"` | `OVF_SCROLL=2` | `block.h:41`；`paint.c:416-417,427` |
| 滚动偏移 | `scroll.{x,y}`（仅 `overflow=scroll` 有意义） | `scroll_x / scroll_y` | `block.h:139`；`paint.c:417`；规则 O1（第 8 章） |

> `OVF_SCROLL` 只做视口平移，不改子块几何、不重排子树（`block.h:41` 注释；`paint.c:19-22,416-417`）；内核不提供滚动条控件（`docs/块化GUI-v2-规格.md:136`）。

### 3.6 描边、圆角、阴影

| CSS | `.blk.json` 字段 | 内核字段 / 行为 | 依据 |
|---|---|---|---|
| `border: 1px solid <color>` | `stroke.w = 1`，`stroke.sides=[L,R,T,B]`，`stroke.color` 角色名 | `st.border_w`（0/1/2）；描边色取活动令牌 `border` 角色 | `block.h:113`；`paint.c:121-136,346-347` |
| `border-width: 2` | `stroke.w = 2` | `border_w=2` 画内缩 1px 同色第二层 | `paint.c:129-135` |
| `border-width: 0` | `stroke.w = 0` | 不画描边 | `paint.c:271,346` |
| `border-color: <角色>` | `stroke.color` | 角色解析走 `resolve_color` 三级回落 | `paint.c:153-172` |
| `border-radius: <px>` | `radius.{tl,tr,br,bl}`，档值 `0/8/12/16/24` | 内核仅 calm 尺寸阶梯，无逐角半径（第 5 章） | `paint.c:198,234-259` |
| `box-shadow: contact` | `shadow.mode = "contact"` | 内核按 layer 定单层 / 双层，不由该字段选 | `paint.c:297-301,305-306` |
| `box-shadow: dual` | `shadow.mode = "dual"` | 同上；仅 OVERLAY / TOAST 出双层 | `paint.c:299,307-311` |
| `box-shadow: none` | `shadow.mode = "none"` | 不产 halo；`calm_shape_eligible` 之外亦不画 | `paint.c:297` |

> 逐边 `stroke.sides`、逐角 `radius`、`shadow.sides` 是设计态字段：`blk_spec` 无对应逐边 / 逐角成员，内核绘制路径不读它们（第 4、5、9 章）。

### 3.7 颜色、字体、层级

| CSS | `.blk.json` 字段 | 内核字段 | 依据 |
|---|---|---|---|
| `background` / `background-color: <角色>` | `fill` | `st.bg` / `role_bg` | `desc.c:58,62`；`paint.c:161-163` |
| `color: <角色>` | `fg` | `st.fg` / `role_fg` | `desc.c:57,61`；`paint.c:157-159` |
| `accent` 色 | `accent` | `st.accent` / `role_accent` | `desc.c:59,63`；`paint.c:165-167` |
| `font-size: 12px` | `font.scale = 1` | `st.scale=1` | `schema.json :: css_properties.supported.font-size`；`paint.c:99-117` |
| `font-size: 16px` | `font.scale = 2` | `st.scale=2`，字形与步距各乘 2 | `paint.c:100,107-111` |
| 其它 `font-size` | — | 报 `UNSUPPORTED_FONT_SIZE` | `schema.json :: css_properties.supported.font-size` |
| `z-index: <int>` | `z` | `z`；绘制按 layer 升序 → z 升序 → 树先序 | `compose.c:74,88-96`；`desc.c:35` |
| `line-height: 16`（scale1）/ `32`（scale2） | 允许；其余报不支持 | 文本行高固定 16*scale | `schema.json :: css_properties.supported.line-height`；`layout.c:47` |

### 3.8 令牌角色表（颜色映射唯一依据）

角色枚举见 `kernel_v25/include/gui/tokens.h:42-51`；三套令牌取值见 `kernel_v25/src/gui/tokens.c:9-26`，与 `schema.json :: token_palette` 逐字一致。`.blk.json` 的 `fill / fg / accent / stroke.color` 只允许取下列角色名或 `#RRGGBB` 显式色。

| 角色名 | 枚举 | dark | light | hc | 消费路径 |
|---|---|---|---|---|---|
| `fg` | `ROLE_FG=1` | `E8E8EC` | `181820` | `FFFFFF` | `paint.c:157-159` |
| `bg` | `ROLE_BG=2` | `1E1E26` | `ECECF1` | `000000` | `paint.c:161-163` |
| `panel` | `ROLE_PANEL=3` | `2A2A34` | `DDDDE6` | `101010` | `paint.c:144` |
| `bar` | `ROLE_BAR=4` | `3A3A48` | `C8C8D6` | `202020` | `paint.c:145` |
| `accent` | `ROLE_ACCENT=5` | `4C8DFF` | `2563D8` | `FFFF00` | `paint.c:165-167` |
| `border` | `ROLE_BORDER=6` | `555A66` | `8A8F9E` | `FFFF00` | `paint.c:147` |
| `line` | `ROLE_LINE=7` | `555A66` | `8A8F9E` | `FFFF00` | `paint.c:148`；分隔线 `paint.c:371` |

显式 `#RRGGBB` 走 `st.fg/bg/accent` 固化色；角色生效时对应固化色清零（`desc.c:57-66`）。主题切换时角色色跟随活动令牌重解析，固化色不跟随。

### 3.9 `data-blk` 覆盖属性

依据 `schema.json :: html_subset.attributes`，HTML 元素上的 `data-blk-*` 属性用于在受限规则之外显式覆盖块字段：

| 属性 | 覆盖目标 | 对应 `.blk.json` 字段 |
|---|---|---|
| `data-blk-layer` | 层 | `layer`（CE 投影仍只允许 `work`，规则 L1） |
| `data-blk-anchor` | 锚点位 | `anchor`（仅 FREE 子块） |
| `data-blk-overflow` | 溢出三态 | `overflow` |
| `data-blk-drag` | 拖拽模式 | `drag`（`none/title/full`） |
| `data-blk-role` | 着色角色 | `fill / fg / accent` 之一 |

`id / class`：`class` 只能命中 HTML 内联 `<style>` 里定义的受限规则；未知 class 报 `UNSUPPORTED_CLASS`。`style` 属性只接受 3.2–3.7 白名单属性。

### 3.10 worked example A：流式行（row + gap + 对齐 + 描边底色）

HTML：

```html
<div style="display:flex; flex-direction:row; gap:8px;
            justify-content:center; padding:8px; background:panel">
  <span>标题</span>
  <button>确定</button>
</div>
```

产物 `.blk.json` 片段（合法 JSON，可过 `json.tool`）：

```json
{
  "id": 1,
  "kind": "row",
  "layer": "work",
  "rect": { "x": 0, "y": 0, "w": 320, "h": 32 },
  "padding": { "t": 8, "r": 8, "b": 8, "l": 8 },
  "layout": { "flow": "row", "gap": 8, "align_main": "center", "align_cross": "start" },
  "fill": "panel",
  "children": [
    {
      "id": 2,
      "kind": "text",
      "parent": 1,
      "rect": { "x": 104, "y": 8, "w": 32, "h": 16 },
      "text": "标题",
      "fg": "fg"
    },
    {
      "id": 3,
      "kind": "button",
      "parent": 1,
      "rect": { "x": 144, "y": 4, "w": 72, "h": 24 },
      "text": "确定",
      "fill": "bar",
      "stroke": { "w": 1, "sides": ["L", "R", "T", "B"], "color": "border" }
    }
  ]
}
```

落点：`kind=2` → `BLK_ROW`（`block.h:22`）；`gap=8` → `b->gap`（`desc.c:30`）；`align_main=center` → `ALN_CENTER`（`layout.c:167`）；`padding` → `pad_*`（`desc.c:28-29`）；`fill=panel` → `ROLE_PANEL`（`tokens.h:46`；`paint.c:144`）。

### 3.11 worked example B：`position:absolute` 子块（FREE + anchor + pct + px）

HTML（水平居中、距顶 16px 的卡片；本模型居中走锚点，不走 `left:50%`）：

```html
<div style="position:absolute; top:16px; width:200px; height:48px">
  <span>居中卡片</span>
</div>
```

产物 `.blk.json` 片段：

```json
{
  "id": 10,
  "kind": "free",
  "parent": 1,
  "rect": { "x": 0, "y": 0, "w": 320, "h": 240 },
  "children": [
    {
      "id": 11,
      "kind": "rect",
      "parent": 10,
      "rect": { "x": 60, "y": 16, "w": 200, "h": 48 },
      "anchor": ["HCENTER"],
      "pct": { "x": 0, "y": 0, "w": 0, "h": 0 },
      "px": { "x": 0, "y": 16 },
      "fill": "panel",
      "children": [
        {
          "id": 12,
          "kind": "text",
          "parent": 11,
          "rect": { "x": 72, "y": 32, "w": 64, "h": 16 },
          "text": "居中卡片"
        }
      ]
    }
  ]
}
```

落点：父 `kind=11` (`BLK_FREE`)，`layout.c:250-259` 遍历 `layout_free_child`；`ANC_HCENTER` 命中 `layout.c:231`（居中分支不读 `pct_x`，故 `pct.x=0`）；`px_y=16` 命中 `layout.c:239`；`pct_w=0` 表示未声明宽度，宽取 `req_w` / 自然尺寸（`layout.c:216`）。`left:50%` 这类百分比左定位对应左锚 + `pct_x`，不走居中锚点（`layout.c:233-234`）。

### 3.12 worked example C：overflow=scroll + 描边 + 阴影档位 + 逐角半径（设计态）

HTML：

```html
<div style="overflow:scroll; width:240px; height:120px;
            border:1px solid border; border-radius:12px;
            box-shadow:dual">
  <span>长文本内容……</span>
</div>
```

产物 `.blk.json` 片段：

```json
{
  "id": 20,
  "kind": "rect",
  "parent": 1,
  "rect": { "x": 16, "y": 16, "w": 240, "h": 120 },
  "overflow": "scroll",
  "scroll": { "x": 0, "y": 0 },
  "stroke": { "w": 1, "sides": ["L", "R", "T", "B"], "color": "border" },
  "radius": { "tl": 12, "tr": 12, "br": 12, "bl": 12 },
  "radius_mode": "size_ladder",
  "shadow": { "mode": "dual", "sides": ["L", "R", "T", "B"] },
  "children": [
    {
      "id": 21,
      "kind": "text",
      "parent": 20,
      "rect": { "x": 24, "y": 24, "w": 208, "h": 16 },
      "text": "长文本内容……",
      "text_overflow": "ellipsis"
    }
  ]
}
```

落点与缺口（必须连同缺口一起读）：
- `overflow=scroll` → `OVF_SCROLL`，平移栈由 `paint.c:416-417,427` 消费；`scroll.x/y` 规则 O1 见第 8 章。
- `stroke.w=1` → `border_w=1`，`paint.c:125-128` 画四边；`sides` 字段内核不逐边读取（4.4）。
- `radius=12`：设计态逐角值；内核 calm 档按 `min(w,h)` 重新分档（`paint.c:234-240`），120/240 的块落 `s=120 >= 64` → 档 16，不会保留 12（第 5 章）。
- `shadow.mode=dual`：内核只在 `LAYER_OVERLAY / LAYER_TOAST` 出双层（`paint.c:299,307-311`）；WORK 层固定单层接触层（`paint.c:300,305-306`）。

---

## 4. 贴边方角与分隔线规则

### 4.1 定义（设计层规则）

依据 `schema.json :: flush_rule.definition`：同一父块内，把子块 `rect` 与父块内容矩形（父 `rect` 去掉 padding 后）按 8pt 量化后逐边比较；某条边坐标完全相等即判该侧贴边（左右比 `x / x+w`，上下比 `y / y+h`）。

- 比较对象：父内容框 `(cax, cay, caw, cah)`，由 `layout.c:265-266` 按父 padding 算出；`bleed` 子块对齐父整框（`layout.c:105`），不参与内容框贴边判定。
- 量化口径：坐标 / 尺寸必须是 8 的倍数（规则 G1，第 8 章；`schema.json :: allowed_spacing.grid=8`）。

### 4.2 四类设计态效果

依据 `schema.json :: flush_rule.effects`：

| # | 效果 | 设计态落点 |
|---|---|---|
| 1 | 该侧两个角的 `radius` 归零 | `radius` 对应角置 0（规则 R1） |
| 2 | 该侧 `stroke` 省略（不画父与子的双线） | `stroke.sides` 移除该侧（规则 S1） |
| 3 | 该侧 `shadow` 抑制 | `shadow.sides` 移除该侧（规则 S2） |
| 4 | 父子共享边只保留 1px 分隔线，颜色 `role=line` | 共享边由分隔线承担，父子不在该侧各画描边（规则 D1） |

### 4.3 内核当前没有实现这套规则（明确分界）

以下三条是 89584c4 源码事实，生成器与预览不得把设计态规则当成内核行为：

1. **阴影是固定四边，没有「贴边侧不投影」**。`paint_halo` 对整块调用 `gui_draw_border` 画四边：接触层 `b->x, b->y+1, b->w, b->h`（`paint.c:306`）；双层投影 `b->x, b->y+3` 与 `b->y+4`（`paint.c:309-310`）。代码路径不存在按边抑制的分支。
2. **角部覆写只处理四个角，没有 per-side 逻辑**。`corner_restore` 按上 / 下各 `r` 行覆写左右两列（`paint.c:324-331`）；`fill_round` 的掩码 inset 只按行算左右两边（`paint.c:253-257`）。没有「某一条边保持直角、其余边圆角」的计算。
3. **没有父子共享边合成逻辑**。父描边与子描边各走 `gui_draw_border`（`paint.c:121-136`）各画各的；不存在「共享边只留 1px `line` 色分隔线」的合并 pass。`BLK_DIVIDER` 是独立节点，画 1px `line` 角色色横线（`paint.c:370-372`），不是父子描边的合成产物。

### 4.4 口径分界表

| 事项 | 设计态（`.blk.json` / preview design） | 内核态（89584c4） |
|---|---|---|
| 贴边侧圆角归零 | 规则 R1 可表达、可断言 | 无逐角半径，谈不上按侧归零（第 5 章） |
| 贴边侧省略描边 | `stroke.sides` 可声明 | `border_w` 只支持 0/1/2 整框描边；无逐边描边字段（`block.h:113`） |
| 贴边侧抑制阴影 | `shadow.sides` 可声明 | `paint_halo` 固定四边（4.3-1） |
| 共享边 1px 分隔线 | 规则 D1 可断言 | 无合成；需要分隔线时显式放 `BLK_DIVIDER` 节点 |

> 规则 R1 / S1 / S2 / D1 是 `validate.py` 在生成数据上做的断言（第 8 章）；它们约束的是 `.blk.json` 自身的一致性，不代表内核绘制会执行对应裁剪。

---

## 5. 圆角覆盖率

### 5.1 calm 档尺寸阶梯

圆角只在 calm 档位生效，阶梯值 `CALM_RADII = {8, 12, 16, 24}`（`paint.c:197-198`），按块短边 `s = min(w, h)` 分档（`paint.c:234-240`）：

| 短边 `s` | 档索引 | 半径 | 代码注释定位 |
|---|---|---|---|
| `s < 32` | 0 | 8 | 小控件，`paint.c:239` |
| `32 <= s < 64` | 1 | 12 | 按钮 / 图标卡，`paint.c:238` |
| `64 <= s < 128` | 2 | 16 | 面板 / 弹出层外框，`paint.c:237` |
| `s >= 128` | 3 | 24 | 应用图标 / 大型欢迎卡，`paint.c:236` |

### 5.2 超椭圆 n≈5 的离线掩码

- 掩码在 `calm_masks_init` 里按档预生成（`paint.c:204-218`）：角象限内 `u = r-dx`、`v = r-dy`，判 `u⁵ + v⁵ <= r⁵`（`fifth_pow`，`paint.c:202,213-214`）。
- 每档 `r` 对应一张 `24×24` 掩码表（`paint.c:199`）；绘制按行查 `calm_inset` 得左右 inset（`paint.c:242-248`），无逐像素浮点。
- 若 `r*2 > w` 或 `r*2 > h`，退回整块直角矩形填充（`paint.c:252`）。

### 5.3 `corner_mode==1` 强制直角

`struct style.corner_mode`：`0` 继承模式 / `1` 强制直角 / `2` 强制圆角（`tokens.h:31`）。
`calm_shape_eligible` 中 `b->st.corner_mode == 1` 直接排除圆角（`paint.c:230`）。`corner_mode==2` 在 89584c4 源码中未见额外分支消费（未核实有强制圆角路径）。

### 5.4 `calm_shape_eligible` 的排除条件

定义于 `paint.c:226-232`，配合 `calm_on`（`paint.c:220-223`）：

| # | 排除条件 | 依据 |
|---|---|---|
| 1 | 非 calm 档（`gui_style_calm()` 为 0） | `paint.c:221`；档位读取 `settings.c:47`，持久化 `/etc/style` 含 `calm` 字样（`settings.c:26,52`） |
| 2 | 高对比度令牌（`TOK_HIGH_CONTRAST`） | `paint.c:222` |
| 3 | `BLK_ROOT` | `paint.c:228` |
| 4 | `bleed` 全出血块 | `paint.c:228` |
| 5 | `LAYER_CHROME` / `LAYER_BG` | `paint.c:229` |
| 6 | `corner_mode == 1` | `paint.c:230` |

### 5.5 为什么设计态逐角半径会在内核里退化成阶梯

1. `blk_spec` / `block` 没有逐角半径字段。`struct style` 仅保留 `corner_radius`（0 = 按尺寸走阶梯；>0 固定角档 px，`tokens.h:30`）与 `corner_mode`（`tokens.h:31`）；全仓检索 `corner_radius` 在 `tokens.h` 之外无消费者。
2. 绘制路径对整块只取一个档索引 `ri = calm_r_index(w, h)`（`paint.c:264,274,320`），`fill_round` 四个角共用同一 `r` 与同一张掩码（`paint.c:250-259`）；没有 tl/tr/br/bl 分别取半径的分支。
3. 因此 `.blk.json` 里的 `radius.{tl,tr,br,bl}` 只在 preview `design` 口径按逐角值绘制；落到内核内置页时，逐角值被 `min(w,h)` 阶梯取代，`radius_mode=per_corner` 仅 design 生效（`schema.json :: node.radius_mode`）。

### 5.6 与规格文档「不做圆角」的口径差

`docs/块化GUI-v2-规格.md:134` 砍项条目写作「不做旋转、alpha 混合、圆角、动画过渡」。这与 `paint.c` 的 calm 圆角**不构成矛盾**：

- 该砍项描述的是 v1 / app 默认态：`tokens.h:7-9` 注明 v1 砍项「radius 恒 0（直角）、elevation 恒 0（无阴影）、anim_ms 恒 0」；`tokens.c:13,19,25` 三套令牌 `radius/elevation/anim_ms` 均置 0。
- calm 是可选档位：`/etc/style` 置 calm 后（`settings.c:52`），`paint.c:196-260` 才画超椭圆圆角、`paint.c:296-312` 才画 halo 阴影。
- 口径结论：**v1 默认态无圆角无阴影；calm 可选态有阶梯圆角与层决定的双层阴影**。写 UI 时不要把 calm 效果当成 app 口径的保证。

---

## 6. 三条预览口径

### 6.1 口径对照表

依据 `schema.json :: preview_modes` 与 `FROZEN.md §3`；档位开关来自 `settings.c:35,47,49-55`。

| 模式 | 画什么 | 对应内核状态 | 内核依据 |
|---|---|---|---|
| `app`（默认） | 直角 + 1px 描边 + 无阴影 | `/etc/style` 非 calm；`calm_shape_eligible` 全 false | `paint.c:221,227-231`；`settings.c:47` |
| `calm` | 按块短边阶梯取圆角 8/12/16/24（超椭圆 n≈5）+ `paint_halo`：WORK 单层接触层；OVERLAY / TOAST 双层 | `/etc/style` 含 `calm`，且非高对比度令牌 | `paint.c:234-240,296-312,220-223`；`settings.c:52` |
| `design` | 按 `.blk.json` 逐角 `radius` 与 `shadow.sides` 渲染，并执行 `flush_rule` | **目标态，内核暂不具备** | `schema.json :: preview_modes.design`；缺口见第 4、5 章 |

### 6.2 对照纪律

- QEMU 截图对照只允许**同口径**比：`app` ↔ 内核 `style=app`；`calm` ↔ 内核 `style=calm`。不允许拿 calm 预览图对照 app 内核截图。
- `design` 口径只做「设计 vs 内核」差异清单（逐角半径、逐边描边 / 阴影、共享边分隔线），不冒充同布局、不做像素对齐验收。
- 预览配色逐字使用 `schema.json :: token_palette`（取自 `tokens.c:9-26`），禁止自拟配色；字模必须解析 `font_ascii.inc`，不许用系统字体近似（`FROZEN.md §4`）。

---

## 7. 不支持 / 必有损清单

### 7.1 硬不支持：命中即报 `UNSUPPORTED`，非 0 退出

依据 `schema.json :: css_properties.unsupported` 与 `policy`：命中 unsupported 一律以 `UNSUPPORTED <selector> <property>` 记录到 diagnostics 并以非 0 退出；仅 `--allow-lossy` 才降级为 warning。**禁止静默丢弃。**

| # | 不支持项 | 判定 / 处理策略 | schema 对应 |
|---|---|---|---|
| 1 | `float` | 报 `UNSUPPORTED <selector> float`，非 0 退出 | `unsupported[0]` |
| 2 | `position:fixed` | 同上；定位只支持 absolute / relative / static | `unsupported[1]` |
| 3 | `position:sticky` | 同上 | `unsupported[1]` |
| 4 | `grid` / `display:grid` | 内核无 Grid 列宽算法（规格 `:135`）；报不支持 | `unsupported[2]` |
| 5 | `inline-block` 布局流 | 行内只落 `TEXT` 块；报不支持 | `unsupported[3]` |
| 6 | `transform` / rotate / scale / translate | 无变换路径；报不支持 | `unsupported[4]` |
| 7 | `transition` / `animation` | `anim_ms` 恒 0（`tokens.h:28`；`tokens.c:13,19,25`）；报不支持 | `unsupported[5]` |
| 8 | `opacity < 1` | 无 alpha 混合；仅 `opacity:1` 允许，其余报不支持 | `unsupported[6]`；`supported.opacity` |
| 9 | `filter` / `backdrop-filter` | 无滤镜路径；报不支持 | `unsupported[7]` |
| 10 | `background-image` / `url()` | 位图只能以 `IMAGE` 节点 + 槽 74 句柄进入；报不支持 | `unsupported[8]` |
| 11 | `gradient` | 无渐变填充；报不支持 | `unsupported[9]` |
| 12 | `border-radius` 百分比或非档值 | 档值仅 `0/8/12/16/24`；其余报不支持 | `unsupported[10]`；`node.radius` |
| 13 | `font-family` / `font-weight` / `font-style` | 字模唯一来源 `font_ascii.inc`；无族 / 字重概念；报不支持 | `unsupported[11]` |
| 14 | `text-align` 非 `left` | 文本左起绘制（`paint.c:96-118`）；按钮文字居中是组件行为（`paint.c:383-385`）；报不支持 | `unsupported[12]` |
| 15 | 伪元素 `::before` / `::after` | 无伪节点机制；报不支持 | `unsupported[13]` |
| 16 | 绝对定位之外的自动换行排版 | `BLK_TEXT` 单行不换行；报不支持 | `unsupported[14]`；`text_overflow_policy` |
| 17 | `rgba()` / `hsl()` 任何带 alpha 的颜色 | 颜色为 `0x00RRGGBB` 不透明值（`block.h:14` 注释）；报不支持 | `unsupported[15]` |
| 18 | 混合模式 `mix-blend-mode` | 无混合通道；报不支持 | `unsupported[16]` |
| 19 | `clip-path` / `mask` | 无裁剪路径；报不支持 | `unsupported[17]` |
| 20 | `writing-mode` | 仅横排；报不支持 | `unsupported[18]` |
| 21 | `direction:rtl` | 无 RTL 双向排版；报不支持 | `unsupported[19]` |
| 22 | 表格 / 列表语义（`table`, `li`） | 无表格 / 列表组件；报不支持 | `unsupported[20]` |
| 23 | `iframe` / `canvas` / `video` / `svg` | `CANVAS` 是内核画布句柄节点，不是 HTML `<canvas>`；报不支持 | `unsupported[21]` |
| 24 | 多行文本自动折行 | 同 #16；超宽按 ellipsis 截断（7.2） | `unsupported[22]` |
| 25 | 任意 CSS 变量 `var()` | 不解析变量；报不支持 | `unsupported[23]` |
| 26 | 媒体查询 `@media` | 无响应式断点；报不支持 | `unsupported[24]` |
| 27 | `@font-face` | 字模内置；报不支持 | `unsupported[25]` |

补充 HTML 侧约束（`schema.json :: html_subset.attributes`）：`class` 只能命中 HTML 内联 `<style>` 里定义的受限规则；未知 class 报 `UNSUPPORTED_CLASS`；`style` 仅接受白名单属性。

### 7.2 文本单行与截断策略

- `BLK_TEXT` 单行不换行：超宽按 `ellipsis` 截断到最后一个能放下的字并追加 `...`；不足时直接 `clip`（`schema.json :: text_overflow_policy`；`paint.c:174-193`）。
- 度量口径与 `decl_text_width` 一致：ASCII 8px、双字节（`0x81..0xFE`）16px、`scale=2` 各乘 2（`decl.c:632-644`；`paint.c:100,117`）。
- 文本宽度放不下且未声明 `ellipsis` 时，规则 T1 校验失败（第 8 章）。

### 7.3 有损但仍可用：内核会压缩，生成器须显式记录

下列项不是硬不支持，但落到内核后会被压缩；`.blk.json` 保留设计态原值，preview `calm` / 内置页按压缩后口径呈现，diagnostics 应标注压缩动作。

| # | 设计态原值 | 内核压缩结果 | 依据 |
|---|---|---|---|
| 1 | 任意逐角 / 任意 px 圆角 | 退化为 calm 短边阶梯 8/12/16/24；非 calm 档恒直角 | `paint.c:234-240,250-259`；第 5 章 |
| 2 | 双层 / 任意边阴影 | 固定 halo：WORK 单层接触层（黑 18% @ y+1）；OVERLAY / TOAST 双层（黑 18% @ y+1，黑 10% @ y+3/y+4）；无逐边 | `paint.c:296-312` |
| 3 | 百分比定位 / 尺寸 | 1/1000 千分位取整；求解为整数除法 `aw * pct / 1000` | `layout.c:215,217,234,240`；`block.h:127-128` |
| 4 | 任意字号 | 只保留 `scale=1`（12px 口径）/ `scale=2`（16px 口径）两档；其它字号按 `schema.json :: supported.font-size` 报 `UNSUPPORTED_FONT_SIZE` | `schema.json :: supported.font-size`；`layout.c:22-23,46-47` |
| 5 | 任意颜色 | 优先映射到令牌角色（fg/bg/panel/bar/border/line/accent）；无法映射时取最近档；带 alpha 颜色不进入此项 | `tokens.c:9-26`；`paint.c:140-151,153-172` |

### 7.4 诊断格式与退出码

- 硬不支持项诊断文本固定为 `UNSUPPORTED <selector> <property>`，selector 取命中的元素选择器（标签 + id/class），property 取 CSS 属性名；写入 diagnostics，默认非 0 退出。
- `--allow-lossy`：把 7.1 硬不支持项降级为 warning，允许产出降级后的 `.blk.json`；7.3 的压缩动作仍须逐条标注。未加该标志时，任一硬不支持项不得进入产物。
- 诊断通道：默认 stderr；`--json` 输出机器可读诊断数组（`FROZEN.md §6`：所有脚本非 0 退出码表示失败）。

诊断示例（格式约定，非内核消息）：

```text
UNSUPPORTED .card transform translate(8px, 0)
UNSUPPORTED #hero background-image url("/bg.png")
UNSUPPORTED p font-weight 700
UNSUPPORTED .fade opacity 0.6
```

### 7.5 静默丢弃禁令

任何未在 7.1 白名单内、又未在 7.3 显式记录压缩的声明，不得在 html2blk 阶段悄悄省略；必须进入 diagnostics。`--allow-lossy` 只把 7.1 的非 0 退出降级为 warning，不改变「不支持项不进产物」这一口径。

---

## 8. 校验与验收清单

### 8.1 13 条规则逐条判定方法

规则 ID 与判定口径取自 `schema.json :: validation_rules`；`validate.py` 逐条断言。

| 规则 | 判定方法 |
|---|---|
| **C1** | 遍历节点：`id >= 1`（根 `id=0`）、树内唯一；每个非根 `parent` 指向存在节点；沿父链无环；节点数 ≤ 256；深度 ≤ 16。依据配额 `decl.h:23-24`，父链校验 `decl.c:197-209`。 |
| **G1** | 每个节点 `rect.x/y/w/h` 均为 8 的倍数（`schema.json :: allowed_spacing.grid=8`）。注意：这是生成器 / 产物约束，内核 `grid_snap` 不做坐标对齐（9.3）。 |
| **G2** | `padding / gap / margin` 取值分别属于 `allowed_spacing`：padding/margin `0/2/4/8/12/16/24`，gap `0/4/8/12/16/24`。 |
| **B1** | `overflow=clip` 时子块 `rect` 完全落在父内容矩形内；`visible/scroll` 允许越界，但必须显式声明对应 `overflow`。 |
| **B2** | 子块 `rect` 不得超出父块外框；`visible` 的越界仅允许在内容矩形 → 父外框之间。 |
| **R1** | 贴边侧对应的两个角 `radius` 必须为 0（4.2 效果 1）。 |
| **S1** | 贴边侧不得声明描边（4.2 效果 2）。 |
| **S2** | 贴边侧不得启用阴影（4.2 效果 3）。 |
| **D1** | 贴边共享边只允许 1px 分隔线；父子不得同时在该侧画 2px 以上描边（4.2 效果 4）。 |
| **T1** | 文本按 `font_ascii.inc` 度量（7.2）必须能落在自身内容矩形内；放不下则必须声明 `text_overflow=ellipsis`。 |
| **L1** | `layer` 取值合法（`bg/100/content/200/work/3000/chrome/4000/overlay/5000/toast`）；走 CE 投影时只允许 `work(200)`，依据 `decl.c:179`。 |
| **O1** | `overflow=scroll` 时 `scroll.x/y` 不得超过内容实际溢出范围。 |
| **P1** | `blk2bin` 产物：`text_off/pix_off` 落在串池内（`0 <= off < slen` 或 `-1`）；`recs` 长度 = `40 * n`（步长 = `sizeof(ce_blk_rec_t)`，`decl.c:329-330`）；`pad0=0`。复核方式：逐条解包 `ce_blk_rec_t` 十二成员（1.2 表偏移列），校验 `text_off` 指向的 NUL 串长度 ≤ 4096 B、串池总长 ≤ 32768 B；`pad0` 必须为 0；记录顺序与 `.blk.json` 树序一致，父记录先于子记录出现（对照 `decl.c:197-200` 的父链前置要求）。 |

### 8.2 `validate.py` 用法（可直接抄）

> 脚本归属见 `FROZEN.md §6`：`tools/blkhtml/validate.py`，纯 Python 3 标准库；非 0 退出码表示失败，诊断走 stderr 或 `--json`。

```bash
# 校验单个 .blk.json；全部规则通过退出 0，任一失败退出非 0
python3 tools/blkhtml/validate.py samples/page.blk.json

# 机器可读诊断（CI 用）
python3 tools/blkhtml/validate.py samples/page.blk.json --json

# 批量过 5 个样例
for f in samples/*.blk.json; do
  python3 tools/blkhtml/validate.py "$f" || exit 1
done
```

> 构建链路口径见 `FROZEN.md §5`：CE 应用经 `sh kernel_v25/extapp/build_ce.sh <app>.c` 构建并由 `apps.list` 嵌入 `/bin/*.ce`；内置页 C 片段由 `gui_shell_cmd()` 追加分支接入（append-only，不动既有枚举）。工具链本身纯 Python 3 标准库，零第三方依赖（`FROZEN.md §6`）。

```bash
# 链路串联：HTML → .blk.json → 二进制 / 内置页
python3 tools/blkhtml/html2blk.py  samples/page.html   -o samples/page.blk.json
python3 tools/blkhtml/blk2bin.py   samples/page.blk.json -o build/
python3 tools/blkhtml/blk2bin.py   samples/page.blk.json --emit-c -o build/page.c
python3 tools/blkhtml/preview.py   samples/page.blk.json --mode app    -o build/page-app.html
python3 tools/blkhtml/preview.py   samples/page.blk.json --mode calm  -o build/page-calm.html
python3 tools/blkhtml/preview.py   samples/page.blk.json --mode design -o build/page-design.html
```

### 8.3 新页面过门 checklist（8 条以内）

1. HTML 仅含 3.1 标签与 3.2–3.7 白名单属性；7.1 清单逐项扫描无命中。
2. 树规模：节点 ≤ 256、深度 ≤ 16、单块文本 ≤ 4096 B、串池 ≤ 32768 B（1.7）。
3. 几何：`rect` 四边 8 的倍数（G1）；padding/gap/margin 取值在档（G2）。
4. 越界：按 B1/B2 核对每个子块；需要越界必须显式 `overflow`。
5. 贴边：父子共享边按 R1/S1/S2/D1 归零 / 省略 / 抑制 / 留 1px 分隔线。
6. 文本：T1 度量通过；超长声明 ellipsis；字号只取 12 / 16 两档。
7. 层：CE 路径只用 `work`；内置页路径层值合法（L1）。
8. 产物：P1 复核串池偏移与记录长度；`validate.py` 全绿后方可进 preview / 内置页构建。

---

## 9. 附录：内核能力现状表

> 判读规则：`blk_spec` 有字段 ≠ 已实现；`layout.c` 有求解路径才算真求解。下表「依据」列给 `文件:行号`。

### 9.1 几何与定位

| 能力 | 内核有 | 内核无 | 依据 |
|---|---|---|---|
| 流式 COLUMN / ROW / STACK | 有：measure 聚合 + place 落位；STACK 子块层叠、交叉轴居中 | — | `layout.c:62-98,126-202` |
| `BLK_FREE` 自由布局求解 | 有：锚点 / 百分比 / 外边距 / min-max 求解 | 布局阶段不 clamp 坐标（越界交溢出策略） | `layout.c:207-248,250-259`；`block.h:31` |
| `anchor` 六点位 | 有：L/R/T/B/HCENTER/VCENTER 求解 | — | `layout.c:230-241`；`block.h:45-52` |
| `pct_x/y/w/h`（1/1000） | 有：整数除法求解 | 千分位取整误差（7.3 #3） | `layout.c:215,217,234,240` |
| `min_w/h max_w/h` | 有：只夹尺寸，不回写坐标 | — | `layout.c:221-224` |
| `margin m_*` | 有：扣进 FREE 父框 | — | `layout.c:208-210` |
| `bleed` 全出血 | 有：对齐父整框；圆角 / 阴影排除 | `blk_spec` 无成员，CE 不可表达（2.5） | `layout.c:105`；`paint.c:228`；`block.h:171` |
| 8pt 坐标对齐 | **内核无**：`grid_snap` 只归整 padding/gap，不碰坐标 | 坐标 8pt 是生成器约束（G1） | `layout.c:35-41,263-264` |
| `align_self` | 字段存在、会拷贝 | 无布局消费者（2.5） | `block.h:107,162`；`desc.c:33` |

### 9.2 溢出、交互与绘制

| 能力 | 内核有 | 内核无 | 依据 |
|---|---|---|---|
| overflow 三态 clip/visible/scroll | 有：裁剪栈 + 平移栈 | — | `block.h:38-42`；`paint.c:414-417,420-427` |
| `scroll_x/y` 视口平移 | 有：只改绘制落点，不改子块几何 | 无滚动条控件；无滚动边界求解（O1 由校验层约束） | `paint.c:416-417,427`；`block.h:139` |
| 拖拽 `drag_mode / drag_bounds` | 有：命中与拖拽收敛；`moved` 后保留绝对几何 | — | `input.c:119,200-214`；`layout.c:244-247`；`desc.c:49` |
| 缩放 `resize_dir/edge/min/max/step` | 有：方向命中、边缘带、下限 24 | `resize_step` 连续 / 步进在 89584c4 未见步进消费（未核实） | `input.c:102-114`；`desc.c:50-54`；`block.h:134-138` |
| 层级 `z` | 有：layer 升序 → z 升序 → 树先序稳定排序 | — | `compose.c:74,88-96` |
| 可见性 / 焦点 / inert | 有：`visible` 跳过绘制；`focusable` 接管键盘；`inert` 跳过命中 | — | `paint.c:335`；`input.c:65,137,189-190`；`desc.c:36-38` |
| 描边 `border_w` 0/1/2 | 有：整框四边；2px 为内缩同色第二层 | 无逐边描边字段；无逐边颜色 | `paint.c:121-136`；`block.h:113` |
| 圆角 | 仅 calm 短边阶梯 8/12/16/24，超椭圆 n≈5 | 无逐角半径；非 calm 恒直角；`corner_radius` 字段无消费者 | `paint.c:197-240,250-259`；`tokens.h:26,30` |
| 阴影 | `paint_halo` 按 layer 固定四边：WORK 单层；OVERLAY/TOAST 双层 | 无接触 / 投影档位选择；无逐边抑制；无自定义偏移 / 模糊 | `paint.c:296-312` |
| 分隔线合成 | `BLK_DIVIDER` 独立节点画 1px `line` 色横线 | 无父子共享边 1px 合成 pass（4.3-3） | `paint.c:370-372`；`block.h:29` |
| 文本 | 8x16 字模单行；scale 1/2；ellipsis / clip | 无折行；无族 / 字重；无居中段落（按钮除外） | `font_ascii.inc:1-8`；`paint.c:96-118,174-193`；`decl.c:632-644` |
| 位图 | IMAGE/SURFACE/CANVAS 按句柄 blit；CANVAS 子矩形脏区 | 无 alpha 位图；内联位图被 CE 拒绝 | `paint.c:356-368`；`decl.c:182-183,293-300` |
| `BLK_TERM` 块内终端 | 内核内部组件类型；HTML 子集不产出，CE 通道不可提交 | 生成器不为它生成映射；探针 / 内置页专用 | `block.h:34`；`decl.c:176` 排除 |
| `SPACER` / `DIVIDER` | SPACER 取 `req_w/req_h`；DIVIDER 高 1、画 `line` 色横线 | — | `layout.c:29,30,57,58`；`paint.c:370-372` |
| 事件投递 | 有：点击 / 按键 / 焦点 / 尺寸 / 关闭经槽 73 轮询；只给局部坐标，不给绝对屏幕坐标与鼠标原始流 | 无原生右键菜单 / 悬停事件通道（未核实有独立 hover 事件类型） | `decl.h:28-36,44`；`ce_abi.h:100-105` |
| 资源载入 | 有：槽 74 按路径载入位图，返回归属本应用的句柄；槽 75 释放 | 无运行时 SVG / 视频解码；位图仅不透明 | `decl.c:154-159,213-219`；`block.h:189` |

### 9.3 容易误读的三条边界

1. **有字段 ≠ 有实现**：`blk_spec` 收录 `anchor/pct/overflow/scroll` 等字段，且 `layout_free` 真的求解了它们；但 `grid_snap` 只归整 padding/gap，不做坐标 8pt 对齐（`layout.c:35-41`）。不要据此声称内核会把坐标吸到 8pt 网格。
2. **calm 效果是可选档，不是默认承诺**：app 口径下圆角 / 阴影全部不出现（5.6）；预览对照必须同口径（6.2）。
3. **设计态字段先落 `.blk.json`，内核缺口如实上报**：逐角半径、逐边描边 / 阴影、共享边分隔线在 `blk_spec` 无对应成员；不许往 ABI 里加字段，也不许在文档里声称内核已支持（4.4、第 5 章）。

---

## 附：引用源码索引

| 文件 | 关键内容 |
|---|---|
| `kernel_v25/include/ce_abi.h:87-105,320-325` | `ce_blk_rec_t` / `ce_blk_event_t`；槽 72–76 |
| `kernel_v25/include/gui/block.h:19-140,153-203` | kind / OVF / ANC / DRAG / BOUND / RESIZE / 对齐 / 层枚举；`blk_spec`；`block` |
| `kernel_v25/include/gui/tokens.h:13-32,42-51` | `struct style`；角色枚举 |
| `kernel_v25/include/gui/decl.h:16-26,29-47` | 拒绝码；配额；事件类型；CE 接线声明 |
| `kernel_v25/src/gui/decl.c:163-222,253-315,317-361,632-644` | 校验 / 落地 / 提交；文本度量 |
| `kernel_v25/src/gui/layout.c:35-41,101-124,126-202,207-290` | 网格归整；流式布局；FREE 求解 |
| `kernel_v25/src/gui/paint.c:121-136,174-193,197-259,261-285,296-332,334-433` | 描边；截断；calm 掩码 / 阶梯；halo；角覆写；节点绘制 |
| `kernel_v25/src/gui/desc.c:12-81,94-150` | `apply_spec` 字段镜像；内置页建块 API |
| `kernel_v25/src/gui/compose.c:74,88-96` | 绘制排序 layer → z → 先序 |
| `kernel_v25/src/gui/input.c:65,102-119,137,189-214` | 命中跳过 inert；拖拽 / 缩放；焦点 |
| `kernel_v25/src/gui/settings.c:26,35,47,49-60` | calm 档位持久化与读取 |
| `kernel_v25/src/gui/tokens.c:9-26` | dark / light / hc 三套令牌取值 |
| `kernel_v25/src/font_ascii.inc:1-8` | 8x16 ASCII 字模口径 |
| `kernel_v25/extapp/dektest.c:34-64` | CE 提交最小范例 |
| `docs/块化GUI-v2-规格.md:134-136` | v2 砍项口径（圆角 / Grid / 滚动条） |
