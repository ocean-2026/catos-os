# 块化声明式 GUI — M0→M4 收口报告

分支：`feat/gui-blocks`　工作树：`/home/user/work/gui-blocks`
报告时间：2026-11-24 05:10（Asia/Shanghai）
构建：`cd kernel_v25 && sh build.sh iso`；QEMU `/usr/bin/qemu-system-i386` 6.2.0，`-m 256 -smp 1 -cpu qemu32`，验收截图一律 `-vga std`。

## 1. 里程碑实况

| 阶段 | 状态 | 证据 |
|---|---|---|
| M0 骨架（块树/btable/`gui_frame`/LFB 原语/`gui` 命令） | 完成，像素断言 PASS | `m0-desktop.png`；dark 分组 6/6 |
| M1 布局（COLUMN/ROW/STACK、padding/gap/对齐、diff 提交、脏位） | 完成，探针 PASS | `idem` 20/20 match=1；`dirty` measured=3/total=7；`leak` leaked=0 |
| M2 damage 合成、拖拽/resize、命中、焦点、层/z | 完成，探针 PASS | `drag` b_same=1、damage 9600px（2.0%）；`hit` z 高者命中；`rate` 33 帧 double_commit=0 |
| M3 任务栏/开始菜单/Presence | 完成，探针 + 像素断言 PASS | `menu` overlay=5、launched=2；`presence` leaving=1、leaked=0；`m3-menu-open.png` 浮层两条目可见 |
| M4 令牌主题/`gui_reset`/demo 树/全量回归 | 完成：主题 live 重着色、reset 探针/像素断言通过；E3/vprobe/F11 三门禁全绿（见 §5） | light/hc 分组 6/6；`reset` nodes=15、orphan=0、match=1 |

## 2. 提交列表（本收口轮次）

| commit | 说明 |
|---|---|
| `6e32624` | fix: WORK 前景被背景命中重绘擦掉 + 主题角色化 live 重着色；像素断言库与回归脚本入树 |
| `2916717` | fix: 开始菜单浮层定位/高度/标签悬空修复 |
| `7e81168` | docs: M4 收口报告、门禁基线、控制台洁净度回归 |
| `94680d6` | fix(paging) #D11: 任务私有 PT 镜像用户窗口内的内核静态帧（supervisor-only）；新增链接布局护栏 `tools/check_link_layout.py` |
| `b7ae5ce` | test: 像素四组（dark/light/hc/menu）纳入全量回归，组→shot 子命令自动映射 |

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

## 5. 门禁结果（全绿）

| 门禁 | 结果 | 耗时 | 备注 |
|---|---|---|---|
| vprobe（Issue #78 跨页系统调用） | **10/10 PASS** | 154.4s | `gates/run_vprobe.py`，端口 4461；日志 `logs/gate-vprobe-r10.log` |
| E3（任务系统调用 staging buffer） | **2/2 PASS**（badptr + conc） | 80.1s | `gates/run_e3.py`，端口 4462；修复前 conc 3/3 FAIL，见 §5.1；日志 `logs/gate-e3-r10.log` |
| F11（$C$ 摘要/权限/掩码） | **9/9 PASS** | 161.9s | `gates/run_f11.py`，端口 4481；基准盘 `scratch/test_disk.img` 先复制 /tmp 私有副本，未写原盘；日志 `logs/gate-f11-r10.log` |
| 本地串口探针 | **15/15 PASS** | — | 见 §3 |
| 像素断言 | dark 6/6、light 6/6、hc 6/6、menu 2/2 | — | 见 §4 |
| 控制台洁净度 | **PASS**（差异 0.91%，阈值 5%） | — | 见 §4 |

### 5.1 E3 conc 回退：根因、取证与修复

**故障现象（修复前，三轮逐次一致）**：`run /bin/e3probe.ce parent` 触发 ring0 #PF：

```
int=0000000e err=00000000 eip=010480ed cr2=02001050 cs=00000008 efl=00000083 eax=02000efc
EIP-> acl_lookup+0x0000005D page fault
```

ERR=0 解码：bit0 P=0 页不存在，bit1 W/R=0 读，bit2 U/S=0 ring0。即**内核态在 syscall 路径上读了一个未映射的用户 VA 片地址**，不是用户态越权。基线 ISO（main `94be727`）同机 3/3 双 PASS；`e3probe.ce` 两树 md5 相同，纯内核侧回退。

**根因（静态可证，独立复现见 `probes/E3-CONC-REPRO.md`）**：

- 故障指令 `mov 0xa8(%esi),%edx` 读 `acl_tab[i].used`。算式吻合：feat `acl_tab` 基址 `0x01ff8540`，条目步长 `0xac`，第 206 条 `0x01ff8540 + 206×0xac = 0x02000fa8`，`+0xa8 = 0x02001050`，与 CR2 完全相等。
- 机制：任务页目录先把 `kdir[i]` 全量镜像，随后把 PDE 8（`0x2000000..0x23FFFFF`）、PDE 9（`0x2400000..0x27FFFFF`）替换成任务私有 4 KiB 页表。syscall 在内核态执行时仍使用**调用者的 CR3**；落在这两个窗口内的内核静态对象不在任务私有 PT 中 present，遍历即走未映射 PTE → ring0 #PF。
- 体积成因：GUI 新增静态/代码使 `_bss_end` 从基线 `0x02092030` 长到 `0x020ae75c`；`acl_tab`（基线 `0x01fe9540..0x01ff4140`，全部在 `0x2000000` 以下）被挤进窗口，feat 区间 `0x01ff8540..0x02003140`。`USER_IMAGE_BASE` 等是 `paging.h` 编译期常量、**不随镜像体积漂移**；随体积漂的是 .bss 布局。

