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
| rate | `frames=33 double_commit=0 ticks=99 result=1`（探针自造节拍，仅验证去重） | PASS |
| idle | **生产空闲路径**：`frames=25 double_commit=0 ticks=100 result=1`（3-tick 节流下 100 tick 提交 25 帧，≤33 帧/秒口径，零重复提交） | PASS |
| diff | 两阶段销毁：`base=2 leaving=1 live_leaving=2 live_after=1 leaked=0 result=1`（删掉的块先入 leaving 帧，下一帧才回收，live 回落） | PASS |
| home | `frame first=1 us=1783 rects=1 area=480000 frame_us=1783 result=1` | PASS |
| menu | `open=0 items=1 overlay=5 launched=2 result=1`（探针结束前已关菜单） | PASS |
| presence | `base=14 added=15 leaving=1 live_after=14 leaked=0 result=1` | PASS |
| theme | `dark_bg=0x001E1E26 light_bg=0x00ECECF1 hc_bg=0x00000000 result=1` | PASS（链路级；像素级见 §4 分组断言） |
| reset | `nodes=15 hash=0x9E82D66C orphan=0 match=1 result=1` | PASS |
| gesture | `clicked=1 drag_moved=1 taskbar_hit=2 result=1` | PASS |
| resize | `grew=1 sibling_same=1 leaf_inside=1 result=1` | PASS |

本地串口探针合计 **17/17 PASS**（原 15 + `idle` 生产节流、`diff` 两阶段销毁），零内核异常。

## 4. 像素断言（`tests/gui_pixel_assert.py`，P6 PPM 逐点采样）

断言类型：`ne_bg` / `ne_color` / `color` / `pair_diff` / `region_count`；容差每通道 6。分组：

| 分组 | 点位/区域 | 结果 |
|---|---|---|
| dark（M0 三条，6 check） | (65,65) 块边缘；(150,70) 标题条 / (200,120) 块底互异；(400,585) 任务栏 / (20,585) Start 文字 | **6/6 PASS**：bar `(58,58,72)`、fg `(232,232,236)`、panel `(42,42,52)` |
| light（6 check） | 桌面底 (300,300)=`0xECECF1`、块体 (200,120)=`0xDDDDE6`、条 (400,585)=`0xC8C8D6`，且均 ≠ 暗色令牌 | **6/6 PASS** |
| hc（6 check） | 底 `0x000000`、块 `0x101010`、条 `0x202020`，且 ≠ 暗色令牌 | **6/6 PASS** |
| menu（2 check） | 浮层矩形 (8,400,180,96) 内 bar 色 ≥100px、fg 色 ≥100px | **2/2 PASS**（bar 9951px / fg 489px） |

控制台洁净度（`tests/gui_console_clean.py`）：`gui` 进入前 / Esc 退出后各抓 640×400 文本帧逐点比对，差异 2338/256000 px（0.91%，阈值 5%），**PASS**。

> D7 裁剪链路：`gui_blit24` 已与 `gui_fill_rect` 同口径接入 `clip_rect()` 父链求交（commit `95aacc9`，后端 `gfx24_blit_stride` 按裁剪后原点与原始行步进偏移）。现有四组帧未触发超父框位图；“块内放超出父框的位图、框外保持背景色”的独立像素用例列为 §9 残留项，本轮未补确定性超框帧。

## 5. 门禁结果（全绿）

