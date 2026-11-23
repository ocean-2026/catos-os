# 块化 GUI 进度表（通宵推进用）

> 每完成一轮（一次提交/一次探针）就在 §2 追加一行，别删历史。
> 规格见 `docs/块化GUI-设计规格.md`；分支 `feat/gui-blocks`，工作树 `/home/user/work/gui-blocks`。
> 截止：2026-11-24 10:00（Asia/Shanghai）。09:30 之后只做汇总与报告，不再开新功能。

## 1. 里程碑状态

| 阶段 | 内容 | 状态 | 探针/证据 |
|---|---|---|---|
| M0 | 骨架：块树/btable/`gui_frame`/LFB 原语/`gui` 命令 | 代码完成，几何缺陷已修，待全量门禁回归 | artifacts/gui-blocks/m0-desktop.png（底色满屏/320×200 标题栏块/任务栏贴底满宽）；Esc 退回文本正常；`gui bounds` result=1 |
| M1 | 布局：COLUMN/ROW/STACK、padding/gap/对齐、diff 提交、脏位 | 探针通过，待 M4 全量回归 | `gui idem` 20/20 match=1、`GUI:idempotent=OK`；`gui dirty` measured=3/total=7、affected_subtree=3、untouched=3、result=1；`gui leak` leaked=0 |
| M2 | damage 合成、拖拽/resize、命中、焦点、层/z | 探针通过，待 M4 全量回归 | `gui drag` 两块互不干扰 b_same=1、measured=2/subtree=2、damage_rects=1、damage_area=9600（2.0% 屏）；`gui hit` 同层 z 高者命中；`gui rate` frames=33、double_commit=0；零异常 |
| M3 | 任务栏、开始菜单、Presence | 探针通过，待 M4 全量回归 | `gui menu` open=1、items=1、overlay=5、launched=2、result=1；`gui presence` base=11、added=12、leaving=1、live_after=11、leaked=0、result=1；`gui leak` 1000 次开关 live=0/tree=1/leaked=0；时钟秒级自刷只打自身脏位 |
| M4 | 令牌主题、`gui_reset` 重建、demo 块树、全量回归 | 探针通过，待全量门禁回归 | `gui theme` 三套令牌 bg/fg/accent 互异、result=1；`gui reset` nodes=15、hash 前后一致、orphan=0、match=1；demo 树含 TEXT/RECT/IMAGE/DIVIDER/SPACER；`gui gesture` clicked=1/drag_moved=1/taskbar_hit=2；`gui resize` w=160/h=110、sibling_same=1、leaf_inside=1；跨运行 idem 哈希两次同为 0x88751F07 |
| 旧 desktop 退役 | `build/pkg.list` 摘掉 desktop 包，源文件保留作学习参考 | 已完成 | pkg.list 摘包；desktop.c 加 LEGACY 注释；gfxdesk.c 仍不打包 |

## 2. 推进日志（追加，倒序在最上面）

