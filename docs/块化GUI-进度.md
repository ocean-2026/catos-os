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
| M3 | 任务栏、开始菜单、Presence | 未开始 | — |
| M4 | 令牌主题、`gui_reset` 重建、demo 块树、全量回归 | 未开始 | — |
| 旧 desktop 退役 | `build/pkg.list` 摘掉 desktop 包，源文件保留作学习参考 | 已完成 | pkg.list 摘包；desktop.c 加 LEGACY 注释；gfxdesk.c 仍不打包 |

## 2. 推进日志（追加，倒序在最上面）

| 时间 | 轮次 | 做了什么 | 提交 | 探针结果 | 下一步 |
|---|---|---|---|---|---|
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

## 4. 性能打点基线（每次合成/布局跑完填）

| 指标 | 值 | 条件 |
|---|---|---|
| 首帧全量合成 | 中位数 1.88 ms（样本 2.19 / 1.87 / 1.88 ms），整屏 area=480000 px，≤8ms PASS | QEMU `-cpu qemu32 -m 256 -smp 1`，800×600，`gui home` 全量建树后首帧，3 次中位数（measure_frame.py） |
| 拖拽帧（damage ≤25% 屏） | 中位数 0.89 ms（样本 0.96 / 0.89 / 0.86 ms）；damage 9600 px ≈ 2.0% 屏，≤4ms PASS | 同上，`gui drag` 单块 120×80 移动 |
| 布局节点数（单块拖拽） | 2（子树 2 + 根） | `stat_layout_nodes`，`gui drag` |