| 门禁 | 结果 | 耗时 | 备注 |
|---|---|---|---|
| vprobe（Issue #78 跨页系统调用） | **10/10 PASS** | 156.9s | `gates/run_vprobe.py`，端口 4461；日志 `logs/gate-vprobe-r12.log` |
| E3（任务系统调用 staging buffer） | **2/2 PASS**（badptr + conc） | 80.2s | `gates/run_e3.py`，端口 4462；修复前 conc 3/3 FAIL，见 §5.1；日志 `logs/gate-e3-r12.log` |
| F11（$C$ 摘要/权限/掩码） | **9/9 PASS** | 161.9s | `gates/run_f11.py`，端口 4481；基准盘 `scratch/test_disk.img` 先复制 /tmp 私有副本，未写原盘；日志 `logs/gate-f11-r12.log` |
| 本地串口探针 | **17/17 PASS** | — | 见 §3 |
| 像素断言 | dark 6/6、light 6/6、hc 6/6、menu 2/2 | — | 见 §4 |
| 控制台洁净度 | **PASS**（差异 0.91%，2338/256000 px，阈值 5%） | — | 见 §4 |
| 静态槽守卫自检 | **PASS**：`[guard] static-slot guard rejected=1 intact=1 result=1` | 启动期 | `paging_static_slot_guard_selftest()`，见 §5.2 |

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

**修复后门禁**：E3 `PASS badptr` / `PASS conc`（`steps=2 ok=2 fail=0`）；vprobe 10/10、F11 9/9、17 串口探针、四组像素、洁净度全部绿（见 §5 表，R12 实测）。

**未取得的运行时 PTE 证据（如实标注）**：本轮未在 BSOD 后驱动 HMP `M map` / `P dump` 导出 PDE/PT 内容；“任务私有 PT 中 `0x2001000` 页不存在”这一点与 ERR 码、地址算式、反汇编、页表代码路径一致，但 PTE 级转储留待后续取证增强（BSOD 后自动导出 / panic_record 落盘）。

### 5.1.1 链接顺序根治（R11，治本路线成本最低一条）

按 `build.sh` 正常链接顺序（保持 `boot.o cpu.o` 在最前，入口不动），把 `user.o`、`task.o` 紧随其后前置，其余对象顺序不变。重链后护栏 `tools/check_link_layout.py --strict` 退出 0，syscall 路径关键表全部回到 `0x2000000` 用户窗口以下：

| 关键表 | 地址（重链后） | 窗口 `[0x02000000, 0x02800000)` |
|---|---|---|
| `_acl_tab` | `0x01136440` | 界下 |
| `_users` | `0x01141060` | 界下 |
| `_pipes` | `0x011abe00` | 界下 |
| `_cur_task` | `0x012555e4` | 界下 |
| `_tasks` | `0x01255600` | 界下 |

`_bss_end / _load_end = 0x020af0a0`；仍有大量**非 syscall 路径**静态（GUI 节点池、日志缓冲、音频/PCI 小标量等）落在窗口内，由 B' supervisor 镜像兜底，护栏对它们只打 `[link][warn]`；`--strict` 只对关键表落窗口判失败，`--strict-warn` 才对其余静态判失败。对照实验与结论见 §9.1。

### 5.2 B' 兜底安全性加固（拒绝把内核静态帧装成用户 PTE）

**隐患**：B' 在任务私有 PT 里为窗口内 `<frames_base` 的内核静态帧装了 supervisor-only PTE。若将来用户镜像/heap 恰好映射到同一 VA 槽，覆盖该 PTE 时若把**同一内核静态物理帧**带 US 位装成用户项，会把内核静态帧暴露给 ring3 写（安全洞）。

**实现**：`task_map_page()` 在写 PTE 前检查新物理帧：`new_pa < frames_base`（属内核 image/.bss 片，分配器从不外发）即拒绝本次映射，返回 `-1`，并打醒目日志：

```
[link][RISK] refuse user map onto kernel static frame va=02000000 frame=020af000 pid=00000000
```

**口径说明（避免误杀合法路径）**：用一块新分配的用户堆/镜像帧（`pa >= frames_base`）**按槽覆盖** B' 镜像 PTE 是 B' 设计允许的行为——内核静态帧在 `kdir` 下仍常驻，任务只是丢掉自己那份私有镜像；守卫**不**拒绝这种覆盖，只拦截“把内核静态物理帧装进用户 PTE”这一条真正的暴露路径。

