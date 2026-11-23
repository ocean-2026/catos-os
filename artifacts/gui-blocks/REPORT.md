# 块化声明式 GUI — M0→M4 收口报告

分支：`feat/gui-blocks`　工作树：`/home/user/work/gui-blocks`
报告时间：2026-11-24 04:30（Asia/Shanghai）
构建：`cd kernel_v25 && sh build.sh iso`；QEMU `/usr/bin/qemu-system-i386` 6.2.0，`-m 256 -smp 1 -cpu qemu32`，验收截图一律 `-vga std`。

## 1. 里程碑实况

| 阶段 | 状态 | 证据 |
|---|---|---|
| M0 骨架（块树/btable/`gui_frame`/LFB 原语/`gui` 命令） | 完成，像素断言 PASS | `m0-desktop.png`；dark 分组 6/6 |
| M1 布局（COLUMN/ROW/STACK、padding/gap/对齐、diff 提交、脏位） | 完成，探针 PASS | `idem` 20/20 match=1；`dirty` measured=3/total=7；`leak` leaked=0 |
| M2 damage 合成、拖拽/resize、命中、焦点、层/z | 完成，探针 PASS | `drag` b_same=1、damage 9600px（2.0%）；`hit` z 高者命中；`rate` 33 帧 double_commit=0 |
| M3 任务栏/开始菜单/Presence | 完成，探针 + 像素断言 PASS | `menu` overlay=5、launched=2；`presence` leaving=1、leaked=0；`m3-menu-open.png` 浮层两条目可见 |
| M4 令牌主题/`gui_reset`/demo 树/全量回归 | **部分完成**：主题 live 重着色与 reset 探针/像素断言通过；门禁 vprobe/F11 通过，**E3 conc 未通过**（见 §5 阻塞） | light/hc 分组 6/6；`reset` nodes=15、orphan=0、match=1 |

## 2. 提交列表（本收口轮次）

| commit | 说明 |
|---|---|
| `6e32624` | fix: WORK 前景被背景命中重绘擦掉 + 主题角色化 live 重着色；像素断言库与回归脚本入树 |
| `2916717` | fix: 开始菜单浮层定位/高度/标签悬空修复 |

更早基线提交：`4f39fb8`（M3 任务栏/菜单/Presence）、`06972b4`（几何不变量 + lint 归零 + 首帧打点）、`b5e8467`（M2 damage 合成与交互）。

## 3. 探针结果（串口 GUI: 行，原始日志见 `artifacts/gui-blocks/<subcmd>-serial.log`）

| 探针 | 关键输出 | 判定 |
|---|---|---|
| idem | `GUI:idempotent=OK`（20 轮 rebuild vs commit 哈希一致） | PASS |
| leak | `GUI:leak live=0 tree=1 leaked=0`（1000 次开关） | PASS |
| geom | 800×600 树几何转储，WORK/CHROME/OVERLAY 层号与坐标正确 | PASS |
| bounds | `GUI:bounds result=1`（子块 ⊂ 父内容框，根 ⊂ 屏幕） | PASS |
| dirty | `measured=3 total=7 affected_subtree=3 untouched=3 result=1` | PASS |
| drag | `ax=110 ay=100 bx=200 by=100 measured=2 subtree=2 damage_rects=1 damage_area=9600 b_same=1 result=1` | PASS |
| hit | 同层 z 高块命中，`result=1` | PASS |
| rate | `frames=33 double_commit=0 ticks=99 result=1` | PASS |
| home | `frame first=1 us=1783 rects=1 area=480000 frame_us=1783 result=1` | PASS |
| menu | `open=0 items=1 overlay=5 launched=2 result=1`（探针结束前已关菜单） | PASS |
| presence | `base=14 added=15 leaving=1 live_after=14 leaked=0 result=1` | PASS |
| theme | `dark_bg=0x001E1E26 light_bg=0x00ECECF1 hc_bg=0x00000000 result=1` | PASS（链路级；像素级见 §4 分组断言） |
| reset | `nodes=15 hash=0x9E82D66C orphan=0 match=1 result=1` | PASS |
| gesture | `clicked=1 drag_moved=1 taskbar_hit=2 result=1` | PASS |
| resize | `grew=1 sibling_same=1 leaf_inside=1 result=1` | PASS |

本地串口探针合计 **15/15 PASS**，零内核异常。

## 4. 像素断言（`tests/gui_pixel_assert.py`，P6 PPM 逐点采样）

断言类型：`ne_bg` / `ne_color` / `color` / `pair_diff` / `region_count`；容差每通道 6。分组：

| 分组 | 点位/区域 | 结果 |
|---|---|---|
| dark（M0 三条，6 check） | (65,65) 块边缘；(150,70) 标题条 / (200,120) 块底互异；(400,585) 任务栏 / (20,585) Start 文字 | **6/6 PASS**：bar `(58,58,72)`、fg `(232,232,236)`、panel `(42,42,52)` |
| light（6 check） | 桌面底 (300,300)=`0xECECF1`、块体 (200,120)=`0xDDDDE6`、条 (400,585)=`0xC8C8D6`，且均 ≠ 暗色令牌 | **6/6 PASS** |
| hc（6 check） | 底 `0x000000`、块 `0x101010`、条 `0x202020`，且 ≠ 暗色令牌 | **6/6 PASS** |
| menu（2 check） | 浮层矩形 (8,400,180,96) 内 bar 色 ≥100px、fg 色 ≥100px | **2/2 PASS**（bar 9951px / fg 489px） |

