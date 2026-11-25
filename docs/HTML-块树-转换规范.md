# CatOS HTML/CSS → 块树转换规范

> 版本：v0.2 · 状态：草案（待冻结） · 基线：`89584c4`
> 适用范围：把受控 HTML/CSS 输入映射为 CatOS 内核块树的全部规则、边界、断言与验收口径。
> 本文是该映射的唯一真源；随仓库附带的转换脚本、预览脚本、校验脚本均为可弃的参考实现，其字段表与诊断码以本文为准。

## 0. 定位：三层真源关系

### 0.1 三层各是什么

| 层 | 形态 | 角色 | 可替换性 |
|---|---|---|---|
| 输入形态 | HTML + 受控 CSS + `data-blk-*` 属性 | 只表达布局意图，不是运行时模型，不携带内核语义 | 可被任何等价意图输入替代；本文规则约束的是意图到块树的映射，不约束 HTML 语法本身 |
| 人可读中间层 | `.blk.json` 节点树 | 树结构、声明几何、样式意图的人可读镜像；非运行时格式、非唯一真源 | 可被任何等价中间表示（如 C 描述数组、二进制记录）替代；其字段定义以本文 §4、§8 为准 |
| 运行时事实 | 内核 `blk_spec` / `block` / `ce_blk_rec_t` | 块树在内核中的真实结构、布局求解与绘制行为；唯一最终裁判 | 不可由文档或工具声明覆盖；与本文冲突时以源码为准 |

### 0.2 真源关系图

```text
 HTML/CSS 意图                .blk.json 中间层                 内核运行时
 ───────────────             ──────────────────             ──────────────────
 受控标签/属性   ──M-* 规则──▶  节点树（ce 镜像 + 声明超集） ──▶  block 树
 (§4 映射规则)                 (§2 两层模型)                   (§1 运行时事实)
                                        │                          ▲
                                        │ 几何/样式全保真路径          │ 布局/绘制消费
                                        ▼                          │
                                 内置页 blk_spec 全字段 ─────────────┘
                                        │
                                        │ 结构保真路径（槽 72）
                                        ▼
                                 ce_blk_rec_t 扁平记录（40B/条）
```

### 0.3 判定纪律

1. 文档与参考实现冲突时，以本文为准。
2. 本文与内核源码冲突时，以源码为准；同时在 §10 登记一条文档债，注明冲突条款、源码位置与待同步方向。
3. 参考实现中的机器化字段副本（脚本内枚举、白名单、诊断串）不构成权威定义；本文未收录的取值域一律视为未定义，不得由脚本反推。
4. 本文只描述映射规则与可表达边界；不承诺内核尚未实现的行为。内核缺口按 §3 演进计划登记，不在当前版本冒充已支持。

### 0.4 术语口径

- **块树**：运行时挂在内核根块之下的 `block` 节点树，见 `kernel_v25/include/gui/block.h::block`。
- **CE 记录**：`ce_blk_rec_t`，用户态可经槽 72 提交的扁平二进制记录，见 `kernel_v25/include/ce_abi.h::ce_blk_rec_t`。
- **声明超集**：`.blk.json` 中除 `ce` 镜像字段外的几何、样式字段；仅对内置页路径有意义，见 §2。
- **设计态**：本文档与中间层表达的目标布局；**内核态**：基线源码的实际行为。两者不一致处按 §5、§7、§9 标注。
- **贴边（flush）**：布局量化后坐标与父内容区边界或兄弟块边界精确相等，容差 0；判定口径引用 `块化GUI-设计规格.md §14.1`，本文不复制其正文。

### 0.5 引用格式

所有内核事实引用统一写作反引号内 `` `相对仓库根的路径::符号名` ``，行号写在反引号外：`` `kernel_v25/src/gui/decl.c::validate` (L163) ``。符号名仅取 `[A-Za-z_][A-Za-z0-9_]*`；行号仅作定位提示，符号名稳定、行号会漂移，复核按符号名进行（见 §8 spec-audit）。文件后缀限定 `.c / .h / .inc`。

---

## 1. 运行时事实：块树不是 JSON

### 1.1 提交通道是等长记录数组 + 字符串池

CE 通道提交的不是 JSON，也不是嵌套树，而是一批等长记录加一个字节串池。记录结构 `ce_blk_rec_t` 在线上的镜像定义于 `kernel_v25/include/ce_abi.h::ce_blk_rec_t`：

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

### 1.2 十二成员逐字段表（40 字节/条）

结构共 12 个成员、40 字节/条，无内部空洞。

| # | 成员 | 类型 | 偏移 | 语义 | 内核校验依据 |
|---|---|---|---|---|---|
| 1 | `id` | int32 | 0 | 块 id，必须 `>=1`；本批内唯一 | `kernel_v25/src/gui/decl.c::validate` (L172)：`id < 1` 或重复 → `E_DECL_BADID` |
| 2 | `parent` | int32 | 4 | 父块 id；`0` 挂到本应用内核自动创建的根块之下；非 0 必须是本批更早出现过的 id | `kernel_v25/src/gui/decl.c::validate` (L197)：父链断裂 → `E_DECL_BADID` |
| 3 | `kind` | uint8 | 8 | `blk_kind` 枚举值，见 §4.1 | `kernel_v25/src/gui/decl.c::validate` (L176)：`< BLK_COLUMN` 或 `> BLK_CANVAS` → `E_DECL_BADREC` |
| 4 | `pad0` | uint8 | 9 | 对齐填充，必须写 0；内核不消费 | 布局见 `kernel_v25/include/ce_abi.h::ce_blk_rec_t`；产物断言 P1 |
| 5 | `layer` | uint16 | 10 | 层枚举；经槽 72 只接受 `LAYER_WORK(200)` | `kernel_v25/src/gui/decl.c::validate` (L179)：`layer != LAYER_WORK` → `E_DECL_LAYER` |
| 6 | `req_w` | int32 | 12 | 声明宽；`-1` = auto | 落地见 `kernel_v25/src/gui/decl.c::decl_apply` (L275) |
| 7 | `req_h` | int32 | 16 | 声明高；`-1` = auto | 同上 |
| 8 | `text_off` | int32 | 20 | 文本在串池中的起始偏移；`-1` = 无文本；否则 `0 <= text_off < slen` | `kernel_v25/src/gui/decl.c::validate` (L187)：越界 → `E_DECL_BADREC`；文本长 > 4096 → `E_DECL_QUOTA` |
| 9 | `pix_off` | int32 | 24 | 位图偏移；线上恒为 `-1`，CE 通道内联位图一律拒绝 | `kernel_v25/src/gui/decl.c::validate` (L182)：`pix_off != -1` 或 `pix_w/pix_h != 0` → `E_PIX_FIELD` |
| 10 | `pix_w` | int32 | 28 | 位图宽；CE 记录内必须为 0，宽高由资源句柄解析 | 同上；IMAGE 资源解析见 `kernel_v25/src/gui/decl.c::decl_apply` (L293) |
| 11 | `pix_h` | int32 | 32 | 位图高；同上，必须为 0 | 同上 |
| 12 | `res` | int32 | 36 | 槽 74 载入的图像 / 画布资源句柄；`-1` = 无 | `kernel_v25/src/gui/decl.c::validate` (L213)：CANVAS / IMAGE 必须持有归属本应用的句柄；其余 kind 带句柄 → `E_DECL_RES` |

### 1.3 字节布局与步长

按成员声明顺序：`id@0(4) + parent@4(4) + kind@8(1) + pad0@9(1) + layer@10(2) + req_w@12(4) + req_h@16(4) + text_off@20(4) + pix_off@24(4) + pix_w@28(4) + pix_h@32(4) + res@36(4) = 40` 字节。内核拷贝长度按 `n * sizeof(ce_blk_rec_t)` 计算，见 `kernel_v25/src/gui/decl.c::decl_commit` (L329)；产物 `recs.bin` 长度必须为 `40 * n`（断言 P1）。

### 1.4 `text_off` 语义与字符串池

- 串池由提交调用的 `strings / slen` 参数带入，见 `kernel_v25/src/gui/decl.c::decl_commit` (L317)；内核 `copy_from_user` 拷入 scratch 池后再校验。
- `text_off == -1`：该块无文本，偏移不参与池内定位。
- `text_off >= 0`：从 `pool + text_off` 起读到 NUL 为止；长度计入单块文本配额 4096 B。
- 内核不保存用户态指针：文本由 `kernel_v25/src/gui/decl.c::decl_copy_text` 复制进本应用 arena；ABI 注释明确内核绝不保存任何用户态指针。
- 串池按 NUL 分隔多段文本；`text_off` 指向段首，段长由读到的第一个 NUL 决定。池尾多余 NUL 不影响校验，但 `slen` 不得超过 32768 B。
- 产物侧池去重规则见 §4.9；`text_off` 在产物生成阶段按去重后池重新计算。

### 1.5 `kind` 枚举与 CE 通道可提交范围

枚举定义于 `kernel_v25/include/gui/block.h::blk_kind`，取值见 §4.1 表。CE 槽 72 可提交范围：

| 范围 | kind | 依据 |
|---|---|---|
| 可提交 | `BLK_COLUMN(1)` … `BLK_CANVAS(10)` | `kernel_v25/src/gui/decl.c::validate` (L176) 上下限 |
| 不可提交 | `BLK_ROOT(0)`（应用根块由内核自动创建）、`BLK_FREE(11)`、`BLK_BUTTON(12)`、`BLK_INPUT(13)`、`BLK_TERM(14)` | 同上；`BLK_ROOT` 创建见 `kernel_v25/src/gui/decl.c::decl_ensure_root` (L226) |

`BLK_FREE / BUTTON / INPUT / TERM` 在内核内置页路径中可由建块 API 创建，只是不能经 CE 记录提交。

### 1.6 槽位分工（72–76）

槽位表见 `kernel_v25/include/ce_abi.h::catos_api_t`：

| 槽 | 签名 | 职责 |
|---|---|---|
| 72 | `blk_commit(const void *recs, int n, const char *strings, int slen)` | 整批提交块记录 + 串池；校验失败整批拒绝 |
| 73 | `blk_event_poll(ce_blk_event_t *ev, int timeout_ms)` | 轮询一个本应用事件（点击 / 按键 / 焦点 / 尺寸 / 关闭） |
| 74 | `blk_image_load(const char *path)` | 载入位图资源，返回句柄；失败返回 `<0` |
| 75 | `blk_image_free(int res)` | 释放句柄 |
| 76 | `blk_text_width(const char *s)` | 文本度量，口径见 §4.7 |

事件类型枚举见 `kernel_v25/include/gui/decl.h`（CLICK / KEY / FOCUS_IN / FOCUS_OUT / RESIZE / CLOSE）；事件结构 `ce_blk_event_t` 见 `kernel_v25/include/ce_abi.h::ce_blk_event_t`。事件只给块内局部坐标，不给绝对屏幕坐标、不给鼠标原始流。

### 1.7 提交语义：整批校验、整批拒绝、配额

`kernel_v25/src/gui/decl.c::decl_commit` (L317) 流程：

1. 配额预检：`n < 0 || n > 256`、`slen < 0 || slen > 32768` → `E_DECL_QUOTA` 并拒绝。
2. `copy_from_user` 拷入记录区与串池；拷贝失败 → `E_DECL_BADREC`。
3. `validate()` 全批静态校验；任一记录非法 → `decl_reject(rc)`，整批拒绝，不触碰旧子树。
4. 校验通过后才进入 `decl_apply` 落地：先回收旧子树（保留根块本身），再逐条建块挂树。

单应用配额（`kernel_v25/include/gui/decl.h`）：

| 配额 | 值 | 拒绝码 |
|---|---|---|
| 最大块数 `DECL_MAX_BLOCKS` | 256 | `E_DECL_QUOTA(-35)` |
| 最大深度 `DECL_MAX_DEPTH` | 16 | `E_DECL_QUOTA(-35)` |
| 单块最大文本 `DECL_MAX_TEXT` | 4096 B | `E_DECL_QUOTA(-35)` |
| 串池最大 `DECL_MAX_POOL` | 32768 B | `E_DECL_QUOTA(-35)` |

拒绝码表（`kernel_v25/include/gui/decl.h`）：`-35` 配额、`-36` 越层、`-37` id、`-38` 资源、`-39` 记录结构；另有 `-33` 内联像素字段（`kernel_v25/include/ce_abi.h::E_PIX_FIELD`）。

### 1.8 CE 声明树在桌面显示的当前边界

进入 GUI 的唯一入口 `gui_begin()` 会经 `gui_reset()` 调用 `tree_reset()` 清空根块子树，见 `kernel_v25/src/gui/compose.c::gui_reset` (L326) 与 `kernel_v25/src/gui/tree.c::tree_reset` (L67)。基线 `89584c4` 上不存在「CE 声明树挂到桌面并参与帧绘制」的只读查看入口；CE 路线的可视对照帧在当前版本不可取，验收以提交 verdict 与字段级核对为准（见 §9 各样例证据口径）。

### 1.9 最小 C 提交范例

参考现成探针 `kernel_v25/extapp/dektest.c`：3 条记录（根 COLUMN + TEXT + RECT），串池 `"Hi"`，提交后轮询事件、度量文本、载入并释放位图。

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

要点：所有记录 `layer=200`；无图块 `pix_off/pix_w/pix_h/res` 按 §1.2 表置位；文本偏移在产物生成阶段重新计算。

---

## 2. 两层模型：CE 可表达层 vs 声明超集

### 2.1 结论先行