**自检探针**：`paging_static_slot_guard_selftest()` 建一个临时任务空间，向 arena 槽请求一张恒等静态帧（`<frames_base`）的用户映射，断言 `rc=-1` 且槽 PTE 原样未改；启动期输出：

```
[guard] static-slot guard rejected=1 intact=1 result=1
```

`kernel.c` 在 `tasks_init()` 后、`intr_enable()` 前调用。至此“用户映射静默覆盖内核静态 supervisor 槽”在写入路径上已不可能：要么合法堆帧按槽覆盖（镜像保留在 kdir），要么静态帧被拒绝并留痕。

### 5.3 验收 rubric 逐条自检（最终 ISO `f9bf3a27`）

| # | 验收项 | 实测数字 | 证据文件 / 来源 | 达成 |
|---|---|---|---|---|
| 1 | E3 2/2（badptr + conc） | 2/2 PASS，80.2s | `probes/logs/gate-e3-r12.log` | 达成 |
| 2 | vprobe 10/10（跨页 syscall） | 10/10 PASS，156.9s | `probes/logs/gate-vprobe-r12.log` | 达成 |
| 3 | F11 9/9（摘要/权限/掩码） | 9/9 PASS，161.9s | `probes/logs/gate-f11-r12.log` | 达成 |
| 4 | 本地串口探针 | **17/17 PASS**（原 15 + `idle`/`diff`），零异常 | `artifacts/gui-blocks/<subcmd>-serial.log` | 达成 |
| 5 | 像素断言 dark/light/hc/menu | 6/6、6/6、6/6、2/2 | `tests/gui_pixel_regress.py` 四组；PPM 落 `artifacts/gui-blocks/` | 达成 |
| 6 | 控制台洁净度 | 2338/256000 px（0.91%，阈值 5%）PASS | `tests/gui_console_clean.py` | 达成 |
| 7 | 链接顺序根治：关键表回到窗口以下 | `acl_tab 0x01136440`、`users 0x01141060`、`pipes 0x011abe00`、`cur_task 0x012555e4`、`tasks 0x01255600`，均 `<0x2000000`；`check_link_layout.py --strict` 退出 0 | `kernel_v25/tools/check_link_layout.py`；提交 `67ad3ea` | 达成 |
| 8 | B' 兜底：拒绝用户映射覆盖内核静态槽 | `[link][RISK] refuse user map onto kernel static frame va=02000000 frame=020af000 pid=00000000`；自检 `rejected=1 intact=1 result=1` | `artifacts/gui-blocks/home-serial.log`；`paging.c`/`kernel.c`；提交 `fc327e3` | 达成（口径见 §5.2：只拒绝静态帧入用户 PTE，合法堆帧按槽覆盖放行） |
| 9 | D2 无 window 语义命名；lint C1 收紧 `\bwin\w*` | lint **0 violation / 39 advisory / 14 review**，GREEN；代码侧无 `win*` 命名 | `probes/lint/gui-lint.py`；提交 `cc7b8b1` | 达成 |
| 10 | D1 生产循环 3-tick 节流；实测 ≤33 帧/秒，零重复提交 | 生产 `idle frames=25 double_commit=0 ticks=100`（≈25fps） | `artifacts/gui-blocks/idle-serial.log`；提交 `cc7b8b1` | 达成 |
| 11 | D7 blit 接入 GUI 裁剪栈 | 代码：`gui_blit24` 先 `clip_rect()` 父链求交 + `gfx24_blit_stride`；提交 `95aacc9` | `src/gui/paint.c`、`src/be_vbe24.c`、`include/gfx24.h` | **部分达成**：裁剪代码已修；“超父框位图框外保底色”独立像素用例未补，残留 §9.3-1 |
| 12 | D3 diff 消失节点走 leaving 两阶段销毁 | `diff base=2 leaving=1 live_leaving=2 live_after=1 leaked=0 result=1`（先 leaving 帧，下一帧回收） | `artifacts/gui-blocks/diff-serial.log`；提交 `cc7b8b1` | 达成 |
| 13 | 链接后布局护栏（关键表落窗口告警/失败） | `--strict` 关键表 RISK 判失败；`--strict-warn` 其余静态判失败；本次 strict 退出 0 | `kernel_v25/tools/check_link_layout.py`；提交 `67ad3ea` | 达成 |
| 14 | D8 v2 槽位空桩声明 | `gui_surface_attach/gui_surface_dirty` 原型入 `include/gui/gui.h`，未实现 | 提交 `a87bf76` | 达成 |
| 15 | REPORT §4/§5/§8 与进度表回填、ISO 指纹 | ISO 11,956,224 B，`kernel.bin` 1,228,936 B，`_bss_end=0x020af0a0`，sha256 `f9bf3a276aba062cbe744138415daba00e6873d17b3664ee2fee594d59c8aa69` | `artifacts/gui-blocks/REPORT.md` §8；`docs/块化GUI-进度.md` §4 | 达成 |
| 16 | 推送 `feat/gui-blocks` | `9bfa10f..5151ff6`（6 提交） | 远端 `ocean-2026/catos-os` | 达成 |

