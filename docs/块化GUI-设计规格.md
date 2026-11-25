# CatOS 块化声明式 GUI —— 设计规格（内核级 v1）

| 项 | 内容 |
|---|---|
| 状态 | Draft v1.0，可实现 |
| 范围 | `kernel_v25` 内的 GUI 子系统（无独立 Shell 进程、无显示服务进程） |
| 输入文档 | `声明式GUI系统设计计划-zh.md`（用户态 Shell 版，作为语义参考） |
| 核心命题 | 桌面是一棵**块树**，是内核块表的**声明式投影**；没有窗口概念，一切 UI 都是块 |

## 0. 与参考文档的差异（先定调，避免照搬）

1. **只有块（Block）**：不存在 window/win_id/窗口表。标题栏、任务栏、开始菜单、通知、工具提示都是块；一个"窗口"只是带边框和标题栏子块的 `BLK_STACK` 子树。
2. **内核级**：块树、布局、脏标记、合成全部在 `kernel_v25` 内核态完成，不做进程间提交、不做 IPC 协议。参考文档的"Shell 进程 + 显示服务进程 + 隔离档位 B/C"在 v1 全部不实现。
3. **借鉴 ArkUI 的"声明式"**：每帧由 `build()` 重建**描述**，与上一帧描述 diff，只把差异落到块树；再 measure → layout → paint。这是 ArkUI 的核心循环，不是 HTML 的 DOM/CSS。
4. **借鉴 HTML 的部分只有三样**：块树嵌套、样式继承（令牌）、事件命中。不引入 DOM API、不引入选择器、不引入"标签"。`div` 这个词在本项目禁止出现。
5. **性能优先于表达力**：v1 砍掉圆角、阴影、半透明、变换动画；令牌里保留字段但实现为直角/单描边，动画只允许"位置瞬移 + 帧计数 Presence"。

## 1. 数据模型

```c
typedef uint32_t block_id;          /* 0 = 无效；>=1 全局唯一 */

typedef enum {
    BLK_ROOT = 0, BLK_COLUMN, BLK_ROW, BLK_STACK,   /* 容器 */
    BLK_TEXT, BLK_RECT, BLK_IMAGE, BLK_SURFACE,      /* 叶 */
    BLK_SPACER, BLK_DIVIDER, BLK_CANVAS,
    BLK_FREE,                                       /* v2 追加在末尾：自由布局容器，规格见 块化GUI-v2-规格.md */
    BLK_BUTTON, BLK_INPUT, BLK_TERM               /* desk 追加：按钮 / 单行输入框 / 块内终端 */
} blk_kind;

typedef struct block {
    block_id   id;
    blk_kind   kind;
    struct block *parent, *first_child, *next_sibling;

    int32_t    req_w, req_h;        /* 声明尺寸，-1 = auto */
    int32_t    x, y, w, h;          /* 布局结果（像素，绝对坐标） */
    int32_t    pad_t, pad_r, pad_b, pad_l;
    int32_t    gap;                 /* ROW/COLUMN 子块间距 */
    uint8_t    align_main, align_cross, align_self;

    uint16_t   layer;               /* 见 §5 层枚举 */
    int32_t    z;                   /* 层内相对序 */
    uint8_t    visible, focusable, focused, inert;

    uint8_t    dirty_layout, dirty_paint, dirty_children, leaving;

    const char *text;               /* BLK_TEXT */
    const uint8_t *pix;             /* BLK_IMAGE / BLK_SURFACE，24bpp */
    int32_t    pix_w, pix_h;
    uint8_t    pix_opaque_only;     /* v1 只支持不透明位图 */

    struct style st;                /* 已解析的样式（§6） */

    void (*on_click)(block_id id, int32_t lx, int32_t ly);
    void (*on_drag )(block_id id, int32_t dx, int32_t dy, uint8_t phase);
} block;
```