控制台洁净度（`tests/gui_console_clean.py`）：`gui` 进入前 / Esc 退出后各抓 640×400 文本帧逐点比对，差异 2338/256000 px（0.91%，阈值 5%），**PASS**。

## 5. 门禁结果与阻塞项

| 门禁 | 结果 | 耗时 | 备注 |
|---|---|---|---|
| vprobe（Issue #78 跨页系统调用） | **10/10 PASS** | 154.6s | `gates/run_vprobe.py`，端口 4461 |
| E3（任务系统调用 staging buffer） | **1/2：badptr PASS，conc FAIL** | 80.3s | 见下 |
| F11（$C$ 摘要/权限/掩码） | **9/9 PASS** | 161.9s | `gates/run_f11.py`，端口 4481；基准盘 `scratch/test_disk.img` 先复制 /tmp 私有副本，未写原盘 |

### 5.1 E3 conc 阻塞（未修复，标未验证）

- 现象：`run /bin/e3probe.ce parent` 触发 kernel fault：
  `task e3probe tid 1 EIP=0x010480ED ERR=0x00000000 CR2=0x02001050 EIP-> acl_lookup+0x0000005D page fault`。
- 对照：同机基线 ISO（main `94be727`）E3 **2/2 PASS**；本分支 `task.c / user.c / sec.c / ce.c / vma.c / paging.c` 相对 main **无源码改动**，仅新增 `gui/` 对象并扩大内核镜像（`kernel.bin` 1.163MB → 1.229MB）。
- 复现性：连续 3 次（含删除共享 scratch 盘全新格式化）故障地址逐次完全确定，非磁盘偶发；F11 9/9、vprobe 10/10 通过，故障局限于并发 spawn / 子任务路径。
- 待查方向：对比 baseline 与本分支 `kernel.pe` 段/BSS 落点，核查内核镜像是否越界到 CE 用户扩展区（`USER_EXT_BASE ≈ 0x02000000`，CR2 `0x02001050` 落在该区）；必要时收紧 `gui/` 静态块表容量（`GUI_MAX_BLOCKS=256`）或核对多引导镜像上限。
- 处置：本轮未改 security/spawn 源码；**E3 判据未达成，不计入通过**。

## 6. 帧耗时基线（条件：QEMU `-cpu qemu32 -m 256 -smp 1`，800×600）

| 指标 | 值 | 条件 |
|---|---|---|
| 首帧全量合成 | 1.783 ms（`home` 探针 area=480000px，≤8ms PASS）；历史中位数 1.88ms（2.19/1.87/1.88） | `gui home` 全量建树后首帧 |
| 拖拽帧（damage ≤25% 屏） | 0.877 ms；damage 9600px ≈ 2.0% 屏（≤4ms PASS） | `gui drag` 单块 120×80 移动 |
| 节流帧 | `GUI:frame n=31 us=9`；同 tick 重复提交由合成层去重 | `gui rate` 33 帧 |
| 布局节点数（单块拖拽） | 2（子树 2 + 根） | `stat_layout_nodes` |

## 7. 验收截图（`artifacts/gui-blocks/`，均 `-vga std`）

| 文件 | 肉眼可见内容 |
|---|---|
| `m0-desktop.png` | dark 桌面：左上 320×200 示例块，标题条 "CatOS Sample"、正文 "demo content area"、分隔线、16×16 棋盘图标；底部满宽任务栏 Start + ASCII 时钟；鼠标箭头在中部 |
| `m4-theme-dark.png` | 同 dark 首帧（令牌 dark：bg `0x1E1E26` / panel `0x2A2A34` / bar `0x3A3A48` / fg `0xE8E8EC`） |
| `m3-menu-open.png` | 开始菜单浮层展开于左下 (8,400)，两条目 "block 2" / "Launch demo"；下层任务栏与时钟仍可见 |
| `m4-theme-light.png` | 浅色令牌：桌面底 `0xECECF1`、块体 `0xDDDDE6`、条 `0xC8C8D6`、文字 `0x181820`，整屏跟随变浅 |
| `m4-theme-contrast.png` | 高对比：黑底 `0x000000`、块 `0x101010`、条 `0x202020`、白字、黄分隔线 `0xFFFF00` |
| `m4-reset.png` | `gui_reset` 重建后桌面，与首帧 dark 一致（`reset` 探针 nodes=15、哈希前后一致、orphan=0） |

## 8. 交付物指纹

- ISO：`kernel_v25/catos.iso`
- 尺寸：11,954,176 字节（约 11.40 MiB）
- sha256：`3395e23a58dba7e140acf4de76560b80749622a523e5525a649b5370b86f0ab9`

## 9. 剩余项与风险

1. **E3 conc 页错误未定位**（§5.1）：门禁 1/2，需在合并前解决或显式降级口径。
2. 探针子命令（home/geom 等）提前 return 分支未请求 `CON_MODE_VBE24`，探针帧后端 `lfb=NULL`；串口判定不依赖图形后端，本轮未改。
3. snap-only 重绘下，拖拽旧位置的背景补齐语义未做损伤条带内回填；drag 探针只校验几何未校验像素残影。
4. lint：0 violation / 36 advisory / 14 review（不阻断）；C5 advisory 未清：`demo.c` 拆分与五个分项测试脚本。
5. 主题探针目前为链路级（标脏/结果位）；像素级覆盖由 `gui_pixel_assert.py` 的 light/hc 分组在客人机上完成。