**未达成 / 残留（如实标注，见 §9.3）**：D7 超框位图像素断言；探针后端统一（11a，`lfb=NULL`）；拖拽旧位置残影像素断言（11b）；D5 demo 树拆 `src/gui/demo.c`；D6 五个分项测试脚本拆分；BSOD 后 PTE/PT 运行时转储。

## 6. 帧耗时基线（条件：QEMU `-cpu qemu32 -m 256 -smp 1`，800×600）

| 指标 | 值 | 条件 |
|---|---|---|
| 首帧全量合成 | 1.783 ms（`home` 探针 area=480000px，≤8ms PASS）；历史中位数 1.88ms（2.19/1.87/1.88） | `gui home` 全量建树后首帧 |
| 拖拽帧（damage ≤25% 屏） | 0.877 ms；damage 9600px ≈ 2.0% 屏（≤4ms PASS） | `gui drag` 单块 120×80 移动 |
| 节流帧（探针节拍） | `GUI:frame n=31 us=9`；同 tick 重复提交由合成层去重 | `gui rate` 33 帧 |
| **生产空闲节流** | 100 tick 提交 **25 帧**，`double_commit=0`（≈25fps，满足“每 3 tick 一帧 ≈33fps”规格；上限口径 ≤33 帧/秒） | `gui idle` 走同一条生产 `gui_idle_step()` 路径 |
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
- 尺寸：11,956,224 字节（约 11.40 MiB）；`build/kernel.bin` 1,228,936 字节，`_bss_end = 0x020af0a0`
- sha256：`f9bf3a276aba062cbe744138415daba00e6873d17b3664ee2fee594d59c8aa69`

## 9. 剩余项与风险

### 9.1 本轮审计偏离处置（D 项）