块表（内核唯一真源，`gui/btable.c`）：`block_id → block*` 的定长数组 + 空闲链；`owner` 字段预留（0 = 内核，>0 = pid，v1 恒为 0）。

**投影规则（幂等）**

- `build()` 产出的是"描述"（`blk_desc` 数组 + 父子关系），不是直接改块树。
- `gui_commit_desc()` 负责 diff：`id` 相同则只更新变化属性并打脏位；新增则插入；本轮描述里消失的进入 `leaving`。
- 全量重建（`gui_rebuild()`）与增量提交必须得到同一棵块树——这是验收第 1 条。

## 2. 声明式 API（ArkUI 形态）

```c
void        gui_begin(void);                  /* 进入图形块桌面，保存文本控制台状态 */
block_id    b_column(block_id parent, const blk_spec *s);
block_id    b_row   (block_id parent, const blk_spec *s);
block_id    b_stack (block_id parent, const blk_spec *s);
block_id    b_text  (block_id parent, const blk_spec *s, const char *txt);
block_id    b_rect  (block_id parent, const blk_spec *s);      /* 纯色矩形 */
block_id    b_image (block_id parent, const blk_spec *s, const uint8_t *pix, int w, int h);
block_id    b_surface(block_id parent, const blk_spec *s);     /* 内容协议 §7 */
block_id    b_space (block_id parent, int w, int h);
void        b_set_props(block_id id, const blk_spec *s);       /* 属性更新，不重建 */
void        b_remove(block_id id);                             /* 两阶段销毁 */
void        gui_frame(void);                                   /* 一帧：measure→layout→paint→flush */
void        gui_end(void);                                     /* 退回文本控制台 */
```

`blk_spec` 是"声明的属性集合"（尺寸、padding、gap、对齐、层、令牌引用、文本/位图、回调）。**禁止**任何 API 用 `window` 命名。

`build()` 由三处驱动：内部 UI 模块（任务栏/开始菜单/时钟）、应用模块（内核内 demo）、以及后续 CE ABI 提交（§7）。

## 3. 布局

- 两阶段：`measure`（自底向上算 auto 尺寸，约束 = 父给的最大可用空间）→ `layout`（自顶向下写绝对 x/y）。
- 支持三种容器：`COLUMN`（主轴纵向）、`ROW`（主轴横向）、`STACK`（层叠，子块各自定位，默认左上+居中选项）。
- 对齐：主轴 `start/center/end/space_between`，交叉轴 `start/center/end/stretch`。
- `gap`、`padding` 四边独立；`req_w/req_h >= 0` 为固定尺寸，`-1` 为 auto。
- 文本度量：`w = 8 * scale * strlen`，`h = 16 * scale`（scale ∈ {1,2}，v1 只用 1 和 2）；不换行，超宽截断并写 `…`（用 ASCII `...`）。
- 禁用"父脏则全子树重算"：只有 `dirty_layout` 的子树参与 measure/layout。

## 4. 脏标记与帧调度

- `dirty_layout`：几何/尺寸/最小化/令牌尺寸类变化 → 冒泡到最近布局容器。
- `dirty_paint`：焦点/文本/颜色/可见性 → 只标自身。
- 帧节拍：PIT 100 Hz → **每 3 tick 合成一次（≈33 fps）**；`gui_frame()` 可被显式调用（demo/探针用），但同一 tick 内重复调用只算一次提交。
- 提交期禁止嵌套提交：事件回调里只打脏位，由帧调度器统一提交。
- damage：合成阶段把脏块的旧矩形与新矩形合并成 damage 矩形链表（上限 8 个，超出退化为"包围盒"），只 blit 这些区域。

## 5. 层与绘制

```
LAYER_BG      = 0     /* 壁纸/桌面底色 */
LAYER_CONTENT = 100   /* 图标、普通块 */
LAYER_WORK    = 200   /* 应用块子树（z 决定前后） */
LAYER_CHROME  = 3000  /* 任务栏 */
LAYER_OVERLAY = 4000  /* 开始菜单、右键菜单 */
LAYER_TOAST   = 5000
```