`ce_blk_rec_t` 里没有几何、没有 padding/gap/align、没有 anchor/pct、没有 overflow、没有颜色、没有 radius/shadow。记录成员止于 §1.2 表内十二项；对照 `blk_spec` 全字段（`kernel_v25/include/gui/block.h::blk_spec`）可逐项核对。

因此中间层必须是两层：

| 层 | 字段 | 去向 |
|---|---|---|
| `ce` 镜像层 | `id / parent / kind / layer(仅 work) / req_w / req_h / text / pix / res` | 产物记录 + 串池 → 槽 72（§1） |
| 声明超集层 | `rect / padding / gap / layout / anchor / px / pct / min_max / margin / overflow / scroll / drag / resize / bleed / z / fill / fg / accent / stroke / radius / radius_mode / shadow / font / visible / focusable / inert` | 只进内置页 `blk_spec` 全字段路径，不能经槽 72 |

### 2.2 CE 可表达字段

可投影到记录成员的字段：`id`、`parent`、`kind`、`layer(仅 work)`、`req_w`、`req_h`、`text`、`pix/res`。`pix` 在 CE 通道以资源句柄 `res` 表达，内联位图被 `kernel_v25/src/gui/decl.c::validate` (L182) 拒绝。

### 2.3 过不了槽 72 的声明字段

| 组 | 字段 | 内置页对应位置 | 消费路径依据 |
|---|---|---|---|
| 1 | `rect` 绝对几何 | `block.x/y/w/h`（布局结果） | `kernel_v25/src/gui/layout.c::layout_node` (L261) |
| 2 | `padding` | `pad_t / pad_r / pad_b / pad_l` | `kernel_v25/src/gui/layout.c::layout_node` (L263) |
| 3 | `gap` | `gap` | `kernel_v25/src/gui/layout.c::measure_node` (L79) |
| 4 | `align_main` | `align_main` | `kernel_v25/src/gui/layout.c::layout_column` (L137) |
| 5 | `align_cross` | `align_cross` | `kernel_v25/src/gui/layout.c::place_child` (L115) |
| 6 | `anchor / px / pct / min / max / margin` | `anchor / px_x,y / pct_x,y,w,h / min_w,h,max_w,h / m_t,r,b,l` | `kernel_v25/src/gui/layout.c::layout_free_child` (L207) |
| 7 | `overflow / scroll` | `overflow / scroll_x,scroll_y` | `kernel_v25/src/gui/paint.c::paint_node` (L414) |
| 8 | `drag / drag_bounds` | `drag_mode / drag_bounds` | `kernel_v25/include/gui/block.h::block` |
| 9 | `resize` | `resize_dir / edge / min_w / min_h / max_w / max_h / step` | 同上 |
| 10 | `bleed` | `block.bleed`（`blk_spec` 无此成员，见 §2.5） | `kernel_v25/src/gui/layout.c::child_box` (L105) |
| 11 | `z` | `z` | `kernel_v25/src/gui/compose.c`（绘制排序） |
| 12 | `fill / fg / accent / stroke` | `fg / bg / accent / role_fg / role_bg / role_accent / border_w` | `kernel_v25/src/gui/paint.c::resolve_color` (L153) |
| 13 | `radius / radius_mode` | 内核仅 `st.corner_radius / corner_mode`，无逐角半径字段 | §5；`kernel_v25/src/gui/paint.c::fill_round` (L250) |
| 14 | `shadow` | 内核无阴影档位字段；由 `paint_halo` 按 layer 固定产出 | `kernel_v25/src/gui/paint.c::paint_halo` (L296) |
| 15 | `font.scale` | `st.scale` | `kernel_v25/src/gui/paint.c::gui_draw_text` (L96) |
| 16 | `visible / focusable / inert` | `visible / focusable / inert` | `kernel_v25/src/gui/paint.c::paint_node` (L335) |

### 2.4 全保真只能走内置页 `blk_spec`

槽 72 只能还原「树形 + 种类 + 层 + 声明尺寸 + 文本 + 位图」；几何与样式全部丢失。需要几何 / 样式全保真的页面，路径是产出内置页 C 片段，经由建块 API 把 `blk_spec` 全字段建进内核块树；不是 CE 应用。内置页追加方式为 append-only，不动既有枚举。

### 2.5 两个字段口径提醒

- **`bleed` 不在 `blk_spec` 里**：`block.bleed` 是运行时块字段，`blk_spec` 无对应成员；内置页建块后需按内核既有模式直接置位。CE 通道无法表达。
- **`align_self` 无布局消费者**：`blk_spec` / `block` 均有该成员，建块镜像会拷贝，但 `kernel_v25/src/gui/layout.c` 全文未读取 `align_self`；生成器不要为它声明期望行为。

---

## 3. 目标形态：blk_spec v2 演进计划

### 3.1 双向同步纪律

字段在 gui-fix 侧定稿后，本 md 同步更新字段表；本 md 先行提出的字段，以 gui-fix 侧落地后的命名为准。本节登记的是**演进计划**，不是当前基线已实现的能力；基线 `89584c4` 的可表达边界以 §1、§2、§5 为准。

### 3.2 动因：三个 ABI 缺口

| 缺口 | 现状（基线事实） | 影响 |
|---|---|---|
| 缺口 1：逐角半径 / 贴边方角 | `struct style` 仅保留整块 `corner_radius` 与 `corner_mode`；绘制路径四个角共用同一档半径与同一张掩码，见 `kernel_v25/src/gui/paint.c::fill_round` (L250) | 贴边侧无法按 §14.1 判定逐角方角；成组紧贴的接缝会漏父底色缝 |
| 缺口 2：共享边双线 | 描边为整框四边，无逐边侧别位掩码；父描边与子描边各画各的，见 `kernel_v25/src/gui/paint.c::gui_draw_border` (L121) | 父子共享边出现 2px 双线；贴边侧无法省略描边 / 抑制阴影 |
| 缺口 3：CE 通道无几何 | `ce_blk_rec_t` 十二成员不含 padding/gap/align/anchor/颜色/样式（§2.3） | CE 应用只能还原树形与文本，几何与样式全丢；FREE/BUTTON/INPUT/TERM 不可提交 |

### 3.3 A. `blk_spec` / `block` 追加字段（内核绘制 / 布局消费）

> 形态约束：append-only 追加在结构末尾；不改旧结构语义、不改旧槽号。默认值列给出旧记录缺省时的行为。

| 字段 | 语义 | 默认值（旧记录缺省行为） | 向后兼容规则 | 内核消费点 | 验收断言 |
|---|---|---|---|---|---|
| `corner_radius[4]` | 逐角半径（tl/tr/br/bl），单位 px；贴边角取 0，判定见 `块化GUI-设计规格.md §14.1` | 全 0：按现有 calm 短边阶梯取整档半径 | 旧 `blk_spec` 不含该字段时，绘制路径回退现有 `corner_radius` 阶梯；旧二进制提交后渲染不变 | `kernel_v25/src/gui/paint.c::fill_round`；`kernel_v25/src/gui/paint.c::corner_restore` | 逐角半径在贴边角为 0；自由边半径落在阶梯档内（对应 R1） |
| `border_mask` | 描边侧别位掩码（L/R/T/B），解决共享边 2px 双线；合并规则见 `块化GUI-设计规格.md §14.3` | 全 1（四边）：等同现有整框描边 | 旧记录缺省全 1，`gui_draw_border` 行为不变；位 0 的侧别不画描边 | `kernel_v25/src/gui/paint.c::body_border`；`kernel_v25/src/gui/paint.c::gui_draw_border` | 贴边侧 `border_mask` 位为 0；共享边只保留 1px `line` 色分隔线（对应 S1、D1） |
| `anim_ms` | 过渡时长（毫秒）；0 = 无动画，位置瞬移 | 0：等同基线 `kernel_v25/include/gui/tokens.h` 预留字段恒 0 的行为 | 旧记录缺省 0，绘制 / 帧循环不产生过渡；非 0 才走过渡路径 | 帧循环消费点待 gui-fix 定稿（基线无消费者，未核实） | `anim_ms=0` 时两帧之间无中间帧；非 0 时长不超过声明值 |
| `show_in_taskbar` | 开始菜单 / 任务栏可见性 | 1（可见）：等同基线默认入栏行为 | 旧记录缺省 1；置 0 仅影响栏位收录，不影响块树本身 | 任务栏 / 开始菜单构建路径（符号待 gui-fix 定稿，未核实） | 置 0 的块不出现在任务栏与开始菜单条目 |
| `place` | 落位模式：`inline`（流式）/ `float`（自由浮层）/ `anchor`（锚定父框） | `inline`：等同现有 COLUMN/ROW/STACK 流式求解 | 旧记录缺省 `inline`，`layout_node` 现有分支不变；`float/anchor` 走新增自由布局分支 | `kernel_v25/src/gui/layout.c::layout_node`；`kernel_v25/src/gui/layout.c::layout_free_child` | `float` 子块不参与父容器主轴流动；`anchor` 子块按锚点落位 |
| `flip` | 镜像方向（水平 / 垂直 / 无） | 无：等同现有 1:1 blit 与文本绘制 | 旧记录缺省无镜像；非无值时 blit / 文本路径按方向翻转 | `kernel_v25/src/gui/paint.c::gui_blit24`；`kernel_v25/src/gui/paint.c::gui_draw_text`（消费者待落地） | 镜像块像素关于中轴对称；文本镜像行为在未落地前按未核实处理 |
| `clamp` | min/max 夹取策略：尺寸夹取 / 坐标夹取 / 不夹 | 尺寸夹取：等同现有 `min_w/h max_w/h` 只夹尺寸、不回写坐标 | 旧记录缺省现有策略；坐标夹取为新增选项，不改变旧记录坐标行为 | `kernel_v25/src/gui/layout.c::layout_free_child` (L221) | 尺寸夹取不回写坐标；坐标夹取后子块不越父框（对应 B1、B2） |

### 3.4 B. `ce_blk_rec_t` v2 追加（声明通道扩容）

> 形态：append-only 扩展记录块。建议形态为 `ce_blk_rec2_t` 追加在 `recs` 之后，由 `blk_commit` 的能力探测决定是否解析；旧二进制仍可按 40B 步长提交，解析器遇到无 v2 能力的内核时忽略扩展块。**不许改旧结构语义、不许改旧槽号。**

| 字段 | 语义 | 默认值（旧记录缺省行为） | 向后兼容规则 | 内核消费点 | 验收断言 |
|---|---|---|---|---|---|
| `pad_l / pad_r / pad_t / pad_b` | 四边内边距 px | 0：等同基线记录落地后 padding 为 0 | 旧记录缺省 0；v2 记录携带后由布局路径读取 | `kernel_v25/src/gui/layout.c::layout_node` (L263) | 内容框 = 外框 − padding；padding 取值在 §4.3 档位内（G2） |
| `gap` | 子块间距 px | 0：等同基线无间距 | 旧记录缺省 0；仅容器 kind 消费 | `kernel_v25/src/gui/layout.c::measure_node` (L79) | 相邻子块间距 = 声明 gap（G2） |
| `anchor` | 锚点位位掩码，取值见 §4.4 | 0：左/上锚 | 旧记录缺省 0；仅 `place=anchor/float` 消费 | `kernel_v25/src/gui/layout.c::layout_free_child` (L230) | 锚点位与 px/pct 求解结果一致 |
| `place` | 落位模式，同 §3.3 | `inline` | 旧记录缺省 `inline` | 同 §3.3 | 同 §3.3 |
| `flip` | 镜像方向，同 §3.3 | 无 | 旧记录缺省无 | 同 §3.3 | 同 §3.3 |
| `clamp` | 夹取策略，同 §3.3 | 尺寸夹取 | 旧记录缺省现有策略 | 同 §3.3 | 同 §3.3 |
| `min_w / min_h` | 最小尺寸 px；0 = 不设 | 0 | 旧记录缺省 0 | `kernel_v25/src/gui/layout.c::layout_free_child` (L221) | 尺寸不小于 min（B1） |
| `max_w / max_h` | 最大尺寸 px；0 = 不设 | 0 | 旧记录缺省 0 | 同上 | 尺寸不大于 max（B1） |
| `m_t / m_r / m_b / m_l` | 外边距 px | 0 | 旧记录缺省 0；扣进 FREE 父框 | `kernel_v25/src/gui/layout.c::layout_free_child` (L208) | 父框扣边距后再落锚点 |
| `corner_radius[4]` | 逐角半径，同 §3.3 | 全 0 | 旧记录缺省 0；CE 通道落地后镜像到块样式 | 同 §3.3 | 同 §3.3 |
| `border_mask` | 描边侧别掩码，同 §3.3 | 全 1 | 旧记录缺省全 1 | 同 §3.3 | 同 §3.3 |

### 3.5 缺口 → 字段 → 断言对照表

| 缺口 | 对应字段 | 验收断言 |
|---|---|---|
| 缺口 1：逐角半径 / 贴边方角 | `corner_radius[4]`（§3.3、§3.4） | R1；贴边角 3×3 邻域无弧线过渡像素（`块化GUI-设计规格.md §14.6`） |
| 缺口 2：共享边双线 | `border_mask`（§3.3、§3.4） | S1、S2、D1；共享边厚度 1px，无双线、无父底色缝 |
| 缺口 3：CE 通道无几何 | §3.4 全组 v2 记录字段 | P1；v2 记录步长与能力探测兼容；旧二进制提交渲染不变 |

---

## 4. HTML/CSS → 块树映射规则（M-*）

### 4.0 规则编号与引用纪律

