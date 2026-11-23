# 块化 GUI 进度表（通宵推进用）

> 每完成一轮（一次提交/一次探针）就在 §2 追加一行，别删历史。
> 规格见 `docs/块化GUI-设计规格.md`；分支 `feat/gui-blocks`，工作树 `/home/user/work/gui-blocks`。
> 截止：2026-11-24 10:00（Asia/Shanghai）。09:30 之后只做汇总与报告，不再开新功能。

## 1. 里程碑状态

| 阶段 | 内容 | 状态 | 探针/证据 |
|---|---|---|---|
| M0 | 骨架：块树/btable/`gui_frame`/LFB 原语/`gui` 命令 | 代码完成，待全量门禁回归 | artifacts/gui-blocks/m0-desktop.png（底色/320×200 标题栏块/任务栏）；Esc 退回文本正常 |
| M1 | 布局：COLUMN/ROW/STACK、padding/gap/对齐、diff 提交、脏位 | 探针通过，待 M4 全量回归 | `gui idem` 20/20 match=1、`GUI:idempotent=OK`；`gui dirty` measured=3/total=7、affected_subtree=3、untouched=3、result=1；`gui leak` leaked=0 |
| M2 | damage 合成、拖拽/resize、命中、焦点、层/z | 探针通过，待 M4 全量回归 | `gui drag` 两块互不干扰 b_same=1、measured=2/subtree=2、damage_rects=1、damage_area=9600（2.0% 屏）、frame_us=511（≤4ms）；`gui hit` 同层 z 高者命中；`gui rate` frames=33、double_commit=0；零异常 |
| M3 | 任务栏、开始菜单、Presence | 未开始 | — |
| M4 | 令牌主题、`gui_reset` 重建、demo 块树、全量回归 | 未开始 | — |
| 旧 desktop 退役 | `build/pkg.list` 摘掉 desktop 包，源文件保留作学习参考 | 已完成 | pkg.list 摘包；desktop.c 加 LEGACY 注释；gfxdesk.c 仍不打包 |

## 2. 推进日志（追加，倒序在最上面）

| 时间 | 轮次 | 做了什么 | 提交 | 探针结果 | 下一步 |
|---|---|---|---|---|---|
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

## 4. 性能打点基线（每次合成/布局跑完填）

| 指标 | 值 | 条件 |
|---|---|---|
| 首帧全量合成 | 待测 | QEMU `-cpu qemu32 -m 256 -smp 1`，3 次中位数（M4 补测） |
| 拖拽帧（damage ≤25% 屏） | 511 µs；damage 9600 px ≈ 2.0% 屏 | 同上，`gui drag` 单块 120×80 移动 |
| 布局节点数（单块拖拽） | 2（子树 2 + 根） | `stat_layout_nodes`，`gui drag` |