| 时间 | 轮次 | 做了什么 | 提交 | 探针结果 | 下一步 |
|---|---|---|---|---|---|
| 2026-11-24 04:10 | R8 | 修复 WORK/CONTENT 层像素不上屏：第二遍损伤合成由"脏节点 + 命中 damage 的干净节点都重绘"改为只重绘脏节点（`snap`），消除全屏背景命中 damage 后整屏重填、擦掉干净前景的问题；新增 `tests/gui_pixel_assert.py`（P6 PPM 逐点采样：ne_bg / ne_color / color / pair_diff 四类断言）与 `tests/gui_pixel_regress.py`（`-vga std` 起机抓帧 + 跑默认三条断言）；`shot.sh`/回归截图统一 std | 待提交 | std 桌面像素回归 6/6 PASS；15 条串口探针全 PASS；lint GREEN（0 violation / 36 advisory / 13 review） | 补拍其余 5 张验收图；全量门禁 vprobe/E3/F11；REPORT.md、sha256、push |
| 2026-11-24 03:10 | R7 | M0 视觉 QC 代码侧修复：令牌扩 `panel/bar/border/line` 分层色（dark bg `0x1E1E26` / panel `0x2A2A34` / bar `0x3A3A48` / border `0x555A66` / accent `0x4C8DFF`；light/HC 同步填值），`spec_panel` 按层取色（BG 桌面色 / CHROME·OVERLAY 条色 / WORK 块色），描边走令牌 border、分隔线走令牌 line；demo 内容区由 RECT 改 COLUMN 竖排（TEXT → DIVIDER → IMAGE 16×16 → SPACER，gap 8）；开始按钮/时钟行 `align_cross=XALN_CENTER` 文字垂直居中；标题栏条色显式取 bar；新增截图专用壳命令 `gui shot-menu / shot-light / shot-hc / shot-reset`（进入对应状态后空闲等截）；`shot.sh` 加 `VGA` 环境变量可切后端 | 待提交 | 15 探针全 PASS 零异常；lint GREEN；geom 转储确认子块已竖排：id6 TEXT y=92、id7 DIVIDER y=116、id8 IMAGE 16×16 y=125、id9 SPACER y=149；时钟 ASCII `HH:MM:SS` 正常出数（无 `???`） | 待可截图环境补拍 6 张验收图（判据见 §3 阻塞行）；全量门禁与 REPORT.md |
| 2026-11-24 02:00 | R6 | M4 主题/reset/完整 demo 树 + 缺口探针：`gui_theme_set` 切令牌后整树 dirty_paint；`gui reset` 探针快照节点数/哈希/孤儿数后重建比对；demo 树补 SPACER/DIVIDER/16×16 IMAGE；空闲链改为取最小空闲槽（修重建 id 乱序），demo 建树恢复根尺寸；新增 `gui gesture`（消费型 on_click 下标题栏拖拽/任务栏命中仍生效）、`gui resize`（右下角手柄改 req_w/req_h 只重排子树、兄弟不动）；leak 探针覆盖 1000 次开关；help 列表补 gui 行；跨运行 idem 哈希两次一致 | 待提交 | 15 探针全 PASS 零异常；theme result=1；reset match=1 orphan=0；gesture drag_moved=1 taskbar_hit=2；resize sibling_same=1 leaf_inside=1；leak live=0/tree=1/leaked=0；lint GREEN；跨运行哈希 0x88751F07 稳定 | 全量门禁 vprobe/E3/F11、`gui` 进出控制台洁净度、REPORT.md 与 push |
| 2026-11-24 01:10 | R5 | M3 任务栏/开始菜单/Presence：开始按钮 `on_click` 开合 LAYER_OVERLAY 菜单，菜单内逐条列出可聚焦工作块（行 `z` 存目标块 id）+ Launch demo 条目；点条目聚焦并前移兄弟链末尾，或启动第二个示例块；菜单出现时下块设 inert，Esc 与点下层空白优先关菜单；时钟按 RTC 秒刷新、只打自身 `dirty_paint`；Presence 两阶段销毁：`b_remove` 摘树标 `leaving`，存活一帧（计数到 2）下一帧由 `gui_presence_sweep` 回收并归还空闲链；新增 `gui menu`/`gui presence` 探针，leak 探针改批次两轮 sweep 支撑 1000 次开关 | 待提交 | menu open=1/items=1/overlay=5/launched=2/result=1；presence base=11 added=12 leaving=1 live_after=11 leaked=0 result=1；leak 1000 次 live=0 tree=1 leaked=0；其余 9 探针回归 PASS、零异常；lint GREEN | M4：三套主题切换接线、`gui_reset` 重建独立探针、完整 demo 树（TEXT/RECT/IMAGE/SURFACE/DIVIDER/SPACER）、六条缺口探针收尾、全量门禁 |
| 2026-11-24 00:35 | R4 | 几何缺陷修复 + lint 归零 + 首帧打点：底色块/任务栏改全出血 `bleed`，宽高从当前模式尺寸取（不写死 800）；容器子块原点 = 父内容框 + 统一 padding，标题栏/任务栏文字 4px 左内边距；新增 `gui bounds` 不变量探针（`child.rect ⊂ parent.content_rect`、`root.rect ⊂ screen`，越界打 `GUI:bounds ... result=0`）；lint 36 fail 归零：局部 `win` 改名 `sample_spec`，UI 路径硬编码色全部改取活动令牌（`gui_active_style()`），清屏色/继承兜底取当前主题 bg/fg，探针 fixture 色改令牌引用；补首帧打点 `GUI:frame first=1 us=.. rects=.. area=.. frame_us=..`，后续每 30 帧节流一行；新增 `gui home` 桌面探针供首帧测量；修 NULL 规格叶子 `visible=0` 与 layer 未继承导致文字被父底色覆盖 | 待提交 | lint GREEN（0 fail / 36 advisory / 13 review）；8 探针 idem/dirty/drag/hit/rate/leak/geom/bounds 全 PASS 零异常；measure_frame 3 次中位数：首帧 1.88ms（样本 2.19/1.87/1.88，≤8ms PASS）、拖拽帧 0.89ms（0.96/0.89/0.86，≤4ms PASS）、damage 2.0%；新几何转储（800×600）：`root 1000 (0,0,800,600)`；`id1 RECT (0,0,800,600)`；`id2 COLUMN (64,64,320,200)`；`id3 ROW (64,64,320,24)`；`id4 TEXT (68,64,96,16)`；`id5 RECT (64,88,320,176)`；`id6 TEXT (68,88,200,16)`；`id7 ROW (0,568,800,32)`；`id8 ROW (0,568,64,24)`；`id9 TEXT (4,568,40,16)`；`id10 ROW (64,568,80,24)`；`id11 TEXT (80,568,64,16)` | M3：任务栏块列表/时钟秒刷、开始菜单 OVERLAY+inert+关闭、leaving 两阶段销毁；随后 M4 主题切换/reset 重建/完整 demo 树 |
| 2026-11-23 23:35 | R3 | M2 damage 合成与交互：damage 两遍收集（脏块矩形合并，上限 8 退化包围盒），干净且不与 damage 相交的节点跳过不写 VRAM；帧前快照脏位（布局清位前记录 `snap`，绘制以快照为准）；输入侧补探针注入钩子（press/move/release）与命中查询；拖拽块加 `moved` 标记，STACK 布局保留其绝对位置；脏位向最近布局容器冒泡；新增 `gui drag`/`gui hit`/`gui rate` 探针 | 待提交 | drag：b_same=1、measured=2/5、damage 9600px（2.0%）、511µs；hit 命中 z 高块；rate 33 帧、零重复提交；idem/dirty/leak/geom 回归全 PASS、零异常；桌面截图正常 | M3：任务栏开始按钮/块列表/时钟、开始菜单、leaving 帧回收；随后 M4 主题/reset 重建/全量门禁 |
| 2026-11-23 23:10 | R2 | M1 布局与 diff 投影收尾：修复 diff 插入不按描述显式 id 落槽（btable_alloc_at + 空闲链脱链）；修复 layout_dirty 未向脏后代冒泡（根未置脏时脏子树永不重排）；新增 `gui dirty` 脏子树计数探针与 `tests/gui_probe.py` 驱动（固定命令行，判定接外部基建 gui_parse） | 待提交 | idem 20/20 match=1、OK；dirty measured=4/7、受影响子树 3、未受影响 3、result=1；leak live=0 leaked=0；三例串口零异常 | M2：damage 合并/只 blit 脏区、拖拽/resize、命中/焦点、拖拽帧耗时打点 |
| 2026-11-23 22:46 | R1 | M0 骨架落地（提交 2363243）：`include/gui/{block,gui,tokens}.h`、`src/gui/{btable,tree,desc,layout,paint,compose,input,tokens,chrome}.c`；块表+块树+声明式建块；LFB 原语 fill_rect/blit24/draw_text/draw_border + 裁剪栈；COLUMN/ROW/STACK 基础布局；`gui` 内建命令进出（复用 gfx24 路径，Esc 经 `con_force_text` 退回）；旧 desktop 退役：pkg.list 摘包、desktop.c 加 legacy 注释，两源文件保留 | 待提交 | 截图 m0-desktop.ppm/png：底色深灰、示例块 320×200 带标题栏、底部任务栏在位；Esc 后提示条回归，ls 正常；串口 m0-serial.log 无内核 fault | M1：对齐补齐（space_between/stretch 等）、20 组随机描述 rebuild vs commit 幂等哈希探针、脏子树计数 |
| — | — | 初始建表 | — | — | M0 |