- 映射规则编号 `M-<章>.<节>-<两位序号>`，例 `M-4.3-01`（尺寸 / 内边距）、`M-4.4-01`（绝对定位）。
- 诊断信息只引用规则 ID 与断言 ID，不引用文件行号；行号仅出现在本文 §1、§8 的源码核对处。
- 本章白名单之外的 CSS 属性进入 §7 三态判定；命中甲类（结构性不可表达）按 §3 演进计划登记，命中乙类（工具暂不支持）给扩展成本，命中丙类（永久不支持）给替代写法。

### 4.1 标签 → `kind`（M-4.1）

| 规则 ID | HTML 标签 | 块 `kind` | 枚举值 | 备注 / 依据 |
|---|---|---|---|---|
| M-4.1-01 | `<div>` + `display:flex; flex-direction:row` | `ROW` | 2 | 流式横排，`kernel_v25/src/gui/layout.c::layout_row` (L156) |
| M-4.1-02 | `<div>` + `display:flex; flex-direction:column` | `COLUMN` | 1 | 流式纵排，`kernel_v25/src/gui/layout.c::layout_column` (L126) |
| M-4.1-03 | `<div>` 无 flex（`display:block`） | `COLUMN` | 1 | 无 flex 的 block 落 COLUMN |
| M-4.1-04 | `<div>` + `position:absolute` 父容器 | `FREE` | 11 | 自由布局容器，`kernel_v25/src/gui/layout.c::layout_free` (L250)；CE 通道不可提交（§1.5） |
| M-4.1-05 | `<span>` | `TEXT` | 4 | 行内即文本块；文本单行，不折行（§7 甲类 U-7.2-04） |
| M-4.1-06 | `<button>` | `BUTTON` | 12 | 内置页路径可用；CE 通道不可提交；建块时自动置 `focusable=1` |
| M-4.1-07 | `<input type="text">` | `INPUT` | 13 | 同上；单行输入 |
| M-4.1-08 | `<img>` | `IMAGE` | 6 | 位图经槽 74 载入取句柄；`pix.{path,w,h,res}` |
| M-4.1-09 | `<hr>` | `DIVIDER` | 9 | 绘制为 1px `line` 角色色横线，`kernel_v25/src/gui/paint.c::paint_node` (L370) |
| M-4.1-10 | `<br>` / 空白占位 | `SPACER` | 8 | 尺寸取 `req_w/req_h`，`kernel_v25/src/gui/layout.c::leaf_natural_w` (L29) |
| M-4.1-11 | （无直接 HTML 标签） | `STACK` | 3 | 层叠容器：子块按先序叠放、居中对齐；仅内置页 / `data-blk` 覆盖路径使用，`kernel_v25/src/gui/layout.c::layout_stack` (L186) |

### 4.2 容器流与对齐 → `layout` / `align_*`（M-4.2）

| 规则 ID | CSS | 中间层字段 | 内核枚举 / 取值 | 依据 |
|---|---|---|---|---|
| M-4.2-01 | `display:flex; flex-direction:row` | `layout.flow="row"`，`kind=ROW` | — | `kernel_v25/src/gui/layout.c::layout_row` (L156) |
| M-4.2-02 | `display:flex; flex-direction:column` | `layout.flow="column"`，`kind=COLUMN` | — | `kernel_v25/src/gui/layout.c::layout_column` (L126) |
| M-4.2-03 | `display:none` | 不生成节点；记入 diagnostics | — | 受控子集语法 §4.8 |
| M-4.2-04 | `justify-content:flex-start / center / flex-end / space-between` | `layout.align_main` | `ALN_START=0 / ALN_CENTER=1 / ALN_END=2 / ALN_SPACE_BETWEEN=3` | `kernel_v25/src/gui/layout.c::layout_column` (L137)；`kernel_v25/src/gui/layout.c::layout_row` (L167) |
| M-4.2-05 | `align-items:flex-start / center / flex-end / stretch` | `layout.align_cross` | `XALN_START=0 / XALN_CENTER=1 / XALN_END=2 / XALN_STRETCH=3` | `kernel_v25/src/gui/layout.c::place_child` (L115) |
| M-4.2-06 | `gap: <px>` | `layout.gap` | 档位 `0/4/8/12/16/24`（§4.3 白名单） | `kernel_v25/src/gui/layout.c::measure_node` (L79) |

### 4.3 尺寸、外边距、内边距（M-4.3）

| 规则 ID | CSS | 中间层字段 | 内核字段 | 依据 |
|---|---|---|---|---|
| M-4.3-01 | `padding: <px>` | `padding`（int 或 `{t,r,b,l}`） | `pad_t/pad_r/pad_b/pad_l` | `kernel_v25/src/gui/layout.c::layout_node` (L263)；档位 §4.3 |
| M-4.3-02 | `padding-top/right/bottom/left` | `padding.{t,r,b,l}` | 同上 | 同上 |
| M-4.3-03 | `margin: <px>` | `margin`（int 或 `{t,r,b,l}`） | `m_t/m_r/m_b/m_l` | `kernel_v25/src/gui/layout.c::layout_free_child` (L208)；档位 §4.3 |
| M-4.3-04 | `width: <px>` | `req.w` | `req_w` | `kernel_v25/src/gui/layout.c::measure_node` (L91) |
| M-4.3-05 | `height: <px>` | `req.h` | `req_h` | 同上 |
| M-4.3-06 | `width:auto` / `height:auto` | `req.w/h = -1` | `-1 = auto` | `kernel_v25/include/gui/block.h::blk_spec` |
| M-4.3-07 | `width:100%` | `pct.w = 1000` | `pct_w`（1/1000 父框） | `kernel_v25/src/gui/layout.c::layout_free_child` (L215) |
| M-4.3-08 | `height:100%` | `pct.h = 1000` | `pct_h` | `kernel_v25/src/gui/layout.c::layout_free_child` (L217) |
| M-4.3-09 | `min-width / min-height` | `min_max.min_w / min_h` | `min_w / min_h`（0 = 不设） | `kernel_v25/src/gui/layout.c::layout_free_child` (L221) |
| M-4.3-10 | `max-width / max-height` | `min_max.max_w / max_h` | `max_w / max_h`（0 = 不设） | `kernel_v25/src/gui/layout.c::layout_free_child` (L223) |

百分比换算：`pct = round(子尺寸 / 父尺寸 * 1000)`；内核求解是整数运算 `aw * pct_w / 1000`，千分位取整误差按 §7.2 有损表登记。

#### 4.3.1 自然尺寸（`auto` 回落口径）

`req_w/req_h = -1` 时，叶子块自然尺寸由 `kernel_v25/src/gui/layout.c::leaf_natural_w` / `leaf_natural_h` 算出；生成器在中间层写 `req=-1` 即按此口径，不自行估算叶子宽高。

| 叶子 kind | 自然宽 | 自然高 | 依据 |
|---|---|---|---|
| `TEXT` / `BUTTON` | `8 * scale * 文本长度` | `16 * scale`；calm 档非文本控件抬高到 24 | `kernel_v25/src/gui/layout.c::leaf_natural_w` (L21)；`leaf_natural_h` (L44) |
| `INPUT` | 80 | `16 * scale`；calm 档抬高到 24 | 同上 |
| `IMAGE` / `SURFACE` / `CANVAS` | `pix_w` | `pix_h` | `kernel_v25/src/gui/layout.c::leaf_natural_w` (L26) |
| `SPACER` | `req_w` | `req_h` | `kernel_v25/src/gui/layout.c::leaf_natural_w` (L29) |
| `DIVIDER` | 0 | 1 | `kernel_v25/src/gui/layout.c::leaf_natural_h` (L58) |

容器自然尺寸：COLUMN 宽 = padding + 子块最大宽、高 = padding + 子块高之和 + gap；ROW 宽 = padding + 子块宽之和 + gap、高 = padding + 子块最大高；STACK 取最大宽 / 最大高（`kernel_v25/src/gui/layout.c::measure_node` (L71)）。

#### 4.3.2 空间档白名单与 8pt 量化口径

| 类别 | 允许档位（px） | 依据 |
|---|---|---|
| padding | `0 / 2 / 4 / 8 / 12 / 16 / 24` | 生成器 / 产物约束 G2；内核 `grid_snap` 仅在 calm 档归整到 `{4,8,12,16,24}`，见 `kernel_v25/src/gui/layout.c::grid_snap` (L35) |
| margin | `0 / 2 / 4 / 8 / 12 / 16 / 24` | 同上 |
| gap | `0 / 4 / 8 / 12 / 16 / 24` | 同上 |
| 坐标 / 尺寸量化 | 必须是 8 的倍数 | 产物约束 G1；**内核 `grid_snap` 不做坐标对齐**，只归整 padding/gap，见 `kernel_v25/src/gui/layout.c::grid_snap` (L35) |

### 4.4 定位：`position:absolute` → `FREE` + `anchor` + `px` / `pct`（M-4.4）

| 规则 ID | CSS | 中间层字段 | 内核字段 | 依据 |
|---|---|---|---|---|
| M-4.4-01 | `position:absolute` | 父容器 `kind=FREE`；子块 `layout.flow="free"` | `BLK_FREE` | `kernel_v25/include/gui/block.h::blk_kind`；`kernel_v25/src/gui/layout.c::layout_free` (L250) |
| M-4.4-02 | `position:relative` / `static` | 留在流式容器，不生成 `anchor` | — | 受控子集位置白名单 |
| M-4.4-03 | `left: <px>` | `px.x`；`anchor` 含 `L`（默认左锚） | `px_x` / `ANC_L=0x1` | `kernel_v25/src/gui/layout.c::layout_free_child` (L233) |
| M-4.4-04 | `top: <px>` | `px.y`；`anchor` 含 `T`（默认上锚） | `px_y` / `ANC_T=0x4` | `kernel_v25/src/gui/layout.c::layout_free_child` (L239) |
| M-4.4-05 | `right: <px>` | `anchor` 含 `R`；`px.x` 为右锚回退偏移 | `ANC_R=0x2` / `px_x` | `kernel_v25/src/gui/layout.c::layout_free_child` (L230) |
| M-4.4-06 | `bottom: <px>` | `anchor` 含 `B`；`px.y` 为下锚回退偏移 | `ANC_B=0x8` / `px_y` | `kernel_v25/src/gui/layout.c::layout_free_child` (L236) |
| M-4.4-07 | 水平居中 | `anchor` 含 `HCENTER` | `ANC_HCENTER=0x10` | `kernel_v25/src/gui/layout.c::layout_free_child` (L231) |
| M-4.4-08 | 垂直居中 | `anchor` 含 `VCENTER` | `ANC_VCENTER=0x20` | `kernel_v25/src/gui/layout.c::layout_free_child` (L237) |
| M-4.4-09 | `left: <%>` | `pct.x`（1/1000） | `pct_x` | `kernel_v25/src/gui/layout.c::layout_free_child` (L234) |
| M-4.4-10 | `top: <%>` | `pct.y` | `pct_y` | `kernel_v25/src/gui/layout.c::layout_free_child` (L240) |

锚点 / 百分比 / 外边距 / min-max 的求解顺序固定：父框扣外边距 → 锚点 / 百分比落基准点 → min/max 夹尺寸（不回写坐标）→ 落矩形；依据 `kernel_v25/src/gui/layout.c::layout_free_child` (L207)。布局阶段不 clamp 坐标，越界与否由溢出策略在绘制 / 命中层对齐。

### 4.5 溢出与滚动（M-4.5）

| 规则 ID | CSS | 中间层字段 | 内核枚举 | 依据 |
|---|---|---|---|---|
| M-4.5-01 | `overflow:hidden` | `overflow="clip"` | `OVF_CLIP=0`（默认） | `kernel_v25/include/gui/block.h`；`kernel_v25/src/gui/paint.c::paint_node` (L414) |
| M-4.5-02 | `overflow:visible` | `overflow="visible"` | `OVF_VISIBLE=1` | `kernel_v25/src/gui/paint.c::paint_node` (L420) |
| M-4.5-03 | `overflow:auto` / `overflow:scroll` | `overflow="scroll"` | `OVF_SCROLL=2` | `kernel_v25/src/gui/paint.c::paint_node` (L416) |
| M-4.5-04 | 滚动偏移 | `scroll.{x,y}`（仅 `overflow=scroll` 有意义） | `scroll_x / scroll_y` | `kernel_v25/include/gui/block.h::block`；断言 O1 |

`OVF_SCROLL` 只做视口平移，不改子块几何、不重排子树；内核不提供滚动条控件。

### 4.6 描边、圆角、阴影（M-4.6）

| 规则 ID | CSS | 中间层字段 | 内核字段 / 行为 | 依据 |
|---|---|---|---|---|
| M-4.6-01 | `border: 1px solid <color>` | `stroke.w=1`，`stroke.sides=[L,R,T,B]`，`stroke.color` 角色名 | `st.border_w`（0/1/2）；描边色取活动令牌 `border` 角色 | `kernel_v25/include/gui/tokens.h::style`；`kernel_v25/src/gui/paint.c::body_border` (L270) |
| M-4.6-02 | `border-width: 2` | `stroke.w=2` | `border_w=2` 画内缩 1px 同色第二层 | `kernel_v25/src/gui/paint.c::gui_draw_border` (L129) |
| M-4.6-03 | `border-width: 0` | `stroke.w=0` | 不画描边 | `kernel_v25/src/gui/paint.c::body_border` (L271) |
| M-4.6-04 | `border-radius: <px>` | `radius.{tl,tr,br,bl}`，档值 `0/8/12/16/24` | 内核仅 calm 尺寸阶梯，无逐角半径（§5） | `kernel_v25/src/gui/paint.c::calm_r_index` (L234) |
| M-4.6-05 | `box-shadow: contact / dual / none` | `shadow.mode` | 内核按 layer 定单层 / 双层，不由该字段选；WORK 单层接触层，OVERLAY / TOAST 双层 | `kernel_v25/src/gui/paint.c::paint_halo` (L296) |