- 绘制顺序：layer 升序 → z 升序 → 树先序；`STACK` 子块按 z。
- 绘制原语（内核内直接写 LFB）：`fill_rect`、`blit24`、`draw_text`（8×16 位图字形）、`draw_border`（1px，可 2 层）。全部走**裁剪栈**（clip rect 与父链求交）。
- 光标：合成结束后统一画 12×12 箭头，使用内核已有的 save/restore 精灵机制（`mouse.c` + `be_vbe24` 的 MC 逻辑），不参与块树。
- 无效区域外不重绘；`visible=0` 的块不参与 measure/paint。

## 6. 样式令牌

```c
struct style {
    uint32_t fg, bg, accent;   /* 0x00RRGGBB */
    uint8_t  scale;            /* 字缩放 1/2 */
    uint8_t  pad;              /* 令牌级 padding */
    uint8_t  border_w;         /* 0/1/2 */
};                             /* v1 无 radius / elevation / alpha */
```

- 定义在 `gui/tokens.c`：`TOK_DARK`、`TOK_LIGHT`、`TOK_HIGH_CONTRAST` 三套值。
- 解析：子块未显式给 `fg/bg` 则继承父块已解析值；令牌变更 = 全局 `dirty_paint`。
- `radius.window`/`elevation.window`/`anim.*` 在 v1 的等价实现：直角 + 1px 双层描边 + 无动画（`anim.duration.*` 恒 0）。它们的字段可以留，但实现必须是"无效果"，避免假承诺。

## 7. 内容协议 v1（应用怎么画进来）

- `BLK_SURFACE`：一块 24bpp、不透明的像素缓冲 + 尺寸；内核合成时 1:1 blit，超出裁剪，不缩放、不做 alpha。
- `BLK_BUTTON`：单行按钮，绘制底/边/居中文案，聚焦（按压）时反色反馈；点击冒泡到自身 `on_click`。
- `BLK_INPUT`：单行输入框，聚焦后接管键盘（可打印字符 / Backspace / Left/Right / Home/End），绘制文本与光标竖线，Enter 触发 `on_submit`；v1 单实例，复用现有全局 key_hook 路径。
- v1 供给方只有内核内模块（任务栏时钟、demo 应用）。CE 侧接入留到 v2：新增 ABI 槽位 `gui_surface_attach(w,h)->ptr` + `gui_surface_dirty(rect)`，由 `gui/compose` 在读帧时检测 dirty 队列；**v1 不实现**，但结构体与 API 预留。
- 该协议是 v1 唯一的"应用内容"通道；块节点化（应用直接建块）留 v2。

## 8. 输入与交互

- 原始输入：`mouse_get_state()`（绝对像素 + 按键位）+ 键盘 `kb_getkey_timed()`。
- 命中：layer 降序 → z 降序 → 坐标落在块矩形内 → 取最深的可命中块（`inert` 跳过）。
- **捕获阶段系统手势优先**：桌面上边缘/角落热区（唤起开始菜单）、任务栏点击、块标题栏拖拽、右下角 resize、`Alt+Tab` 等价切换、`Esc` 退出桌面。命中即消费，不下发。
- 拖拽：改 `req` 位置→打 `dirty_layout`；拖拽期间该子树内容块设 `no_input`，其他子树不重排（验收第 2 条）。
- 焦点：`focused_block` 单槽 + `focus_stack[4]`；模态块（v1 只有开始菜单）出现时向下块设 `inert`。

## 9. 验收（可测，探针必须能打点）