## 3. 阻塞与取舍记录

| 时间 | 问题 | 处理 | 结论 |
|---|---|---|---|
| 2026-11-23 23:00 | diff 提交幂等探针首轮外全部 match=0：新建块走空闲链分配 id，忽略描述里的显式 id；二次 commit 按 spec id 匹配失败，旧节点被判 unseen 回收、新节点另建 | 块表新增 `btable_alloc_at(kind,id)`，插入前先把目标槽从空闲链脱链；槽位占用才退回空闲链分配 | rebuild 与增量 commit 得到同一棵块树，20 组随机描述哈希一致 |
| 2026-11-23 23:05 | 改叶子 req_w 后第二次布局不重排：`layout_dirty` 只在节点自带脏位时向下走，根脏位已清，脏后代被整棵跳过 | `layout_dirty` 改为先递归子节点、任一后代脏即标脏，再 layout_node 并清位 | 脏子树冒泡正确；未受影响兄弟子树不参与重排 |
| 2026-11-23 23:20 | damage 首帧被整帧跳过：`layout_run` 在绘制前清掉 dirty_layout，收集 damage 时所有块已变干净，只画到底色 | 布局清位前先做帧快照 `snap`；damage 收集与绘制判定都以快照为准，绘制完成后清 `snap`/dirty_paint | 首帧桌面块/任务栏恢复；后续干净帧只重绘 damage 相交区 |
| 2026-11-23 23:25 | 叶子脏位不冒泡：`measure_dirty` 只看直接子节点自带脏位，孙辈脏时父容器与根被跳过，布局计数缺节点 | 子树任一后代脏即向父节点置 `dirty_layout`（冒泡到最近布局容器） | 脏子树计数正确（dirty measured=3/7） |
| 2026-11-23 23:28 | 拖拽块的绝对位置被 STACK 布局下一帧重置回原点 | 块加 `moved` 标记，`on_drag_move` 置位；STACK 布局对 `moved` 子块保留 x/y | 拖拽位置跨帧保持；未拖拽兄弟子树位置不变 |
| 备注 | 跨运行哈希值有小幅波动，但单轮内 rebuild/commit 一致（验收口径只要求轮内一致）；波动来源待 M4 复核 | — | 不阻塞 M1 |
| 2026-11-24 00:15 | 进度正常未停滞：M2 探针运行中（build.sh + gui_probe drag 活跃），近 30 分钟 7 个串口证据持续产出，工作树 9 个文件为 M2 未提交改动 | 本线程只读，不改代码不提交 | 等 M2 提交后进 M3 |
| 2026-11-24 04:10 | **WORK/CONTENT 层像素不上屏（已定位并修复，旧"环境抓不到 LFB"结论作废）**：对照实验证明 std 与 vmware 都能抓到帧、任务栏/文字落地，问题在合成层。块树/布局/首帧 damage/几何转储均正确，填充也到达后端 `fill()`；根因是第二遍损伤合成里"干净节点只要与 damage 相交就整节点重绘"：后续帧时钟等小块产生 damage，全屏背景节点 `id1` 命中 damage 后整屏重填底色，把尚未重绘的干净 WORK 前景整片擦掉，而 WORK 不与 damage 相交、不会被恢复。| 第二遍只重绘本帧脏节点（`snap`），移除"命中即重绘干净底层块"的整节点重绘；背景擦旧位置应由背景在损伤条带内补齐，不在此整节点重填。`tests/gui_pixel_assert.py` 固化三条像素断言（块边缘非底色 / 标题条与块底互异且非底色 / 任务栏与 Start 文字非底色），`tests/gui_pixel_regress.py` 起 std 客人机抓帧并回归 | 修复后 `-vga std` 桌面 6/6 断言 PASS：(65,65)=bar `0x3A3A48`、(150,70)=fg、(200,120)=panel `0x2A2A34`、(400,585)=bar、(20,585)=fg；15 条串口探针全 PASS，lint GREEN | 待补其余 5 张验收图（菜单/三套主题/reset），判据：② `m3-menu-open.png` OVERLAY 条目展开、下层任务栏仍可见；③ dark 分层色可辨；④ light 整屏跟随变浅；⑤ HC 黑底黄描边/黄强调；⑥ reset 后桌面与首帧一致 |