逐边 `stroke.sides`、逐角 `radius`、`shadow.sides` 是设计态字段：基线 `blk_spec` 无对应逐边 / 逐角成员，内核绘制路径不读它们（§3 缺口 1、2）。

### 4.7 颜色、字体、层级（M-4.7）

| 规则 ID | CSS | 中间层字段 | 内核字段 | 依据 |
|---|---|---|---|---|
| M-4.7-01 | `background` / `background-color: <角色>` | `fill` | `st.bg` / `role_bg` | `kernel_v25/src/gui/paint.c::resolve_color` (L161) |
| M-4.7-02 | `color: <角色>` | `fg` | `st.fg` / `role_fg` | `kernel_v25/src/gui/paint.c::resolve_color` (L157) |
| M-4.7-03 | accent 色 | `accent` | `st.accent` / `role_accent` | `kernel_v25/src/gui/paint.c::resolve_color` (L165) |
| M-4.7-04 | `font-size: 12px` | `font.scale=1` | `st.scale=1` | `kernel_v25/src/gui/paint.c::gui_draw_text` (L96) |
| M-4.7-05 | `font-size: 16px` | `font.scale=2` | `st.scale=2`，字形与步距各乘 2 | `kernel_v25/src/gui/paint.c::gui_draw_text` (L107) |
| M-4.7-06 | `z-index: <int>` | `z` | `z`；绘制按 layer 升序 → z 升序 → 树先序 | `kernel_v25/src/gui/compose.c`（排序路径） |
| M-4.7-07 | `line-height: 16`（scale1）/ `32`（scale2） | 允许；其余报不支持 | 文本行高固定 `16*scale` | `kernel_v25/src/gui/layout.c::leaf_natural_h` (L47) |

#### 4.7.1 令牌角色表（颜色映射唯一依据）

角色枚举见 `kernel_v25/include/gui/tokens.h::gui_role`；三套令牌取值逐字取自 `kernel_v25/src/gui/tokens.c`（`tok_dark / tok_light / tok_hc`）。中间层 `fill / fg / accent / stroke.color` 只允许取下列角色名或 `#RRGGBB` 显式色。

| 角色名 | 枚举 | dark | light | hc |
|---|---|---|---|---|
| `fg` | `ROLE_FG=1` | `E8E8EC` | `181820` | `FFFFFF` |
| `bg` | `ROLE_BG=2` | `1E1E26` | `ECECF1` | `000000` |
| `panel` | `ROLE_PANEL=3` | `2A2A34` | `DDDDE6` | `101010` |
| `bar` | `ROLE_BAR=4` | `3A3A48` | `C8C8D6` | `202020` |
| `accent` | `ROLE_ACCENT=5` | `4C8DFF` | `2563D8` | `FFFF00` |
| `border` | `ROLE_BORDER=6` | `555A66` | `8A8F9E` | `FFFF00` |
| `line` | `ROLE_LINE=7` | `555A66` | `8A8F9E` | `FFFF00` |

显式 `#RRGGBB` 走 `st.fg/bg/accent` 固化色；角色生效时对应固化色清零。主题切换时角色色跟随活动令牌重解析，固化色不跟随。

#### 4.7.2 文本度量与字模

- 字模唯一来源：`kernel_v25/src/font_ascii.inc`（8×16 ASCII 字模，16 行字节 / 字形，MSB 为最左像素；95 个可打印字形 `0x20..0x7E`）。禁止用系统字体近似。
- 度量口径与 `kernel_v25/src/gui/decl.c::decl_text_width` (L632) 一致：ASCII 8px；双字节（`0x81..0xFE`）16px；`scale=2` 各乘 2。
- 截断策略：`BLK_TEXT` 单行不换行；超宽按 `ellipsis` 截断到最后一个能放下的字并追加 `...`；不足时直接 `clip`。度量与截断路径见 `kernel_v25/src/gui/paint.c::text_trimmed` (L174)。

### 4.8 受控子集语法

#### 4.8.1 允许标签

`div / span / button / input(type=text) / img / hr / br`。其余标签进入 §7 丙类。

#### 4.8.2 允许 CSS 属性白名单

`display / flex-direction / justify-content / align-items / gap / padding(-*) / margin(-*) / width / height / position / left / top / right / bottom / background(-color) / color / border(-width/-color) / border-radius / box-shadow / overflow / font-size / opacity(仅 1) / line-height / z-index`。白名单之外的属性进入 §7 判定。

#### 4.8.3 `data-blk-*` 覆盖属性

HTML 元素上的 `data-blk-*` 属性是样式白名单之外唯一的显式覆盖入口：

| 属性 | 覆盖目标 | 中间层字段 |
|---|---|---|
| `data-blk-layer` | 层 | `layer`（CE 投影仍只允许 `work`，断言 L1） |
| `data-blk-anchor` | 锚点位 | `anchor`（仅 FREE 子块） |
| `data-blk-overflow` | 溢出三态 | `overflow` |
| `data-blk-drag` | 拖拽模式 | `drag`（`none/title/full`） |
| `data-blk-drag-bounds` | 拖拽边界 | `drag_bounds`（`free/parent/screen`） |
| `data-blk-resize` | 缩放方向 | `resize.dir` |
| `data-blk-resize-edge` | 缩放边缘带 px | `resize.edge` |
| `data-blk-bleed` | 全出血 | `bleed`（`true/false`） |
| `data-blk-role` | 着色角色 | `fill / fg / accent` 之一 |
| `data-blk-z` | 层内序 | `z` |
| `data-blk-text-overflow` | 截断策略 | `text_overflow`（`ellipsis/clip`） |

`class` 只能命中 HTML 内联 `<style>` 里定义的受限规则；未知 class 记 `UNSUPPORTED_CLASS`。`style` 属性只接受 §4.8.2 白名单属性。

### 4.9 `.blk.json` 节点字段全表（规范定义）

> 本表是中间层节点字段的权威定义；参考实现内的字段副本可弃。类型记法：`int` 整数 px；`enum` 取值域见 §4.1、§4.2、§4.5；`role` 取值域见 §4.7.1。

| 字段 | 类型 | 必填 | 默认值 | 取值域 / 说明 |
|---|---|---|---|---|
| `id` | int | 是 | — | 树内唯一；根节点 `id=0`，其余 `>=1` |
| `parent` | int | 否 | 由 `children` 反向填充 | 禁止手写与 `children` 冲突的值 |
| `kind` | enum | 是 | — | 见 §4.1 枚举表 |
| `layer` | enum | 否 | `work` | `bg / content / work / chrome / overlay / toast`；CE 投影只允许 `work` |
| `rect` | `{x,y,w,h}` | 是 | — | px；生成器产出的 8pt 量化绝对几何；断言 G1 / B1 / B2 |
| `req` | `{w,h}` | 否 | `{-1,-1}` | `-1 = auto` |
| `layout` | `{flow,gap,align_main,align_cross}` | 否 | — | `flow: column/row/stack/free`；`gap` 档位 §4.3.2；对齐枚举 §4.2 |
| `padding` | int 或 `{t,r,b,l}` | 否 | 0 | 档位 §4.3.2 |
| `anchor` | 字符串列表 | 否 | `[]` | 仅 FREE 子块；`L/R/T/B/HCENTER/VCENTER` |
| `px` | `{x,y}` | 否 | `{0,0}` | 锚点像素偏移 |
| `pct` | `{x,y,w,h}` | 否 | `{0,0,0,0}` | 1/1000 父框；0 = 未声明 |
| `min_max` | `{min_w,min_h,max_w,max_h}` | 否 | 0 | 0 = 不设 |
| `margin` | int 或 `{t,r,b,l}` | 否 | 0 | 档位 §4.3.2 |
| `overflow` | enum | 否 | `clip` | `clip/visible/scroll` |
| `scroll` | `{x,y}` | 否 | `{0,0}` | 仅 `overflow=scroll` 有意义 |
| `drag` | enum | 否 | `none` | `none/title/full` |
| `drag_bounds` | enum | 否 | `free` | `free/parent/screen` |
| `resize` | `{dir,edge,min_w,min_h,max_w,max_h,step}` | 否 | — | `dir` 位掩码 `L/R/T/B`；`edge` 默认 6；min 默认 24；max/step 0 = 不设 / 连续 |
| `bleed` | bool | 否 | `false` | `true` = 忽略父 padding 贴父外框 |
| `visible` | bool | 否 | `true` | 跳过绘制 |
| `focusable` | bool | 否 | `false` | 接管键盘 |
| `inert` | bool | 否 | `false` | 跳过命中 |
| `z` | int | 否 | 0 | 层内相对序 |
| `fill` | role 或 `#RRGGBB` | 否 | `bg` | §4.7.1 |
| `fg` | role 或 `#RRGGBB` | 否 | `fg` | §4.7.1 |
| `accent` | role 或 `#RRGGBB` | 否 | `accent` | §4.7.1 |
| `stroke` | `{w,sides,color}` | 否 | — | `w:0/1/2`；`sides:[L,R,T,B]`；`color` role |
| `radius` | `{tl,tr,br,bl}` | 否 | — | px，档 `0/8/12/16/24`；设计态逐角声明 |
| `radius_mode` | enum | 否 | `size_ladder` | `per_corner/size_ladder`；`per_corner` 只在 design 预览生效 |
| `shadow` | `{mode,sides}` | 否 | — | `mode:none/contact/dual`；`sides:[L,R,T,B]`，仅 design 态生效 |
| `text` | string | 否 | — | 单行 |
| `font` | `{scale}` | 否 | `{scale:1}` | `scale:1/2` |
| `text_overflow` | enum | 否 | `ellipsis` | `ellipsis/clip` |
| `pix` | `{path,w,h,res}` | 否 | — | 图像块；`res` 为槽 74 句柄 |
| `ce` | 记录九字段镜像 | 是 | — | 见 §1.2；产物生成阶段可写回 `text_off` |
| `children` | node[] | 否 | — | 子块树 |

### 4.10 枚举值与内核常量对应

| 中间层枚举 | 取值 | 内核常量 | 定义位置 |
|---|---|---|---|
| `kind` | `root/column/row/stack/text/rect/image/surface/spacer/divider/canvas/free/button/input/term` | `BLK_ROOT … BLK_TERM`（0…14） | `kernel_v25/include/gui/block.h::blk_kind` |
| `layer` | `bg/content/work/chrome/overlay/toast` | `LAYER_BG=0 / LAYER_CONTENT=100 / LAYER_WORK=200 / LAYER_CHROME=3000 / LAYER_OVERLAY=4000 / LAYER_TOAST=5000` | `kernel_v25/include/gui/block.h` |
| `overflow` | `clip/visible/scroll` | `OVF_CLIP=0 / OVF_VISIBLE=1 / OVF_SCROLL=2` | `kernel_v25/include/gui/block.h` |
| `anchor` | `L/R/T/B/HCENTER/VCENTER` | `ANC_L=0x1 / ANC_R=0x2 / ANC_T=0x4 / ANC_B=0x8 / ANC_HCENTER=0x10 / ANC_VCENTER=0x20` | `kernel_v25/include/gui/block.h` |
| `align_main` | `start/center/end/space_between` | `ALN_START=0 / ALN_CENTER=1 / ALN_END=2 / ALN_SPACE_BETWEEN=3` | `kernel_v25/include/gui/block.h` |
| `align_cross` | `start/center/end/stretch` | `XALN_START=0 / XALN_CENTER=1 / XALN_END=2 / XALN_STRETCH=3` | `kernel_v25/include/gui/block.h` |

### 4.11 布局求解规则

- **两阶段**：measure 自底向上算 auto 尺寸；layout 自顶向下写绝对 `x/y`。只重算带 `dirty_layout` 标记的子树。入口 `kernel_v25/src/gui/layout.c::layout_run` (L316)。
- **COLUMN（纵主轴）**：主轴纵向，交叉轴横向；子块纵向依次落位，`align_main` 控制纵向起 / 中 / 止 / 均分，`align_cross` 控制横向起 / 中 / 止 / 拉伸。见 `kernel_v25/src/gui/layout.c::layout_column` (L126)。
- **ROW（横主轴）**：主轴横向，交叉轴纵向；见 `kernel_v25/src/gui/layout.c::layout_row` (L156)。
- **STACK**：子块层叠，交叉轴居中；见 `kernel_v25/src/gui/layout.c::layout_stack` (L186)。
- **FREE**：子块不参与父容器主轴流动；按 §4.4 顺序求解。`bleed` 子块对齐父整框，其余对齐父内容框，见 `kernel_v25/src/gui/layout.c::child_box` (L101)。
- **`grid_snap`**：只在 calm 档生效，归整 padding/gap 到 `{4,8,12,16,24}`；不做坐标 8pt 对齐，1px 描边不动。见 `kernel_v25/src/gui/layout.c::grid_snap` (L35)。
- **min/max**：只夹尺寸，不回写坐标；越界交溢出策略处理。

---

## 5. 贴边方角与圆角覆盖率（规则 + 分界）

### 5.1 贴边判定（设计层规则）

同一父块内，把子块 `rect` 与父块内容矩形（父 `rect` 去掉 padding 后）按 8pt 量化后逐边比较；某条边坐标完全相等即判该侧贴边（左右比 `x / x+w`，上下比 `y / y+h`）。