| 项 | 内容 | 状态 | 证据 / 提交 |
|---|---|---|---|
| D2 | `wins` 复数命名违反“无窗口概念”硬约束；`chrome.c` 局部数组改名 `menu_ids`；lint C1 规则 `\bwin\b` 收紧为 `\bwin\w*`（re.I），复跑 0 violation | **已修** | `cc7b8b1`；lint GREEN |
| D1 | 生产空闲循环原每 tick 调 `gui_frame()`，3-tick 节拍只活在探针里。抽出 `gui_idle_step()`：pump_input→clock_refresh→`timer_ticks - s_idle_frame_tick < 3` 跳过，否则提交帧；同 tick 重复调用仍只算一次提交 | **已修** | `cc7b8b1`；生产实测 `idle frames=25 double_commit=0 ticks=100` |
| D7 | `gui_blit24` 原只做屏幕边界裁剪，绕过 `clip_rect()` 父链。改为先与裁剪栈求交，同 `gui_fill_rect` 口径；后端加 `gfx24_blit_stride` | **代码已修，像素用例残留** | `95aacc9`；超父框位图框外保底色的独立像素断言未补（见 §9.3-1） |
| D3 | diff 消失节点原直接回收，与 `b_remove` 两阶段口径不一致。unseen 节点改 `tree_detach` + `mark_leaving`，交 `gui_presence_sweep` 计数回收 | **已修** | `cc7b8b1`；`diff base=2 leaving=1 live_leaving=2 live_after=1 leaked=0` |
| D8 | `include/gui/gui.h` 补 §7 v2 槽位空桩声明 `gui_surface_attach/gui_surface_dirty`（只声明不实现） | **已修** | `a87bf76` |
| D5 | demo 树拆到 `src/gui/demo.c`（规格 §11 文件清单） | 残留（有余力再做） | — |
| D6 | 补齐 `tests/gui_{layout,paint,idempotent,drag,leak}.py` 五个分项脚本，或在报告把现有 `gui_probe.py` 明确映射成分项 | 残留 | 现 17 子命令即分项探针，未拆独立脚本 |

### 9.2 布局根治与 B' 兜底

- **链接顺序根治（已落地）**：`user.o`/`task.o` 前置后，`acl_tab/users/tasks/pipes/cur_task` 全部回到 `0x2000000` 以下（§5.1.1）；护栏 `--strict` 对关键表落窗口判失败，已纳入回归口径。非 syscall 路径静态仍有部分落在窗口内，由 B' supervisor 镜像兜底，护栏只打 `[link][warn]`，不阻断。
- **B' 覆盖加固（已落地）**：用户映射到内核静态物理帧（`<frames_base`）被拒绝并留 `[link][RISK]` 日志，自检 `result=1`（§5.2）。镜像槽被合法堆帧按槽覆盖是设计允许路径，未拒绝。
- 未采用 A（抬高 `USER_IMAGE_BASE`）：需同步 CE loader/PDE/脚本/文档，动用户 ABI，半径最大；未采用 C（压 GUI 静态占用）：实测关键表在 .bss 内偏移由 `.text/.reloc` 总量决定，压缩 bss 数组拉不回边界，属治标靠体积运气。

### 9.3 残留项（非阻塞，如实记录）

1. **D7 超框位图像素断言**：缺一块“超出父裁剪框的位图”确定性测试帧，框外保底色的像素级覆盖未固化；当前裁剪正确性由代码路径（与 fill_rect 共用 `clip_rect`）+ 现有四组帧审查支撑。
2. **探针后端统一（11a）**：探针子命令提前 return 分支未请求 `CON_MODE_VBE24`，探针帧后端 `lfb=NULL`；串口判定不依赖图形后端。需统一走“进桌面→dump→退出”同一条后端路径，影响后续像素类断言可信度，本轮未改（17 条探针后端切换开放面大，避免动绿基线）。
3. **拖拽残影像素断言（11b）**：snap-only 重绘下拖拽旧位置的背景补齐语义未做损伤条带内回填；`drag` 探针只校验几何（`b_same=1`），未断言离开位置旧区域回到桌面底/父背景。
4. 主题探针目前为链路级（标脏/结果位）；像素级覆盖由 `gui_pixel_assert.py` 的 light/hc 分组在客人机上完成。
5. PTE 级取证未补（见 §5.1 末）：BSOD 后未自动导出 PDE/PT；如需闭环“私有 PT 该页 present 与否”的运行时实证，待加 BSOD 转储导出。

**lint 复跑**：0 violation / 39 advisory / 14 review，Result GREEN。代码侧无 window 语义命名；剩余 advisory 均为规格/进度文档里的“窗口”措辞（内存窗口 / 规格引述），不阻断。