| 2026-11-24 02:15 | 进度活跃未停滞：M3 已提交 4f39fb8，M4 侧 15 探针全 PASS、lint GREEN，当前 qemu geom3 探针（monitor 4602）在跑，近 10 分钟 5 份串口日志产出；§2 顶行时间戳写 03:10 与实际 02:15 不符，属记录笔误 | 本线程只读，不改代码不提交 | 继续 M4 收尾：补截图验收、跑全量门禁、写 REPORT.md |

## 4. 性能打点基线（每次合成/布局跑完填）

| 指标 | 值 | 条件 |
|---|---|---|
| 首帧全量合成 | 中位数 1.88 ms（样本 2.19 / 1.87 / 1.88 ms），整屏 area=480000 px，≤8ms PASS | QEMU `-cpu qemu32 -m 256 -smp 1`，800×600，`gui home` 全量建树后首帧，3 次中位数（measure_frame.py） |
| 拖拽帧（damage ≤25% 屏） | 中位数 0.89 ms（样本 0.96 / 0.89 / 0.86 ms）；damage 9600 px ≈ 2.0% 屏，≤4ms PASS | 同上，`gui drag` 单块 120×80 移动 |
| 布局节点数（单块拖拽） | 2（子树 2 + 根） | `stat_layout_nodes`，`gui drag` |