- 比较对象：父内容框，由 `kernel_v25/src/gui/layout.c::layout_node` (L265) 按父 padding 算出；`bleed` 子块对齐父整框，不参与内容框贴边判定。
- 量化口径：坐标 / 尺寸必须是 8 的倍数（G1）。
- 判定细则引用 `块化GUI-设计规格.md §14.1 / §14.2 / §14.3 / §14.5`，本文不复制其正文。

### 5.2 四类设计态效果

| # | 效果 | 设计态落点 | 断言 |
|---|---|---|---|
| 1 | 该侧两个角的 `radius` 归零 | `radius` 对应角置 0 | R1 |
| 2 | 该侧 `stroke` 省略（不画父与子的双线） | `stroke.sides` 移除该侧 | S1 |
| 3 | 该侧 `shadow` 抑制 | `shadow.sides` 移除该侧 | S2 |
| 4 | 父子共享边只保留 1px 分隔线，颜色 `role=line` | 共享边由分隔线承担，父子不在该侧各画描边 | D1 |

### 5.3 基线内核未实现这套规则（明确分界）

以下三条是基线源码事实，生成器与预览不得把设计态规则当成内核行为：

1. **阴影固定四边，没有按边抑制**。`kernel_v25/src/gui/paint.c::paint_halo` 对整块调用 `gui_draw_border` 画四边：接触层 `b->x, b->y+1, b->w, b->h`；双层投影 `b->y+3` 与 `b->y+4`。代码路径不存在按边抑制的分支。
2. **角部覆写只处理四个角，没有 per-side 逻辑**。`kernel_v25/src/gui/paint.c::corner_restore` (L317) 按上 / 下各 `r` 行覆写左右两列；`kernel_v25/src/gui/paint.c::fill_round` (L250) 的掩码 inset 只按行算左右两边。没有「某一条边保持直角、其余边圆角」的计算。
3. **没有父子共享边合成逻辑**。父描边与子描边各走 `gui_draw_border` 各画各的；不存在「共享边只留 1px `line` 色分隔线」的合并 pass。`BLK_DIVIDER` 是独立节点，画 1px `line` 角色色横线，不是父子描边的合成产物。

### 5.4 口径分界表

| 事项 | 设计态（中间层 / design 预览） | 内核态（基线） |
|---|---|---|
| 贴边侧圆角归零 | R1 可表达、可断言 | 无逐角半径，谈不上按侧归零（§5.5） |
| 贴边侧省略描边 | `stroke.sides` 可声明 | `border_w` 只支持 0/1/2 整框描边；无逐边描边字段 |
| 贴边侧抑制阴影 | `shadow.sides` 可声明 | `paint_halo` 固定四边（§5.3-1） |
| 共享边 1px 分隔线 | D1 可断言 | 无合成；需要分隔线时显式放 `BLK_DIVIDER` 节点 |

### 5.5 calm 档尺寸阶梯

圆角只在 calm 档位生效，阶梯值 `CALM_RADII = {8, 12, 16, 24}`，按块短边 `s = min(w, h)` 分档，见 `kernel_v25/src/gui/paint.c::calm_r_index` (L234)：

| 短边 `s` | 档索引 | 半径 | 用途 |
|---|---|---|---|
| `s < 32` | 0 | 8 | 小控件 |
| `32 <= s < 64` | 1 | 12 | 按钮 / 图标卡 |
| `64 <= s < 128` | 2 | 16 | 面板 / 弹出层外框 |
| `s >= 128` | 3 | 24 | 应用图标 / 大型欢迎卡 |

掩码在 `kernel_v25/src/gui/paint.c::calm_masks_init` (L204) 按档预生成：角象限内 `u = r-dx`、`v = r-dy`，判 `u⁵ + v⁵ <= r⁵`；每档 `r` 对应一张 `24×24` 掩码表；绘制按行查 `kernel_v25/src/gui/paint.c::calm_inset` (L242) 得左右 inset，无逐像素浮点。若 `r*2 > w` 或 `r*2 > h`，退回整块直角矩形填充。

### 5.6 `corner_mode` 与可绘制排除条件

`struct style.corner_mode`：`0` 继承模式 / `1` 强制直角 / `2` 强制圆角（`kernel_v25/include/gui/tokens.h::style`）。`kernel_v25/src/gui/paint.c::calm_shape_eligible` (L226) 排除条件：

| # | 排除条件 | 依据 |
|---|---|---|
| 1 | 非 calm 档（`gui_style_calm()` 为 0） | `kernel_v25/src/gui/paint.c::calm_on` (L220)；档位读取 `kernel_v25/src/gui/settings.c::gui_style_calm` (L47) |
| 2 | 高对比度令牌（`TOK_HIGH_CONTRAST`） | `kernel_v25/src/gui/paint.c::calm_on` (L222) |
| 3 | `BLK_ROOT` | `kernel_v25/src/gui/paint.c::calm_shape_eligible` (L228) |
| 4 | `bleed` 全出血块 | 同上 |
| 5 | `LAYER_CHROME` / `LAYER_BG` | `kernel_v25/src/gui/paint.c::calm_shape_eligible` (L229) |
| 6 | `corner_mode == 1` | `kernel_v25/src/gui/paint.c::calm_shape_eligible` (L230) |

`corner_mode==2` 在基线源码中未见额外分支消费（未核实；核实方法：在 `kernel_v25/src/gui/paint.c` 绘制路径检索 `corner_mode` 非 1 的分支）。

### 5.7 设计态逐角半径为何在内核里退化成阶梯

1. `blk_spec` / `block` 没有逐角半径字段。`struct style` 仅保留整块 `corner_radius`（0 = 按尺寸走阶梯；>0 固定角档 px）与 `corner_mode`；全仓检索 `corner_radius` 在 `kernel_v25/include/gui/tokens.h` 之外无消费者。
2. 绘制路径对整块只取一个档索引 `ri = calm_r_index(w, h)`，`fill_round` 四个角共用同一 `r` 与同一张掩码；没有 tl/tr/br/bl 分别取半径的分支。
3. 因此中间层的 `radius.{tl,tr,br,bl}` 只在 preview `design` 口径按逐角值绘制；落到内核内置页时，逐角值被 `min(w,h)` 阶梯取代，`radius_mode=per_corner` 仅 design 生效。

### 5.8 与 v1 砍项口径的关系

v1 默认态：`kernel_v25/include/gui/tokens.h` 预留字段 `radius / elevation / anim_ms` 恒 0（直角、无阴影、无动画），三套令牌取值均置 0。calm 是可选档位：置 calm 后才画超椭圆圆角与 halo 阴影。口径结论：**v1 默认态无圆角无阴影；calm 可选态有阶梯圆角与层决定的双层阴影**。写 UI 时不要把 calm 效果当成 app 口径的保证。

---

## 6. 三条预览口径

### 6.1 口径对照表

| 模式 | 画什么 | 对应内核状态 | 内核依据 |
|---|---|---|---|
| `app`（默认） | 直角 + 1px 描边 + 无阴影 | `/etc/style` 非 calm；`calm_shape_eligible` 全 false | `kernel_v25/src/gui/paint.c::calm_shape_eligible` (L226)；`kernel_v25/src/gui/settings.c::gui_style_calm` (L47) |
| `calm` | 按块短边阶梯取圆角 8/12/16/24（超椭圆 n≈5）+ `paint_halo`：WORK 单层接触层；OVERLAY / TOAST 双层 | `/etc/style` 含 `calm`，且非高对比度令牌 | `kernel_v25/src/gui/paint.c::calm_r_index` (L234)；`kernel_v25/src/gui/paint.c::paint_halo` (L296) |
| `design` | 按中间层逐角 `radius` 与 `shadow.sides` 渲染，并执行贴边规则 | **目标态，内核暂不具备** | 缺口见 §3、§5 |

### 6.2 对照纪律

- QEMU 截图对照只允许**同口径**比：`app` ↔ 内核 `style=app`；`calm` ↔ 内核 `style=calm`。不允许拿 calm 预览图对照 app 内核截图。
- `design` 口径只做「设计 vs 内核」差异清单（逐角半径、逐边描边 / 阴影、共享边分隔线），不冒充同布局、不做像素对齐验收。
- 预览配色逐字使用 §4.7.1 令牌表，禁止自拟配色；字模必须解析 `kernel_v25/src/font_ascii.inc`，不许用系统字体近似。

---

## 7. 不支持项三态判定（U-*）

### 7.1 三态定义与处理策略

| 态 | 定义 | 处理 |
|---|---|---|
| 甲 内核结构性不可表达 | 基线 ABI / 绘制路径无对应能力，必须等 §3 字段演进 | 每条点名依赖的 §3 字段或 §14 条款；当前版本不得进入产物 |
| 乙 本工具暂不支持 | 内核可表达或可通过既有字段迂回，参考实现尚未实现 | 给扩展环节与工作量估计；可在后续版本补齐 |
| 丙 永久不支持 | 语义与块模型不匹配，不规划字段演进 | 给替代写法（可抄的 HTML / 中间层片段） |

诊断策略：命中不支持项一律以 `UNSUPPORTED <selector> <property>` 记入 diagnostics 并以非 0 退出；仅 `--allow-lossy` 才降级为 warning。**禁止静默丢弃。** 诊断信息只引用 U-* 编号，不引用文件行号。

### 7.2 三态分表

#### 7.2.1 甲：内核结构性不可表达

| ID | 项 | 判定理由 | 依赖（字段 / 条款） | 验收 |
|---|---|---|---|---|
| U-7.2-01 | `border-radius` 逐角半径 / 非阶梯半径 | 绘制路径四角共用同一档半径与掩码，无 tl/tr/br/bl 分别取值分支；贴边角无法独立归零 | 等 §3.3 / §3.4 `corner_radius[4]`；判定见 `块化GUI-设计规格.md §14.1` | R1；逐角半径落地后贴边角为 0 |
| U-7.2-02 | `box-shadow` 侧别（贴边侧抑制投影） | `paint_halo` 固定四边画 halo，无按边抑制分支；接缝处无法取消投影 | 等与 `border_mask` 同批的侧别掩码（`shadow_mask`，字段名待 gui-fix 定稿）；见 `块化GUI-设计规格.md §14.3` | S2；贴边侧无阴影暗带 |
| U-7.2-03 | `opacity < 1`（半透明） | 绘制为不透明 RGB24 直填，无 alpha 混合通道；颜色为 `0x00RRGGBB` 不透明值 | 等渲染合成层 alpha 通道；字段待 gui-fix 定稿（§3 未列定） | 落地前仅 `opacity:1` 允许 |
| U-7.2-04 | 多行文本自动折行 | `BLK_TEXT` 单行不换行，无折行布局路径；超宽只做 ellipsis / clip | 等文本折行字段（候选 `text_wrap`，字段名待定稿）+ 布局折行路径 | T1；折行落地前长文本走 ellipsis |
| U-7.2-05 | `transition` / `animation` | `anim_ms` 预留字段恒 0，帧循环无过渡求解；基线无动画路径 | 等 §3.3 `anim_ms` | `anim_ms=0` 时无中间帧 |
| U-7.2-06 | `position:fixed` / `position:sticky` | 无屏幕锚定与滚动吸附的落位模式；现有 FREE 仅锚定父框 | 等 §3.3 `place`（`float/anchor` 模式） | `fixed` 语义落地前按未核实处理 |
| U-7.2-07 | CE 通道几何 / 样式（FREE/BUTTON/INPUT 提交、颜色、padding/gap/align/anchor） | 记录十二成员不含几何与样式（§2.3）；kind 白名单止于 `BLK_CANVAS`；布局 / 颜色 / 样式落地即丢 | 等 §3.4 `ce_blk_rec2_t` v2 记录块；缺口 3 | P1；v2 扩容前 CE 路线只保树形 / 文本 / 句柄 |

#### 7.2.2 乙：本工具暂不支持（可扩展）

| ID | 项 | 判定理由 | 替代 / 扩展环节 | 工作量估计 | 验收 |
|---|---|---|---|---|---|
| U-7.2-08 | `float` | 块模型有 `BLK_FREE` + anchor 可迂回表达浮动定位；参考实现定位分支未接收 `float` | 扩展 html2blk 定位分支：`float` → `FREE` 子块 + `anchor` 位 | 小：定位规则表加一行 + 分支判定 | 扩展后 `float` 样例几何过 B1/B2 |
| U-7.2-09 | `inline-block` 布局流 | 行内只落 `TEXT` 块；现有流模型不混排行内块 | 扩展行内盒聚合：把相邻行内块聚为 ROW 子树 | 中：解析器行内收集 + 聚合规则 | 扩展后行内块横向依次落位 |
| U-7.2-10 | `text-align` 非 `left` | 文本左起绘制；按钮文字居中是组件行为（`kernel_v25/src/gui/paint.c::paint_node` (L383)），不是段落对齐 | 段落居中需等布局层对齐字段；短期用 `BUTTON` 组件或 FREE 锚定居中替代 | 小（组件替代）；中（段落对齐） | 替代写法过 T1 |
| U-7.2-11 | `%` 类数值精度细化 | 内核求解为整数除法 `aw * pct / 1000`，千分位取整；参考实现取整口径未细化 | 扩展百分比换算与误差标注；不改变内核整数求解 | 小：换算函数 + diagnostics 标注 | 有损表 §7.3-3 误差上界内 |
| U-7.2-12 | `border-radius` 非档值归档 | 内核阶梯档固定 `0/8/12/16/24`；参考实现未把非档值归档到最近档并记录 | 扩展半径归档：就近取档 + diagnostics 标注压缩动作 | 小：档位表 + 归档函数 | 归档后半径落在档内（G2 类比） |