1. **投影幂等**：随机生成 20 组块描述，分别走"全量 rebuild"与"增量 commit"，块树结构哈希（id/kind/父子/几何）一致。
2. **脏标记正确性**：拖动单个块一轮，`stat_layout_nodes` ≤ 该子树节点数；记录到串口。
3. **系统手势优先**：demo 块的 `on_click` 返回"消费一切"，拖拽标题栏仍生效。
4. **重建**：`gui_reset()` 后按块表重建，节点数/几何与 reset 前快照一致，孤儿数 0。
5. **无泄漏**：开关 1000 次块，`stat_blocks_live` 与块表条目数一致，`stat_blocks_leaked == 0`。
6. **提交对齐**：连续 1 秒，`stat_frames` ≤ 33，且 `stat_double_commit == 0`。
7. **帧耗时**：800×600 全量首帧 ≤ 8 ms，拖拽帧（damage ≤ 25% 屏）≤ 4 ms（QEMU `-cpu qemu32 -m 256 -smp 1`，3 次中位数）。

探针口径：串口输出 `GUI:` 前缀行（`GUI:frames=...`），文本控制台在 `gui` 激活期间不参与；探针用 `-serial file:` 抓取，必要时配合 screendump 做视觉回归。

## 10. 里程碑（每阶段必须有可跑命令 + 验收）

| 阶段 | 内容 | 验收 |
|---|---|---|
| M0 | `gui/` 骨架：块树、btable、样式解析、`gui_frame` 提交链、LFB 原语封装、`gui` 命令进出 | 屏幕出桌面底色 + 一个 320×200 带标题栏的块 + 一条任务栏；`Esc` 干净退回文本控制台；文本侧 vprobe 等门禁不受影响 |
| M1 | measure/layout（COLUMN/ROW/STACK、padding/gap/对齐）、diff 提交、脏位 | 改一个块 `req_w` 只重排该子树；20 组随机描述幂等探针通过 |
| M2 | damage 合成 + 块拖拽/resize + 命中 + 焦点 + 层/z | 拖两个块互不干扰；拖拽帧 ≤4 ms；`stat_layout_nodes` 达标 |
| M3 | 任务栏（开始按钮/块列表/时钟）、开始菜单（块列表 + 启动 demo）、Presence 帧计数 | 点开始按钮弹菜单；关闭块有 leaving 帧且句柄回收 |
| M4 | 令牌三套主题切换 + `gui_reset` 重建 + demo 块树应用 + 全量回归 | 切主题无硬编码色残留（静态检查脚本）；reset 重建 100%；vprobe 10/10、E3 2/2、F11 9/9 仍全绿 |

## 11. 代码结构

```
kernel_v25/
  include/gui/{block.h, gui.h, tokens.h}
  src/gui/{btable.c, tree.c, desc.c, layout.c, paint.c, compose.c,
           input.c, tokens.c, chrome.c, demo.c}
  src/shell.c            # 新增 `gui` 内建命令（进入/退出）
  tests/gui_{layout,paint,idempotent,drag,leak}.py
docs/块化GUI-进度.md      # 每轮更新的进度与打点
```

## 12. 风险与砍项

| 风险 | 处理 |
|---|---|
| 块树在内核态出 bug = 内核崩 | `gui_reset()` 可重入；每次 frame 前后做节点计数断言；`gui` 命令可随时退出 |
| 像素带宽（800×600×3 ≈ 1.37 MB/帧） | 只 blit damage；帧上限 33 fps；VMware SVGA 25 Hz flush 下不追求更高 |
| 无 alpha/圆角导致"看起来不像桌面" | 用 token 配色 + 1px 双层描边补偿；不为观感引入混合 |
| 应用接入缺失导致"只有壳" | 先做内核内 demo 块树（文本/矩形/位图/任务栏）确保链路完整；CE 接入明确列为 v2 |
| 布局引擎复杂度失控 | v1 只有三种容器；不做 Grid/Flex-wrap/基线对齐 |

## 13. 明确不做（v1）

多显示器、吸附分屏、圆角/阴影/半透明/变换动画、Grid、文本换行与富文本、应用节点化、跨进程代理与沙箱、无障碍 AT 协议、FLIP 动画。