**修法选型（按爆炸半径评估）**：

- **A 抬高 `USER_IMAGE_BASE`**：需同步 CE loader、`ce.h`、PDE 索引、攻击样例与主机模拟脚本，动用户 ABI 布局，半径最大，未采用。
- **B' 任务私有 PT 补 supervisor 镜像（采用）**：不切上下文、不改地址布局、CE ABI 不动。`task_space_create()` 在替换 PDE 8/9 后，把落在两个窗口内、且物理帧 `< frames_base`（即内核静态帧，分配器从不外发，见 #D10 物理片预留）的页，按恒等物理地址装成 supervisor-only PTE（P|RW，无 US 位）；任务后续映射自己的 image/heap/stack 时按槽覆盖为用户 PTE，两类映射不冲突。复用了既有 LFB PDE supervisor 镜像的同款模式（`sync_lfb_pdes`）。
- **C 压缩 GUI 静态占用**：治标。实测 `acl_tab` 在 .bss 内偏移固定，其位置由 `.text/.reloc` 总量上推，压缩 GUI bss 数组拉不回边界；仅在 A/B 都不可接受时才考虑，本轮未采用。

**修复 diff**：`kernel_v25/src/paging.c` 新增 `mirror_kernel_static_in_arena()`，在 `task_space_create()` 对 `[USER_IMAGE_BASE, USER_ARENA_END)` 与 `[USER_EXT_BASE, USER_EXT_END)` 两个窗口逐槽补 supervisor PTE；`task_space_destroy()` 对 `< frames_base` 的帧 `frame_free` 早退，不会误释放内核静态帧。

**护栏（结构性防回归）**：`kernel_v25/tools/check_link_layout.py`，链接后用 `nm -S` 解析 PE 符号，对落入 `[0x2000000, 0x2800000)` 窗口的 syscall 路径关键表（`acl_tab/users/tasks/pipes/cur_task`）打 `[link][RISK]`，其余静态打 `[link][warn]`。本地构建非阻断（B' 镜像兜底）；`--strict` 供 CI/未来“把表拉回窗口以下”的布局收缩里程碑判失败。本次构建护栏输出：

```
[link] _bss_end = 0x020ae75c  arena window = [0x02000000, 0x02800000)
[link][RISK] key table `users` 0x02003160..0x02004ae0 (size 0x1980) lands in the user arena window
[link][RISK] key table `acl_tab` 0x01ff8540..0x02003140 (size 0xac00) lands in the user arena window
```

**对照实验定性（一锤定音）**：临时把 `gui/*.o` 从链接摘掉（`gui` 命令壳以空桩保留）重链对照 ISO，E3 badptr/conc 双 PASS、无页错误 → 确认回退是 GUI 引入的镜像布局问题，而非 spawn/acl/分页代码逻辑缺陷。对照产物未入树。

**修复后门禁**：E3 `PASS badptr` / `PASS conc`（`steps=2 ok=2 fail=0`）；vprobe 10/10、F11 9/9、15 探针、四组像素、洁净度全部绿（见上表）。

**未取得的运行时 PTE 证据（如实标注）**：本轮未在 BSOD 后驱动 HMP `M map` / `P dump` 导出 PDE/PT 内容；“任务私有 PT 中 `0x2001000` 页不存在”这一点与 ERR 码、地址算式、反汇编、页表代码路径一致，但 PTE 级转储留待后续取证增强（BSOD 后自动导出 / panic_record 落盘）。

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
- 尺寸：11,954,176 字节（约 11.40 MiB）；`build/kernel.bin` 1,228,756 字节，`_bss_end = 0x020ae75c`
- sha256：`a688f098284eeaf289b3559fe28efae2569080b7716d25cb09f4bef1df50fcdc`

## 9. 剩余项与风险

1. **布局残留（B' 兜底，非阻塞）**：`acl_tab`/`users` 仍静态落在用户窗口内（护栏 `[link][RISK]` 列出）。B' supervisor 镜像保证其在调用者 CR3 下可达；根治方向是后续收缩 `.text/.bss`（块池/按需分配、crashlog 符号表精简）把关键表拉回 `0x2000000` 以下，届时启用护栏 `--strict`。当前基线镜像本身也有大量非 syscall 静态位于窗口内，故未把“窗口内有静态”设为硬失败。
2. 探针子命令（home/geom 等）提前 return 分支未请求 `CON_MODE_VBE24`，探针帧后端 `lfb=NULL`；串口判定不依赖图形后端，本轮未改。
3. snap-only 重绘下，拖拽旧位置的背景补齐语义未做损伤条带内回填；drag 探针只校验几何未校验像素残影。
4. lint：0 violation / 36 advisory / 14 review（不阻断）；C5 advisory 未清：`demo.c` 拆分与五个分项测试脚本。
5. 主题探针目前为链路级（标脏/结果位）；像素级覆盖由 `gui_pixel_assert.py` 的 light/hc 分组在客人机上完成。
6. PTE 级取证未补（见 §5.1 末）：BSOD 后未自动导出 PDE/PT；如需闭环“私有 PT 该页 present 与否”的运行时实证，待加 BSOD 转储导出。