#### 7.2.3 丙：永久不支持

| ID | 项 | 判定理由 | 替代写法 | 验收 |
|---|---|---|---|---|
| U-7.2-13 | `grid` / `display:grid` | 无 Grid 列宽 / 轨道算法；块模型只有三种流式容器 + FREE | 用 ROW/COLUMN 嵌套 + 固定 px 尺寸表达行列 | 替代树过 C1/G1 |
| U-7.2-14 | `transform` / rotate / scale / translate | 无变换路径；块几何为整数矩形，无仿射变换 | 用 FREE + `px`/`pct` 落位表达位移；旋转 / 缩放不支持 | — |
| U-7.2-15 | `filter` / `backdrop-filter` | 无滤镜与背景采样路径 | 用令牌角色色替代滤镜观感；不提供半透明叠加 | — |
| U-7.2-16 | `background-image` / `url()`（作为绘制方式） | 位图只能以 `IMAGE` 节点 + 槽 74 句柄进入；背景填充无图片通道 | 放 `<img>` / `IMAGE` 节点，`pix.path` 经槽 74 载入 | 句柄归属校验过 P1 |
| U-7.2-17 | `gradient` | 无渐变填充；填充为纯色角色 / 固化色 | 用多个 `RECT` 节点分段近似；不承诺渐变连续 | — |
| U-7.2-18 | `font-family` / `font-weight` / `font-style` | 字模唯一来源 `kernel_v25/src/font_ascii.inc`；无族 / 字重 / 斜体概念 | 只用默认字模；强调用 `accent` 色或 `scale=2` | T1 |
| U-7.2-19 | 伪元素 `::before` / `::after` | 无伪节点机制；节点树由显式标签生成 | 在 HTML 里显式加首尾 `span` / `rect` 节点 | C1 |
| U-7.2-20 | `rgba()` / `hsl()` 任何带 alpha 的颜色 | 颜色为不透明 `0x00RRGGBB`；无 alpha 通道 | 取 RGB 分量映射到最近令牌角色或 `#RRGGBB` | 颜色值过 §4.7.1 域 |
| U-7.2-21 | 混合模式 `mix-blend-mode` | 无混合通道；合成按 layer → z → 先序覆盖 | 不支持；用节点层叠顺序表达前后关系 | — |
| U-7.2-22 | `clip-path` / `mask` | 无裁剪路径；裁剪只到矩形父框 | 用矩形 `RECT` 与父框 `overflow=clip` 表达 | B1/B2 |
| U-7.2-23 | `writing-mode` | 仅横排；无竖排排版 | 不支持 | — |
| U-7.2-24 | `direction:rtl` | 无 RTL 双向排版；文本按字模顺序左起绘制 | 不支持；需要镜像布局时人工调换节点顺序 | — |
| U-7.2-25 | 表格 / 列表语义（`table`, `li`） | 无表格 / 列表组件；无语义行列聚合 | 用 ROW 行 + COLUMN 列嵌套表达；列表项用 SPACER 分隔 | G1/G2 |
| U-7.2-26 | `iframe` / `<canvas>` / `video` / `svg` | `BLK_CANVAS` 是内核画布句柄节点，不是 HTML `<canvas>`；无视频 / SVG 解码 | 位图内容走 `IMAGE`；独立画布走槽 69–71 通道 | — |
| U-7.2-27 | 任意 CSS 变量 `var()` | 不解析变量；样式值必须是字面角色名 / 档值 | 把变量展开为字面量后再输入 | — |
| U-7.2-28 | 媒体查询 `@media` | 无响应式断点；布局按固定 800×600 口径 | 不支持；不产出多套断点树 | — |
| U-7.2-29 | `@font-face` | 字模内置且唯一来源 `kernel_v25/src/font_ascii.inc` | 不支持；无外部字模装载通道 | — |

### 7.3 有损但可用：压缩动作与误差上界

下列项不是硬不支持，但落到内核后会被压缩；中间层保留设计态原值，预览 calm / 内置页按压缩后口径呈现，diagnostics 逐条标注压缩动作。

| ID | 设计态原值 | 内核压缩结果 | 允许的误差上界 | 兜底断言 |
|---|---|---|---|---|
| L-7.3-01 | 任意逐角 / 任意 px 圆角 | 退化为 calm 短边阶梯 8/12/16/24；非 calm 档恒直角 | 半径偏差 ≤ 最近阶梯档间距的半档；角部邻域像素差异计入 calm 档残差预算 | R1；§5.5 阶梯表 |
| L-7.3-02 | 双层 / 任意边阴影 | 固定 halo：WORK 单层接触层（黑 18% @ y+1）；OVERLAY / TOAST 双层（黑 18% @ y+1，黑 10% @ y+3/y+4）；无逐边 | 阴影只出现在自由边；贴边侧阴影暗带宽度 0 | S2；`kernel_v25/src/gui/paint.c::paint_halo` (L296) |
| L-7.3-03 | 百分比定位 / 尺寸 | 1/1000 千分位取整；整数除法 `aw * pct / 1000` | 单边取整误差 ≤ 1px | B1/B2 |
| L-7.3-04 | 任意字号 | 只保留 `scale=1`（12px 口径）/ `scale=2`（16px 口径）两档；其它字号报 `UNSUPPORTED_FONT_SIZE` | 档间不插值；字形宽高按档值 | T1 |
| L-7.3-05 | 任意颜色 | 优先映射到令牌角色（fg/bg/panel/bar/border/line/accent）；无法映射时取最近档；带 alpha 颜色不进入此项 | 色差在最近角色档内；固化色不跟随主题 | §4.7.1 表 |

### 7.4 诊断格式与退出码

- 诊断文本固定为 `UNSUPPORTED <selector> <property>`，selector 取命中的元素选择器（标签 + id/class），property 取 CSS 属性名；写入 diagnostics，默认非 0 退出。
- `--allow-lossy`：把 7.2 甲 / 乙项降级为 warning，允许产出降级后的中间层；7.3 的压缩动作仍须逐条标注。未加该标志时，任一甲 / 乙项不得进入产物。
- 诊断通道：默认 stderr；机器可读诊断走 `--json`。所有脚本非 0 退出码表示失败。

诊断示例（格式约定，非内核消息）：

```text
UNSUPPORTED .card transform translate(8px, 0)
UNSUPPORTED #hero background-image url("/bg.png")
UNSUPPORTED p font-weight 700
UNSUPPORTED .fade opacity 0.6
```

### 7.5 静默丢弃禁令

任何未在 §7.2 三态表内、又未在 §7.3 显式记录压缩的声明，不得在转换阶段悄悄省略；必须进入 diagnostics。`--allow-lossy` 只把非 0 退出降级为 warning，不改变「不支持项不进产物」这一口径。

---

## 8. 校验、验收与可复核断言

### 8.1 断言 ID 总表

断言短码沿用既有口径，不改名；spec-audit 编号 `A-9-01..nn`。诊断信息只引用这些 ID，不引用文件行号。

#### 8.1.1 产物 / 树结构断言

| 断言 | 判定方法 | 失败时怎么修 |
|---|---|---|
| **C1** | 遍历节点：`id >= 1`（根 `id=0`）、树内唯一；每个非根 `parent` 指向存在节点；沿父链无环；节点数 ≤ 256；深度 ≤ 16。配额依据 `kernel_v25/include/gui/decl.h`；父链校验 `kernel_v25/src/gui/decl.c::validate` (L197) | 修 id 重复 / 父链断裂 / 超配额；父记录先于子记录出现 |
| **G1** | 每个节点 `rect.x/y/w/h` 均为 8 的倍数。注意：这是生成器 / 产物约束，内核 `grid_snap` 不做坐标对齐（§4.3.2） | 把坐标 / 尺寸归整到 8 的倍数 |
| **G2** | `padding / gap / margin` 取值分别属于 §4.3.2 白名单 | 替换为档内值 |
| **B1** | `overflow=clip` 时子块 `rect` 完全落在父内容矩形内；`visible/scroll` 允许越界，但必须显式声明对应 `overflow` | 收窄子块几何或改 `overflow` |
| **B2** | 子块 `rect` 不得超出父块外框；`visible` 的越界仅允许在内容矩形 → 父外框之间 | 收窄几何或调整 `bleed` |
| **R1** | 贴边侧对应的两个角 `radius` 必须为 0（§5.2 效果 1） | 贴边角半径置 0 |
| **S1** | 贴边侧不得声明描边（§5.2 效果 2） | 从 `stroke.sides` 移除贴边侧 |
| **S2** | 贴边侧不得启用阴影（§5.2 效果 3） | 从 `shadow.sides` 移除贴边侧 |
| **D1** | 贴边共享边只允许 1px 分隔线；父子不得同时在该侧画 2px 以上描边（§5.2 效果 4） | 省略一侧描边；必要时显式放 `DIVIDER` |
| **T1** | 文本按 §4.7.2 度量必须能落在自身内容矩形内；放不下则必须声明 `text_overflow=ellipsis` | 缩短文本 / 放宽几何 / 声明 ellipsis |
| **L1** | `layer` 取值合法（§4.10 表）；走 CE 投影时只允许 `work(200)`，依据 `kernel_v25/src/gui/decl.c::validate` (L179) | 改层值；CE 路线去掉非 work 层 |
| **O1** | `overflow=scroll` 时 `scroll.x/y` 不得超过内容实际溢出范围 | 收窄滚动偏移 |
| **P1** | 产物二进制：`text_off/pix_off` 落在串池内（`0 <= off < slen` 或 `-1`）；`recs` 长度 = `40 * n`（步长 = `sizeof(ce_blk_rec_t)`，`kernel_v25/src/gui/decl.c::decl_commit` (L329)）；`pad0=0`；`pix_off` 恒 `-1`。复核方式：逐条解包十二成员（§1.2 偏移列），校验 `text_off` 指向的 NUL 串长度 ≤ 4096 B、串池总长 ≤ 32768 B；记录顺序与中间层树序一致，父记录先于子记录 | 重算串池偏移 / 步长 / 填充位 |

#### 8.1.2 映射规则与不支持项 ID

| 类别 | 编号区间 | 判定方法 | 失败时怎么修 |
|---|---|---|---|
| 映射规则 | `M-4.1-01` … `M-4.11` | 逐条核对 HTML/CSS 输入到中间层字段的落点是否符合 §4 表 | 按 §4 对应行修正映射分支 |
| 甲类不可表达 | `U-7.2-01` … `U-7.2-07` | 核对依赖字段是否已在 §3 落地；未落地前不得进入产物 | 移除该声明或改用替代写法 |
| 乙类暂不支持 | `U-7.2-08` … `U-7.2-12` | 核对是否已扩展；未扩展前按 warning / 替代写法处理 | 按 §7.2.2 扩展环节补齐 |
| 丙类永久不支持 | `U-7.2-13` … `U-7.2-29` | 核对是否命中丙类语义；命中即拒绝 | 按 §7.2.3 替代写法重写 |
| 有损压缩 | `L-7.3-01` … `L-7.3-05` | 核对压缩动作已在 diagnostics 标注，误差在 §7.3 上界内 | 补标注或收窄设计态值 |

#### 8.1.3 spec-audit 断言（`--spec-audit`）

| 断言 | 判定方法 | 失败时怎么修 |
|---|---|---|
| **A-9-01** | 文档每个 `` `path::symbol` `` 引用能在对应源码文件中按符号名解析到定义或使用；行号仅提示，不判错 | 符号改名时同步更新文档引用；文件路径后缀必须为 `.c/.h/.inc` |
| **A-9-02** | §4.10 枚举值、§4.3.2 档位、§4.7.1 令牌值、§1.7 配额与头文件 / 令牌源码逐字一致 | 同步表值；令牌以 `kernel_v25/src/gui/tokens.c` 为准 |
| **A-9-03** | §7.2.1 甲类每条可回指 §3 字段名或 §14 条款号；对不上的不得归甲类 | 调整三态归类或补登 §3 字段 |
| **A-9-04** | §9 各样例 `recs.bin` 步长 40B/条、`pad0=0`、`pix_off=-1`、串池 NUL 结尾且 ≤ 32768 B | 重算产物二进制 |

### 8.2 spec-audit 语义与 CI 用法

- **语义**：spec-audit 核对文档断言仍能在源码里找到对应符号；行号仅作定位提示，符号名缺失或文件路径错误才判失败。符号名稳定、行号会漂移，不要求文档行号与当前源码逐行对齐。
- **引用格式校验**：反引号内必须为 `` `相对路径::符号名` ``；符号名只允许 `[A-Za-z_][A-Za-z0-9_]*`；禁止嵌套 `foo()::bar` 形式。
- **CI 用法**：在干净 checkout 上跑 `--spec-audit`，期望 RC=0；任一 A-9-* 失败时 CI 红灯。映射 / 断言类校验（C1…P1）在产物上跑，期望全绿退出 0。
- 校验命令形态（脚本属于可弃参考实现，路径以实际实现为准；权威口径以本节断言表为准）：

```bash
# 校验单个中间层 JSON；全部断言通过退出 0，任一失败退出非 0
validate <page.blk.json>

# 机器可读诊断（CI 用）
validate <page.blk.json> --json

# spec-audit：文档符号引用复核，期望 RC=0
validate --spec-audit
```

参考实现若按目录组织，校验脚本位于其工具目录下；脚本缺失时，§8.1.1 / §8.1.3 各断言须按表中判定方法手工或另行实现复核，断言 ID 与判定口径不因实现缺失而改变。

### 8.3 新页面过门 checklist