## 14. 圆角与贴边策略（per-corner radius，v2 calm 外观档位）

> 本节为 v2 calm & soft 外观档位的追加规格，不回溯改写上文 v1 砍项约定。圆角不再按块整体取
> 统一半径，而是**逐角判定**；与兄弟块或父块完全贴边的那一侧走方角对接。本节决定 cov 形状，
> 覆盖率合成（见 §14.4）决定 cov 如何落到像素，两者同时生效后才出图。

### 14.1 逐角判定（禁全局统一半径）
- 每个角由两条边界定；**任一条边贴边，该角半径 = 0（方角）**。
- 贴边定义：布局量化（走 8pt 网格归整）后坐标**精确相等**——x/y/w/h 与父内容区边界或兄弟块
  边界同值，**容差 0**；不许用“看起来接近”的阈值判定贴边。
- 圆角半径按块自由边尺寸走既有阶梯档（小控件 8 / 按钮图标 12 / 面板弹出层 16 / 大卡片 24），
  但每个角独立取值；贴边角强制取 0。

### 14.2 贴父内容区边界
- 内嵌卡片的上下左右内边：贴到父内容区边界的那一侧，该侧两角方角。
- full-bleed 通栏块：左右全方角，外侧沿用父外形，不新画弧。

### 14.3 与兄弟块紧贴
- 相接那条边整条方角；两块间**零缝隙、零叠压**——不得因各自圆角在接缝漏出父底色缝，也不得
  两边各画一条描边形成 2px 双线。
- 描边在贴边侧合并：共享边只画 **1px 分隔线**（取父/子描边色中较暗一档），各自贴边侧描边省略；
  自由边照常画描边。
- 阴影在贴边侧取消：贴边处不得投影（避免接缝造假空隙/暗带）；阴影只出现在自由边外侧。

### 14.4 覆盖率合成（cov 如何落像素）
- 每块对每像素算覆盖率 `cov ∈ [0,255]`，`out = under*(1-cov) + color*cov`，`under` 为该像素
  当前已合成好的父表面结果；不使用 XOR、`dst=src` 或“先写再抠”。
- `cov=0` 的角外区域必须逐像素等于父表面原值（再由父形状裁剪兜底）；`cov=255` 块内等于块色；
  边缘 1px 带内 cov 单调过渡（连续曲率，非锯齿弧）。
- 父形状裁剪：子块/子文本按父的圆角形状裁剪，不只矩形裁剪；子块贴到父圆角区时跟随父曲线，
  但子块靠内的那个角方角。
- 阴影为块本体之下、父表面之上的独立外扩合成层，不靠压暗块色伪造；阴影 alpha 叠加后角部不得
  把父内容透出当“阴影纹理”。

### 14.5 组件成组规则
- 按钮、输入栏、菜单项、列表行成组紧贴时（菜单项纵向排列、Settings 行列表）：**组内相接边
  方角，只有整组最外沿的角圆**；只有独立浮起的卡片/窗口四角才全圆。

### 14.6 验收用例（像素可判定）
1. 父卡内嵌子块贴左 / 贴顶；
2. 两块横向紧贴；
3. 纵向菜单项整组；
4. full-bleed 通栏块。

每例采样接缝两侧像素，断言：无父底色缝（接缝处无第三方颜色行）、无双线（分隔线厚度 = 1px）、
贴边侧角点 3×3 邻域内无弧线过渡像素（方角）、接缝处与两侧块色差 ≤1 级（无阴影暗带）。
dark / light / hc 三主题各跑一遍，附修前/修后同坐标截图与 diff。

### 14.7 术语表（追加）
- **radius（逐角半径）**：单个角的圆角半径 px；贴边角为 0。
- **flush（贴边）**：布局量化后坐标与父内容区边界或兄弟块边界精确相等（容差 0）。
- **packed（成组紧贴）**：兄弟块间零缝隙、零叠压地排成一组；组内相接边方角，仅整组外沿圆角。