1. HTML 仅含 §4.8.1 标签与 §4.8.2 白名单属性；§7.2 清单逐项扫描无命中。
2. 树规模：节点 ≤ 256、深度 ≤ 16、单块文本 ≤ 4096 B、串池 ≤ 32768 B（§1.7）。
3. 几何：`rect` 四边 8 的倍数（G1）；padding/gap/margin 取值在档（G2）。
4. 越界：按 B1/B2 核对每个子块；需要越界必须显式 `overflow`。
5. 贴边：父子共享边按 R1/S1/S2/D1 归零 / 省略 / 抑制 / 留 1px 分隔线。
6. 文本：T1 度量通过；超长声明 ellipsis；字号只取 12 / 16 两档。
7. 层：CE 路径只用 `work`；内置页路径层值合法（L1）。
8. 产物：P1 复核串池偏移与记录长度；全部断言全绿后方可进预览 / 内置页构建。

---

## 9. 可执行附录：5 个样例端到端

### 9.0 证据口径与残差归因

**证据基线**：基线 `89584c4`；屏幕档 800×600；主题 dark；空间档仅用白名单；颜色只用令牌角色名。

**已核几何 / 像素证据**：

| 证据组 | 样例 | 几何一致 | 像素一致率（app / calm） |
|---|---|---|---|
| 首轮 fixture（`run/`） | 1 根 + 7 非根 | app 7/7，calm 7/7（容差 0） | app 98.700% / calm 97.228% |
| settings（`final/`） | 18 块（根 + 17 非根） | 18/18（容差 0） | app 97.749% / calm 81.200% |

**残差逐类归因与合格线**：

| 残差类别 | 归因 | 规范是否允许 | 允许到多少 |
|---|---|---|---|
| app 档：光标混帧 | QEMU 帧含箭头光标，预览无光标，计入背景类差异 | 允许 | app 档一致率 ≥ 98.5%（残差 ≤ 1.5%），且差异必须落在字形 / 光标邻域 |
| app 档：字形边缘 | 内核 8×16 位图字体无抗锯齿，与预览字体在边缘像素不重合；整串绘制与 ellipsis 截断口径差 | 允许 | 同上，计入字形邻域残差 |
| calm 档：圆角掩码 | 内核超椭圆离线表（n≈5，24×24 掩码）与预览几何弧在角部像素不重合 | 允许 | calm 档一致率 ≥ 75% 为合格线；角部邻域差异计入本类 |
| calm 档：阴影算法 | halo 固定四边 + 混黑系数（接触层黑 18%，投影层黑 10%），与预览双层 / 逐边模型不一致 | 允许 | 同上 ≥ 75%；贴边侧不允许出现阴影暗带（S2） |
| calm 档：`grid_snap` 抬 padding | `grid_snap` 仅 calm 生效，把 padding/gap 归整到 `{4,8,12,16,24}`，预览静态几何未归整 | 允许 | 单边归整偏差 ≤ 档间距；几何比对以内核求解为准 |

**复现环境**：内核构建 `sh kernel_v25/build.sh iso`（RC=0）；QEMU `qemu-system-i386 -m 256 -display none -boot d -cpu qemu32 -net none -cdrom kernel_v25/catos.iso`，ISO-only。以下命令路径相对仓库根；在干净 checkout 上原样执行。

**流水线五阶段**（各样例 (d) 块逐条执行；参考实现脚本位于 `tools/blkhtml/`）：

1. `html2blk.py`：HTML → `.blk.json`；必须带 `--title <样例名> --theme dark`。`title` 进 `.blk.json` 顶层，缺省则哈希不可复现。
2. `validate.py`：断言复核（C1…P1 全绿，rc=0）。
3. `blk2bin.py`：产出 `.recs.bin / .strings.bin`，`--stats` 输出几何 / 块数统计，`--emit-c` 产出内置页片段。
4. `preview.py`：离线 PNG，`--mode app|calm|design`（§6）。
5. 内核构建 + QEMU 取帧；内置页路径在 shell 内 `gui shot-style-app <样例名>` / `gui shot-style-calm <样例名>`。

**产出哈希锚点**（`.blk.json`，`--title <样例名> --theme dark` 严格复现；`sha256sum` 比对）：

| 样例 | `sha256` |
|---|---|
| settings | `0acdc677f80d775320b6b798e81e81cb81eb49d42cea83efa44bd2df585cc64a` |
| startmenu | `7886a4f1a1f867184379493b533f1f4fb1dadca4d9cbaea7669d591508049a44` |
| notepad | `3bd40cb4614e6f71f570f6305dd930f9bb96bb7da8ab0237c102d9641058f8d2` |
| browser-toolbar | `8137303d838d2b486ac3c208b2f96863ae585ffde10b09fd29bc24eb9b6f39bf` |
| fullbleed | `263add68c522893ecf183da1b90953bf748762772cf476803286b8822e02cff3` |

### 9.1 样例 settings（18 块 · 池 86 · recs 720B）

#### (a) HTML 片段与规则标注

```html
<div class="card" style="width:800px;height:600px;">
  <div class="titlebar" style="display:flex;flex-direction:row;justify-content:space-between;background:bar;padding:8px;width:784px;">
    <span>Settings</span><button class="close">Close</button>
  </div>
  <div class="body" style="display:flex;flex-direction:row;gap:8;padding:8px;">
    <div class="cats" style="display:flex;flex-direction:column;">
      <div class="cat active"><span style="color:accent;">Appearance</span></div><div class="cat"><span>Display</span></div>
    </div>
    <div class="opts" style="display:flex;flex-direction:column;gap:8;">
      <div class="ron"><span style="color:accent;">Dark theme</span></div><div class="roff"><span>Light theme</span></div>
    </div>
  </div>
  <div class="note"><span>Changes apply after restart.</span></div>
</div>
```

触发规则：`M-4.1-01`（row 容器）、`M-4.1-02`（column 容器）、`M-4.1-05`（span→TEXT）、`M-4.1-06`（button）、`M-4.2-04`（space-between）、`M-4.2-06`（gap）、`M-4.3-01`（padding）、`M-4.7-01/02/03`（角色色）。不支持项：无命中。

#### (b) `.blk.json` 关键节点

```json
{
  "id": 1, "kind": "column", "layer": "work",
  "rect": { "x": 0, "y": 0, "w": 800, "h": 600 },
  "req": { "w": 800, "h": 600 }, "fill": "panel",
  "ce": { "id": 1, "parent": 0, "kind": 1, "layer": 200,
          "req_w": 800, "req_h": 600, "text_off": -1, "pix_off": -1,
          "pix_w": 0, "pix_h": 0, "res": -1 },
  "children": [
    { "id": 2, "kind": "row", "parent": 1,
      "rect": { "x": 0, "y": 0, "w": 784, "h": 40 },
      "layout": { "flow": "row", "align_main": "space_between" },
      "padding": { "t": 8, "r": 8, "b": 8, "l": 8 }, "fill": "bar",
      "ce": { "id": 2, "parent": 1, "kind": 2, "layer": 200,
              "req_w": 784, "req_h": -1, "text_off": -1, "pix_off": -1,
              "pix_w": 0, "pix_h": 0, "res": -1 } },
    { "id": 3, "kind": "text", "parent": 2,
      "rect": { "x": 8, "y": 8, "w": 64, "h": 16 }, "text": "Settings",
      "ce": { "id": 3, "parent": 2, "kind": 4, "layer": 200,
              "req_w": -1, "req_h": -1, "text_off": 0, "pix_off": -1,
              "pix_w": 0, "pix_h": 0, "res": -1 } },
    { "id": 4, "kind": "button", "parent": 2,
      "rect": { "x": 712, "y": 8, "w": 64, "h": 24 }, "text": "Close",
      "ce": { "id": 4, "parent": 2, "kind": 12, "layer": 200,
              "req_w": 64, "req_h": 24, "text_off": 9, "pix_off": -1,
              "pix_w": 0, "pix_h": 0, "res": -1 } }
  ]
}
```

#### (c) `recs.bin` 前 5 条记录（每行 40B）

| idx | id | parent | kind | layer | req_w | req_h | text_off | pix_off | res |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 200 | -1 | -1 | -1 | -1 | -1 |
| 1 | 1 | 0 | 1 | 200 | 800 | 600 | -1 | -1 | -1 |
| 2 | 2 | 1 | 2 | 200 | 784 | -1 | -1 | -1 | -1 |
| 3 | 3 | 2 | 4 | 200 | -1 | -1 | 0 | -1 | -1 |
| 4 | 4 | 2 | 12 | 200 | 64 | 24 | 9 | -1 | -1 |

串池：`Settings\0Close\0Appearance\0Display\0Dark theme\0Light theme\0Changes apply after restart.\0`（86 B）。

#### (d) 截图、几何 / 像素数字与复现命令

- 几何：18/18 非根 + 根，容差 0（`final/geom-settings.log`，两档一致）。
- 像素：app 一致率 97.749%（差异 2.2515%）；calm 一致率 81.200%（差异 18.8004%）。calm 残差主类：圆角掩码、阴影算法、`grid_snap` 抬 padding（§9.0）。
- 哈希锚点：`.blk.json` sha256 `0acdc677f80d775320b6b798e81e81cb81eb49d42cea83efa44bd2df585cc64a`。
- 复现（路径相对仓库根）：

```bash
python3 tools/blkhtml/html2blk.py tools/blkhtml/samples/settings.html \
    --out out/settings.blk.json --title settings --theme dark
python3 tools/blkhtml/validate.py out/settings.blk.json
python3 tools/blkhtml/blk2bin.py out/settings.blk.json \
    --out-prefix out/settings --stats --emit-c out/settings.c
python3 tools/blkhtml/preview.py out/settings.blk.json \
    --png out/settings.png --mode app
# calm 口径：--mode calm
sh kernel_v25/build.sh iso
qemu-system-i386 -m 256 -display none -boot d -cpu qemu32 -net none -cdrom kernel_v25/catos.iso
# 内置页路径：shell 内 gui shot-style-app settings / gui shot-style-calm settings
```

### 9.2 样例 startmenu（20 块 · 池 87 · recs 800B）

#### (a) HTML 片段与规则标注

```html
<div style="position:absolute;width:800px;height:600px;padding:0;">
  <div class="menu" data-blk-layer="overlay" data-blk-anchor="R|T"
       style="position:absolute;width:256px;height:480px;right:8px;top:8px;overflow:scroll;">
    <button class="item" style="left:0px;top:0px;">Notepad</button>
    <button class="item" style="left:0px;top:24px;">Canvas demo</button>
    <hr style="width:240px;height:8px;left:0px;top:368px;">
    <button class="user" data-blk-anchor="B">catos-user</button>
  </div>
</div>
```

触发规则：`M-4.1-04`（FREE 父容器）、`M-4.1-06`（button）、`M-4.1-09`（hr→DIVIDER）、`M-4.4-01/05/06`（absolute + R/T 锚）、`M-4.5-03`（scroll）、`M-4.8.3`（`data-blk-layer / data-blk-anchor`）。不支持项：`U-7.2-07`（CE 通道不可还原 overlay 定位）；CE 投影 FAIL=17，属协议缺口。

#### (b) `.blk.json` 关键节点

```json
{
  "id": 2, "kind": "column", "parent": 1, "layer": "overlay",
  "rect": { "x": 536, "y": 8, "w": 256, "h": 480 },
  "anchor": ["R", "T"], "overflow": "scroll",
  "scroll": { "x": 0, "y": 0 },
  "ce": { "id": 2, "parent": 1, "kind": 1, "layer": 4000,
          "req_w": 256, "req_h": 480, "text_off": -1, "pix_off": -1,
          "pix_w": 0, "pix_h": 0, "res": -1 },
  "children": [
    { "id": 3, "kind": "button", "parent": 2,
      "rect": { "x": 0, "y": 0, "w": 240, "h": 24 }, "text": "Notepad" },
    { "id": 4, "kind": "button", "parent": 2,
      "rect": { "x": 0, "y": 24, "w": 240, "h": 24 }, "text": "Canvas demo" }
  ]
}
```

#### (c) `recs.bin` 前 5 条记录（每行 40B）

| idx | id | parent | kind | layer | req_w | req_h | text_off | pix_off | res |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 200 | -1 | -1 | -1 | -1 | -1 |
| 1 | 1 | 0 | 1 | 200 | 800 | 600 | -1 | -1 | -1 |
| 2 | 2 | 1 | 1 | 4000 | 256 | 480 | -1 | -1 | -1 |
| 3 | 3 | 2 | 12 | 200 | 240 | 24 | 0 | -1 | -1 |
| 4 | 4 | 2 | 12 | 200 | 240 | 24 | 8 | -1 | -1 |

串池：`Notepad\0Canvas demo\0Files\0Images\0Browser\0Settings\0Terminal\0Launch demo\0Help\0catos-user\0`（87 B）。

#### (d) 截图、几何 / 像素数字与复现命令

- 裸树校验 `FAIL=0`；CE 投影 `FAIL=17`（free 定位 / anchor / overlay 非槽 72 可还原，`U-7.2-07`）；P1 `FAIL=0`。
- 本样例内核可视帧在基线不可取（§1.8）；验收以提交 verdict 与字段级核对为准。
- 哈希锚点：`.blk.json` sha256 `7886a4f1a1f867184379493b533f1f4fb1dadca4d9cbaea7669d591508049a44`。
- 复现（路径相对仓库根）：

```bash
python3 tools/blkhtml/html2blk.py tools/blkhtml/samples/startmenu.html \
    --out out/startmenu.blk.json --title startmenu --theme dark
python3 tools/blkhtml/validate.py out/startmenu.blk.json
python3 tools/blkhtml/blk2bin.py out/startmenu.blk.json \
    --out-prefix out/startmenu --stats --emit-c out/startmenu.c
python3 tools/blkhtml/preview.py out/startmenu.blk.json \
    --png out/startmenu.png --mode app
sh kernel_v25/build.sh iso
# 内置页路径：shell 内 gui shot-style-app startmenu / gui shot-style-calm startmenu
```

### 9.3 样例 notepad（9 块 · 池 51 · recs 360B）

#### (a) HTML 片段与规则标注

```html
<div class="win" data-blk-drag="title" data-blk-resize="R,B"
     data-blk-resize-edge="6" style="width:800px;height:600px;">
  <div class="titlebar" style="display:flex;flex-direction:row;justify-content:space-between;">
    <span>notes.txt</span>
    <button class="close">Close</button>
  </div>
  <div class="body"><span>Edit your notes here.</span></div>
  <div class="inputbar"><input type="text" style="width:784px;height:24px;"></div>
</div>
```

触发规则：`M-4.1-02`（column）、`M-4.1-01`（row）、`M-4.1-05`（TEXT）、`M-4.1-06`（BUTTON）、`M-4.1-07`（INPUT）、`M-4.2-04`（space-between）、`M-4.8.3`（`data-blk-drag / data-blk-resize`）。不支持项：`U-7.2-07`（button/input 槽 72 拒收）；CE 投影 FAIL=2。

#### (b) `.blk.json` 关键节点

```json
{
  "id": 1, "kind": "column", "parent": 0,
  "rect": { "x": 0, "y": 0, "w": 800, "h": 600 },
  "drag": "title", "resize": { "dir": ["R", "B"], "edge": 6 },
  "children": [
    { "id": 2, "kind": "row", "parent": 1,
      "layout": { "flow": "row", "align_main": "space_between" } },
    { "id": 3, "kind": "text", "parent": 2, "text": "notes.txt",
      "ce": { "id": 3, "parent": 2, "kind": 4, "layer": 200,
              "req_w": -1, "req_h": -1, "text_off": 0, "pix_off": -1,
              "pix_w": 0, "pix_h": 0, "res": -1 } },
    { "id": 4, "kind": "button", "parent": 2,
      "rect": { "x": 712, "y": 8, "w": 64, "h": 24 }, "text": "Close" }
  ]
}
```

#### (c) `recs.bin` 前 5 条记录（每行 40B）

| idx | id | parent | kind | layer | req_w | req_h | text_off | pix_off | res |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 200 | -1 | -1 | -1 | -1 | -1 |
| 1 | 1 | 0 | 1 | 200 | 800 | 600 | -1 | -1 | -1 |
| 2 | 2 | 1 | 2 | 200 | -1 | -1 | -1 | -1 | -1 |
| 3 | 3 | 2 | 4 | 200 | -1 | -1 | 0 | -1 | -1 |
| 4 | 4 | 2 | 12 | 200 | 64 | 24 | 10 | -1 | -1 |

串池：`notes.txt\0Close\0Edit your notes here.\0Type here...\0`（51 B）。

#### (d) 截图、几何 / 像素数字与复现命令

- 裸树校验 `FAIL=0`；CE 投影 `FAIL=2`（button/input 槽 72 拒收）；P1 `FAIL=0`。
- 预览口径：app。基线无可视 CE 帧（§1.8）。
- 哈希锚点：`.blk.json` sha256 `3bd40cb4614e6f71f570f6305dd930f9bb96bb7da8ab0237c102d9641058f8d2`。
- 复现（路径相对仓库根）：

```bash
python3 tools/blkhtml/html2blk.py tools/blkhtml/samples/notepad.html \
    --out out/notepad.blk.json --title notepad --theme dark
python3 tools/blkhtml/validate.py out/notepad.blk.json
python3 tools/blkhtml/blk2bin.py out/notepad.blk.json \
    --out-prefix out/notepad --stats --emit-c out/notepad.c
python3 tools/blkhtml/preview.py out/notepad.blk.json \
    --png out/notepad.png --mode app
sh kernel_v25/build.sh iso
# 内置页路径：shell 内 gui shot-style-app notepad
```

### 9.4 样例 browser-toolbar（8 块 · 池 32 · recs 320B）

#### (a) HTML 片段与规则标注

```html
<div class="toolbar" style="display:flex;flex-direction:row;align-items:center;
     gap:0;background:bar;padding:0;height:40px;">
  <button class="tbtn">Back</button>
  <button class="tbtn">Fwd</button>
  <button class="tbtn">Refresh</button>
  <button class="tbtn">Stop</button>
  <input class="addr" type="text" placeholder="addr">
  <button class="menu">Menu</button>
</div>
```

触发规则：`M-4.1-01`（row）、`M-4.1-06`（BUTTON）、`M-4.1-07`（INPUT）、`M-4.2-05`（align-items:center）、`M-4.2-06`（gap 0）、`M-4.6-04`（`border-radius:8px` 设计态）。贴边专项：按钮组紧贴，触发 R1/S1/S2/D1 核对。不支持项：`U-7.2-07`（button/input CE 拒收）；CE 投影 FAIL=6。

#### (b) `.blk.json` 关键节点

```json
{
  "id": 1, "kind": "row", "parent": 0,
  "rect": { "x": 0, "y": 0, "w": 800, "h": 40 },
  "layout": { "flow": "row", "align_cross": "center", "gap": 0 },
  "fill": "bar",
  "children": [
    { "id": 2, "kind": "button", "parent": 1,
      "rect": { "x": 0, "y": 0, "w": 48, "h": 40 }, "text": "Back",
      "radius": { "tl": 8, "tr": 8, "br": 8, "bl": 8 } },
    { "id": 3, "kind": "button", "parent": 1,
      "rect": { "x": 48, "y": 0, "w": 48, "h": 40 }, "text": "Fwd" },
    { "id": 5, "kind": "input", "parent": 1,
      "rect": { "x": 192, "y": 8, "w": 480, "h": 24 } }
  ]
}
```

#### (c) `recs.bin` 前 5 条记录（每行 40B）

| idx | id | parent | kind | layer | req_w | req_h | text_off | pix_off | res |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 200 | -1 | -1 | -1 | -1 | -1 |
| 1 | 1 | 0 | 2 | 200 | 800 | 40 | -1 | -1 | -1 |
| 2 | 2 | 1 | 12 | 200 | 48 | 40 | 0 | -1 | -1 |
| 3 | 3 | 1 | 12 | 200 | 48 | 40 | 5 | -1 | -1 |
| 4 | 4 | 1 | 12 | 200 | 48 | 40 | 9 | -1 | -1 |

串池：`Back\0Fwd\0Refresh\0Stop\0addr\0Menu\0`（32 B）。

#### (d) 截图、几何 / 像素数字与复现命令

- 裸树校验 `FAIL=0`；贴边断言 R1/S1 通过；CE 投影 `FAIL=6`；P1 `FAIL=0`。
- 预览口径：app、design。本样例为贴边专项，design 口径下核对逐角半径与共享边 1px 分隔线（§5.2）。
- 哈希锚点：`.blk.json` sha256 `8137303d838d2b486ac3c208b2f96863ae585ffde10b09fd29bc24eb9b6f39bf`。
- 复现（路径相对仓库根）：

```bash
python3 tools/blkhtml/html2blk.py tools/blkhtml/samples/browser-toolbar.html \
    --out out/browser-toolbar.blk.json --title browser-toolbar --theme dark
python3 tools/blkhtml/validate.py out/browser-toolbar.blk.json
python3 tools/blkhtml/blk2bin.py out/browser-toolbar.blk.json \
    --out-prefix out/browser-toolbar --stats --emit-c out/browser-toolbar.c
python3 tools/blkhtml/preview.py out/browser-toolbar.blk.json \
    --png out/browser-toolbar.png --mode app
# design 口径：--mode design
sh kernel_v25/build.sh iso
# 内置页路径：shell 内 gui shot-style-app browser-toolbar
```

### 9.5 样例 fullbleed（6 块 · 池 51 · recs 240B）

#### (a) HTML 片段与规则标注

```html
<div class="banner" data-blk-bleed="true" style="width:800px;">
  <span>Release Notes</span>
</div>
<div class="content" style="display:flex;flex-direction:column;gap:8;padding:16px;">
  <span>Welcome to CatOS.</span>
  <span>Blocks and tokens.</span>
</div>
```

触发规则：`M-4.1-02`（column）、`M-4.1-05`（TEXT）、`M-4.2-06`（gap）、`M-4.3-01`（padding）、`M-4.8.3`（`data-blk-bleed`）。贴边专项：banner 全出血，R1/S1/S2/D1 全组通过。

#### (b) `.blk.json` 关键节点

```json
{
  "id": 1, "kind": "column", "parent": 0, "bleed": true,
  "rect": { "x": 0, "y": 0, "w": 800, "h": 96 }, "fill": "bar",
  "children": [
    { "id": 2, "kind": "text", "parent": 1,
      "rect": { "x": 16, "y": 40, "w": 128, "h": 16 }, "text": "Release Notes" }
  ]
}
```

#### (c) `recs.bin` 前 5 条记录（每行 40B）

| idx | id | parent | kind | layer | req_w | req_h | text_off | pix_off | res |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 200 | -1 | -1 | -1 | -1 | -1 |
| 1 | 1 | 0 | 1 | 200 | 800 | 96 | -1 | -1 | -1 |
| 2 | 2 | 1 | 4 | 200 | -1 | -1 | 0 | -1 | -1 |
| 3 | 3 | 0 | 1 | 200 | 768 | 128 | -1 | -1 | -1 |
| 4 | 4 | 3 | 4 | 200 | -1 | -1 | 14 | -1 | -1 |

串池：`Release Notes\0Welcome to CatOS.\0Blocks and tokens.\0`（51 B）。

#### (d) 截图、几何 / 像素数字与复现命令

- 裸树校验 `FAIL=0`；贴边断言 R1/S1/S2/D1 全通过；CE 投影 `FAIL=0`；P1 `FAIL=0`。
- 预览口径：app。`bleed` 块对齐父整框，不参与内容框贴边判定（§5.1）。
- 哈希锚点：`.blk.json` sha256 `263add68c522893ecf183da1b90953bf748762772cf476803286b8822e02cff3`。
- 复现（路径相对仓库根）：

```bash
python3 tools/blkhtml/html2blk.py tools/blkhtml/samples/fullbleed.html \
    --out out/fullbleed.blk.json --title fullbleed --theme dark
python3 tools/blkhtml/validate.py out/fullbleed.blk.json
python3 tools/blkhtml/blk2bin.py out/fullbleed.blk.json \
    --out-prefix out/fullbleed --stats --emit-c out/fullbleed.c
python3 tools/blkhtml/preview.py out/fullbleed.blk.json \
    --png out/fullbleed.png --mode app
sh kernel_v25/build.sh iso
# 内置页路径：shell 内 gui shot-style-app fullbleed
```

### 9.6 产物二进制字节级规范

| 产物 | 规范 |
|---|---|
| `recs.bin` | 等长记录数组，步长 40B；成员布局见 §1.3；`pad0=0`；`pix_off=-1`；`pix_w=pix_h=0`；无图块 `res=-1`；记录顺序与中间层树先序一致，父记录先于子记录 |
| `strings.bin` | NUL 分隔文本池；去重后写入；`text_off` 指向段首；段长由首个 NUL 决定；单段 ≤ 4096 B；整池 ≤ 32768 B；池尾可留多余 NUL |
| 句柄 | IMAGE / CANVAS 的 `res` 由槽 74 / 槽 69 载入，归属本应用；其余 kind `res=-1` |

---

## 10. 版本与变更纪律

### 10.1 变更记录

| 版本 | 日期 | 改动 | 驱动方 |
|---|---|---|---|
| v0.1 | 基线 `89584c4` | 工作记录稿：HTML/JSON → 块树映射、内核事实摘录、首轮样例证据 | 参考实现轮次 |
| v0.2 | 2026-11-25 | 定位重写（三层真源，工具降为可弃参考实现）；新增 §3 `blk_spec` v2 演进计划；§7 三态判定（甲 / 乙 / 丙）+ 有损表；规则 ID 体系（M-* / U-* / L-7.3-* / A-9-*）；断言可复核化（`path::symbol` 双标注）；§9 五个端到端样例附录；规范表全部内联，去除对外部契约文件的权威外挂 | 规范定稿 |

### 10.2 文档债定义与登记位置

- **文档债**：本文与内核源码冲突、§3 字段落地后命名 / 语义与本表不一致、或 §7 归类因内核演进需要调整而未同步的条目。
- **登记位置**：本节变更记录表追加一行，列：版本、日期、冲突条款（§ 号 + ID）、源码位置（`path::symbol`）、待同步方向、登记人。
- **同步义务**：任何内核 ABI 变更必须在本 md 同步落一条变更记录，否则视为文档债。ABI 变更包括：`blk_spec` / `block` / `ce_blk_rec_t` 字段增删、枚举值调整、槽位增减、配额与拒绝码变更。
- **处置顺序**：文档与参考实现冲突以本文为准；本文与源码冲突以源码为准并登记文档债；债务在下次规范冻结前清零或转入 §3 演进计划。

### 10.3 冻结条件

进入冻结前须满足：§8 全部断言在干净 checkout 上 RC=0；A-9-01..04 spec-audit 通过；§3 未落地字段均标注依赖状态；§7 甲类每条可回指 §3 字段或 §14 条款；§9 五样例产物二进制过 P1。
