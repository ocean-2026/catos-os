# CatOS 内核问题清单（CE 用户态开发视角）

> 汇总 ot（ocntools.ce / ocean_tools）系列开发以来遇到的内核侧限制、bug 与怪癖。
> **维护约定：每次开发遇到新的内核问题，就在对应分类下追加条目**（含确认值、影响、规避方式、发现版本）。

---

## 一、内存与执行模型

> **编号命名空间说明（2026-11-09 落档补充）**
> 本节及第四节使用**主清单编号**（一~八节连续编号 `#1`~`#46`）。
> 2026-11-09 按合并要求落档 D-1~D-15 时，第一节续编 `#48`~`#50`、第四节续编 `#51`~`#65`；
> 同日移植 Go 解释器时暴露 `u2k_str()` 的 argv 校验缺陷，第四节续编 `#68`；
> 同日核实 Go 解释器"死循环"实为模拟器缺陷，续编 `#69`~`#72`（`ce_sim` 的 CF 锁存缺口、
> group3 缺内存操作数、实机验收脚本三处自身缺陷、`atoi` 未建模）。
> 2026-11-10 合流续编 `#73`（干净克隆截图竞态）与 `#74`~`#77`（第十一节：
> `task_pages_of()` 页数虚高、文档状态滞后、QEMU wav 头未 finalize、
> SB16 驱动接入时 int 0x80 关中断陷阱）。
> 第九节的 `#48`/`F1`/`A2`/`#3` 与第十节的 `#47` 属于**各自独立的局部命名空间**
> （第九节是 b31 修复闭合表、第十节是工具链常量核对表，均已在节首自行声明），
> 与主清单编号**不冲突、不互相引用**。引用时请连节号一起写（如"第四节 #61"）。

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 1 | CE 镜像大小上限 | **v2.5 起 262144 字节（256 KiB）**：`include/ce.h` 的 `CE_MAX_IMAGE=0x40000`，`CE_MAX_BSS=0x40000`；v1.x 的 65536 字节是旧 `mkce.py` 的检查值 | ocntools.ce v1.6=60120B、v1.7=57400B 曾贴近旧上限；v2.5 自举产物 CC2.CE≈82KB 被旧检查直接拒掉（见 #47） | 工具链上限从 ce.h 运行时读取，不再单独硬编码；体积敏感代码仍可外置 .csc | v1.0 起 / v2.5 更新 / 2026-11-06 修正 | 已确认/已修正 |
| 2 | 内核栈仅 8KiB | 8 KiB | CE 程序大栈帧会踩栈（v1.6 B1：run_script_args 栈上 buf[8192]+lines[128][97]≈21.4KB） | 大缓冲必须文件级 static；v1.6 已把 CSC 解释器缓冲 static 化 + g_csc_active 防重入 | v1.6 | 已确认/已修复 |
| 3 | 堆为 64KiB 静态池 | api->alloc/free，64 KiB 静态池 | alloc 失败即池耗尽；大块分配需及时 free | alloc 后立即 free；脚本里 alloc/free 配对 | v1.7 | 已确认 |
| 4 | 无固定地址 alloc + 固定链接基址 0x02000000 | 链接基址固定，无固定地址分配 | CE 程序无法动态加载/运行其他 CE（ot 不能 run .ce） | cecheck 作为用户态安全等价物（不依赖内核加载器） | v1.2 评估 | 已确认/已规避 |
| 5 | api->exit() 有 bug：重载同一镜像 | exit() 会 relaunch 同镜像 | 调用 exit() 导致程序反复重启 | ce_main 直接 return 0，禁止调 api->exit() | v1.0 起 | 已确认/已规避 |
| 6 | 无浮点运算 | 内核 API 无浮点 | 数学只能整数 | 全部整数运算 | v1.0 起 | 已确认 |
| **48** | **`heap_ensure()` 只映射 1 页 ⇒ 跨页分配触发 `#PF` 杀任务**（原项目的 `heap_ensure_range()` 在 VMA 重构时丢失） | 现项目 `kernel_v25/src/task.c:706` `heap_ensure()` 每次只 `kzalloc_frame()` + `task_map_page()` **单页**；全仓 `grep heap_ensure_range kernel_v25/` → **0 命中**；原项目 `src/task.c:661` 有多页版 `heap_ensure_range()` | 分配跨越 4 KiB 页边界时未映射页被访问 ⇒ `#PF cr2=0x02101007` ⇒ 任务直接被杀（PM 已实机复现） | 移植原项目 `heap_ensure_range()`（约 20 行）；**必须放在 `heap_ensure()` 定义之后或加前置声明**，否则 `implicit declaration` 编译错误 | v2.10 VMA 重构后回归 / 2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **49** | **`USER_STACK_SZ` 相对原项目倒退**（自举需要 128 KiB） | 现项目 `kernel_v25/include/paging.h:82` `USER_STACK_SZ 0x10000u`（**64 KiB**）；原项目 `include/paging.h:35` `0x20000u`（128 KiB，原注释："2026-11-02: CC2.CE self-host needs >64K"） | 自举编译（`cc.ce` 编译自身）栈空间不足；大栈帧 CE 程序踩栈 | 改 `0x20000u` | 原项目 2026-11-02 引入 / 现项目未同步，2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **50** | **`USER_HEAP_MAX` 相对原项目倒退**（自举需 ~380 KiB） | 现项目 `kernel_v25/include/paging.h:80` `USER_HEAP_MAX 0x40000u`（**256 KiB**）；原项目 `include/paging.h:33` `0x80000u`（512 KiB，原注释："2026-10-31: cc.c self-host needs ~380K"） | 自举编译器堆不够用（arena 申请失败 → 编译中断） | 改 `0x80000u` | 原项目 2026-10-31 引入 / 现项目未同步，2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |

## 二、文件系统（FAT32）

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 7 | save() 单次写入上限 | ≤512 字节/次 | 大文件无法一次保存（vcs/edt 需分片） | 分片保存（shards）；CSC 侧配合 save 分片 | v1.4（vcs/edt 集成） | 已确认 |
| 8 | cat/load 读取截断 | ≤8KB | 超过 8K 的文件读不全（CSC 脚本同限） | 脚本 ≤8K/≤128 行；大文件用工具分片读 | v1.0 起 | 已确认 |
| 9 | 路径段长度上限 | ≤31 字符（FAT 短名） | 长文件名不可用 | 命名控制 ≤31 | v1.0 起 | 已确认 |
| 10 | resolve_existing 解析绝对路径最长 | ~127 字符 | 深路径装入小 buffer 会越界（v1.6 M2：vcs base[96] 等 5 处） | 槽位按 ≥160 预留（已修 base[160]/path[160]） | v1.6 | 已修复 |
| 11 | 目录查找大小写不敏感 | resolve_existing 两级解析（cwd+/bin） | 路径大小写随意 | 解析层统一处理；fatfs 写入路径段大写 | v1.0 起 | 已确认 |
| 12 | disk.img 路径段规范 | 路径段大写（FAT32） | 小写段可能解析歧义 | fatfs.py 操作时路径段大写 | v1.0 起 | 约定 |

## 三、显示与输入

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 13 | 屏幕规格 | 80×25 字符、16 色、无浮点 | 全屏工具布局受 80 列限制（状态栏 >79 列会折行） | 状态栏压 ≤79 列；整屏重绘约束内 | v1.4 | 已确认 |
| 14 | 无背景色 API | set_fg 无背景色位 | fmgr 选中行无法真反白 | 亮白 + `>` 前缀近似（审美评审认可） | v1.4 | 已确认/已近似 |
| 15 | CE 键盘键码 | UP=0x100 / DOWN=0x101 / TAB=0x09 / ESC=0x1B（api->getch） | 方向键是非 ASCII 值 | 按键分支按 CE 键码判 | v1.2 起 | 已确认 |
| 16 | 等宽字体换算基准 | 1 空格 = 1 字符 = 9px；1 行 = 16px | 图形/布局换算（如标题居中、plot 坐标） | 以 9px/16px 为基准计算 | v1.2 | 已确认 |

## 四、工具链与加载

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 17 | mkce.py 的 bss_size quirk | 把 readelf 的 .bss **文件偏移**当 bss_size 写入 CE01 头（真实 .bss=126224B） | 头字段值可疑，实测运行正常 | **2026-11-06 核对**：本快照两份 `mkce.py` 都不解析 readelf；bss 由 `build_ce.sh` 取 `objdump -h` 第 3 列（**Size**，非 offset）作参数传入，无此 bug；另在构建期增加 bss 上限校验（见 #47） | v1.4 起 | 已核对/本快照不适用 |
| 18 | CE01 头 / 基址 / 重定位 | magic/entry/bss_size/api_offset/image_size 实测正确；基址 0x02000000；无重定位 | 正常 | — | v1.6 审查 | 已确认 |
| 19 | QEMU sendkey 丢键 | 空格/字母/回车偶发丢失（测试环境噪音） | 长命令粘连、验证误判 | 慢速发送 + vga 屏幕回显校验 + BS 清行重试 | v1.0 起 | 环境噪音（非内核） |
| 20 | fatfs.py 工具 bug/口径 | dump_tree 递归 TypeError 勿用；read_chain 参数是簇链 list 非 slot 号 | 误用会崩或读错 | 用 add_file/delete_file/read_chain（簇链） | v1.0 起 | 工具备忘 |
| 21 | 测试环境每轮重置 | Ubuntu 云环境 qemu 等配置可能每轮消失 | 直接测试会失败 | 开工先检查 qemu/gcc/git 配置（AGENTS.md 规则） | v1.0 起 | 环境规则 |
| **51** | **`cc.c` 停在阶段 0，阶段 1-3 能力在 `cc_self.c` 里却是死代码** | `kernel_v25/extapp/cc.c` **1,237 行**（阶段 0：无 struct/typedef/enum/switch/goto、无 `no ce_main` 报错）；`kernel_v25/extapp/cc_self.c` **2,250 行**（阶段 1-3 已实现）。全仓 `grep -rn cc_self kernel_v25/ tests/ --include=*.sh --include=*.py` → **0 命中**（无任何构建脚本引用 `cc_self.c`） | 改 `cc.c` 源码得不到阶段 1-3 能力；`cc_self.c` 的 2,250 行是全树无引用的死代码 | **用户拍板：用 `cc_self.c` 覆盖 `cc.c`**（两份均归档保留）；替换时需核对 `TOK_MAX`/缓冲尺寸与 `OB_MAX`/`SRC_MAX` 是否适配 512 KiB 堆 | 原项目 `TODO_STAGE5.md` 记录 / 2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **52** | **`CC2.CE` 自举固定点（cc2 → cc3 字节级一致）未验证** | 原项目 `build/HOSTCC2_REF.CE` = 81,522 B / md5 `eeec32f838fbab4c111d428df22aa409`（Linux 机器编译的期望输出）；本机构建 `build/CC2_NEW.CE` = 82,586 B / md5 `d21d66ea5845492dab406d04f666c60e`（= 已从根目录删除的 `Catos_Better_b`）。二者**字节级不一致** | 「cc 能自举」目前只是"两个产物大小接近"，没有固定点证明 | 跑 `build/verify_bootstrap.sh`（移植并改写为相对路径）：cc2 编译 cc.c → CC3.CE，与 CC2.CE 比 md5；**gcc 版本差异（Rev4 vs Rev5）会导致字节不一致，需先记录参考值的工具链版本** | 原项目 `BUILD_REPORT_STAGE5.md` / `HANDOFF_STAGE5.md` 记录 / 2026-11-09 核实 | 待核对（PRD **P2-1**） |
| **53** | **Go 解释器 FIND-G3：无递归深度上限** | 原项目 `extapp/go.c:783` `g_call_depth` + `:784` `CALL_DEPTH_MAX 200` + `:1048` 入口检查（**源码已修**）；但 `docs/go-review.md` 仍记为"未修" | 无限递归撑爆 C 栈（CE 用户栈仅 64 KiB，与 #49 同源） | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G3 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **54** | **Go 解释器 FIND-G4：非法字符被词法 default 静默吞** | 原项目 `extapp/go.c:358-362` default 分支置 `g_perr`/`g_errline`（**源码已修**）；但 `docs/go-review.md` 仍记为"未修" | 非法字节不在词法期报错，而是延后变成运行时"undefined variable" | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G4 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **55** | **Go 解释器 FIND-G5：`readfile` 请求 20479 > 内核 `file_read` 单次 16384 上限** | 原项目 `extapp/go.c:1085` 新增 `read_whole()`，按 `FS_CHUNK 16384` **分块循环读至 EOF**（**源码已修**）；但 `docs/go-review.md` 仍记为"未修"。内核 `ce_abi.h` 注释：`file_read ... max <= 16384 per call` | 未修则 >16 KiB 的脚本被拒（`got<0` → `""` 或 "cannot read script"） | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G5 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **56** | **Go 解释器 FIND-G6：arena 扩容 `s->v`/`a->v` 未判空** | 原项目 `extapp/go.c:701` 与 `:732` 补 `if (!s->v) { g_oom = 1; return 0; }`（**源码已修**）；但 `docs/go-review.md` 仍记为"未修" | arena 耗尽时 `s->v/a->v = 0`，后续 `scope_declare`/`arr_push` 解引用空指针 | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G6 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **57** | **Go 解释器 FIND-G7：字符串字面量 >1023 静默截断、未闭合引号不报错** | 原项目 `extapp/go.c:223-235` 改为**两遍扫描**（先数解码长度再按 arena 尺寸分配），未闭合引号报词法错误（**源码已修**）；但 `docs/go-review.md` 仍记为"未修" | 长字面量被静默截断；引号不闭合导致后续解析错乱 | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G7 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **58** | **Go 解释器 FIND-G8：顶层 break/continue/return 静默停跑不报错** | 原项目 `extapp/go.c:188-191` 解析期嵌套计数器 `g_loop_depth`/`g_func_depth` + `:658/:665/:669` 三处 `depth==0` 报错（**源码已修**）；但 `docs/go-review.md` 仍记为"未修" | 脚本在顶层误用 `break` 会静默终止，后续语句不执行 | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G8 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **59** | **Go 解释器 FIND-G9：for init 的 `:=` 变量泄漏外层作用域** | 原项目 `extapp/go.c:882-899` 每个 for 循环建独立 `Scope`（parent = 调用者作用域），结束恢复 `g_scope`（**源码已修**）；但 `docs/go-review.md` 仍记为"未修" | 循环变量泄漏到外层，与 Go 语义不符（原评为"无害"） | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G9 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **60** | **Go 解释器 FIND-G10：数字字面量溢出无检测（int32 UB）** | 原项目 `extapp/go.c:302-313` 扫描时检测 `> 2147483647` 并置词法错误（**源码已修**）；但 `docs/go-review.md` 仍记为"未修" | 超大字面量静默溢出为负数/回绕 | 移植 `go.c` 后逐条确认该修复随行 | 原项目 `go-review.md` FIND-G10 / 2026-11-09 核实 | 原项目已修/移植已验证（2026-11-10 复核） |
| **61** | **`kernel_v25/build/catos_work.iso` 从未入库 ⇒ 干净克隆不可构建** | `git ls-files kernel_v25/build/` 仅返回 `gen_builtin.py` / `mkce.py` / `patch_work_iso.py`；`kernel_v25/build/patch_work_iso.py:5` 硬依赖 `build/catos_work.iso`（10,725,376 B GRUB El Torito 模板）；`.gitignore` 的 `!kernel_v25/build/catos_work.iso` 否定规则写在 `*.iso` **之前**（第 7/12 行 vs 第 27 行），被覆盖 | 干净克隆执行 `build.sh iso` 抛 `FileNotFoundError: build/catos_work.iso`；**当前"基线 12/12 PASS"依赖手工拷贝的未跟踪模板，不具备 CI 意义** | 把模板提交入库**并**把 `!` 否定规则移到 `*.iso` **之后**（git 是 last-match-wins；只 `git add -f` 不移位是假修复） | 2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **62** | **`build.sh` 应用列表不含 `cc` ⇒ `cc.ce` 是陈旧产物** | `kernel_v25/build.sh:43` 应用列表 = `hello calc sysinfo demo curl netinfo spinner crash perms hw gfxdemo zhdemo mmaptest`（**无 `cc`**）；`kernel_v25/extapp/bin/cc.ce` = 26,952 B（入库值），源码重编应为 27,080 B | 改 `cc.c` 源码**没有任何效果**，却因"产物在库里"看起来像已验证 —— 伪装成已验证 | 把 `cc` 加入 `build.sh` 应用列表（`gen_builtin.py` 的 `OPTIONAL` 已含 `/bin/cc.ce`） | 2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **63** | **`node.c` 从不被任何构建脚本编译** | `grep -rn node kernel_v25/build.sh kernel_v25/extapp/build_ce.sh` → **0 命中**；`kernel_v25/node/node.c`（1,414 行）与 `node.ce`（35,116 B）**均已入库**，且与原项目 `node/node.c` diff **0 行差异**、`node.ce` **逐字节相同** | `node.ce` 是**无法复现的预编译产物**（与 #62 同构）：一旦有人改 `node.c`，镜像里的 `node.ce` 会静默过期 | 把 `node` 加入 `build.sh` 应用列表；补独立验收 `tests/node_regress.py`（QEMU 内 `run /bin/node.ce` 跑 `TEST.JS`） | 2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **64** | **`tests/inject_any.py` 的 FAT 几何参数过时** | 脚本 `tests/inject_any.py:18-21` 硬编码 `SPC=8` / `RSVD=33` / `FAT_SZ=8192`（按 4 GiB 稀疏盘假设）；内核 `kernel_v25/src/fat.c:8` 实为 `RESERVED 32u`，且 `fat_sz`/`spc` 由卷大小动态决定（`fat.c:635/647/650`）。该脚本与原项目 `tools/inject_any.py` **逐字节相同** | 任何"注入新应用再验证"的流程**第一步即失败**（报 `BIN dir not found`），极易被误判为"移植不成功" | 改为从 DBR 读取几何（PM 已用读 DBR 的版本验证可正常注入） | 2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **65** | **`.gitignore` 否定规则位置写反（dead negation）** | `.gitignore:7` 与 `:12` 有 `!kernel_v25/build/catos_work.iso`，但 `.gitignore:27` 是 `*.iso`；git 是 **last-match-wins** ⇒ `git check-ignore -v kernel_v25/build/catos_work.iso` 命中的是 `.gitignore:27:*.iso`（**忽略规则**），否定规则形同虚设。注释写的 *"exceptions must precede rules"* **结论是错的** | 构建产物跟踪状态脆弱；下次有人删了重建该文件会再次被 `*.iso` 忽略，构建可复现性再次失效 | 把否定规则移到 `*.iso` **之后**，并把注释改成符合 git 语义的表述 | 2026-11-09 核实 | 已修复（合流 6945860 / 2026-11-10 复核） |
| **68** | **`u2k_str()` 用整段 `max` 字节窗口校验用户字符串 ⇒ argv 传参路径全线失效** | 现项目 `kernel_v25/src/task.c:668` 的 `u2k_str()` 一次性校验 `[us, us+max-1]` 整段（`max = FS_PATH_MAX = 160`），而 `task_spawn_ce()` 把 argv 字符串**放在用户栈顶**（`sp = USER_STACK_TOP - 0x10`，逐个 `sp -= l+1`）⇒ 160 字节窗口必然越过 `USER_STACK_TOP` ⇒ `validate_uaddr()` 的 `vma_contains(t->vm, end)` 失败 ⇒ 返 -14 ⇒ `u2k_str` 返 -1 ⇒ `SYS_FILE_READ` 返 **-3**、`SYS_FILE_EXISTS` 返 **0**（"不存在"）。**第二重缺陷**：修前 `dst[i] = us[i]` 是从 ring0 **直接解引用用户指针**（读内核地址空间）；CE image 恰好被恒等映射所以能 work，用户栈**没有**恒等映射。**原项目**（`D:/CatOs/Catos_Better_better/src/task.c:618`）用的是**单点 arena 边界检查**（`USER_IMAGE_BASE <= us < USER_ARENA_END`），**没有 160 字节窗口** ⇒ 原项目不触发此缺陷 | `run /bin/go.ce /BIN/TEST.GO` 实机答 `go: cannot read script: /BIN/TEST.GO`，但该文件**确实在 FAT 卷上**（独立宿主侧 FAT32 解析器逐簇核对内容正确）。**决定性对照实验**：同一路径，字面量（在 image VMA 内）`exists=1 / file_read=592` **成功**，argv（在栈顶）`exists=0 / file_read=-3` **失败** ⇒ 缺陷在内核字符串拷贝路径，与 Go 解释器无关。**影响 7 条 syscall**：`FILE_READ`/`FILE_EXISTS`/`FILE_WRITE`/`SAVE`/`SPAWN` 的 argv 拷贝 + 网络的 `url`/`host`/`ip` 三张参数表 —— 全部是同一 bug 的受害者。原项目当年验 go 只走**无参内置 demo 回退**（`go.c:1215`），从未走 argv ⇒ 缺陷一直潜伏 | 逐字节校验（`validate_uaddr(t, a, 1)`）+ 经任务页表翻译读取（`task_virt_to_phys()` + 内核恒等映射别名），不再从 ring0 直接解引用。**2026-11-09 已修复**（commit `38fee6a`），修复后 `run /bin/go.ce /BIN/TEST.GO` → `ALL PASS`、`run /bin/node.ce /BIN/TEST.JS` → `ALL PASS`（两条都依赖 argv 传参，修前必然失败） | 移植 Go 解释器时暴露 / 2026-11-09 核实 | **已修复** |
| **69** | **`ce_sim` 的 `cmp al, imm8`（0x3C）不锁存 CF ⇒ Go 解释器被误判为"死循环"** | `tests/ce_sim.py` 对 `cmp` 的 0x38/0x39/0x3A/0x3D 四种形式都维护了 CF，**唯独 0x3C（`cmp al, imm8`）漏了**。gcc -O2 把 cc 的 `is_idc()` 内联成 `and $0xffffffdf,%eax / sub $0x41,%eax / cmp $0x19,%al / jbe`，而 **`jbe` 读的就是 CF 锁存器** ⇒ CF 保持陈旧值 ⇒ 模拟的 lexer 走错分支 ⇒ 看起来卡死在 tokenizer 循环 | `ce_regress` 把 `go.ce` 判成 FAIL，极易被误读为「Go 解释器有死循环缺陷」而重写实现。**真实结论相反：Go 解释器本身没有死循环** | 补上 `self.cf = 1 if al < (imm & 0xFF) else 0`。修复后 `go.ce` 在 `ce_sim` 下 **COMPLETED，246041 步，rc=0**，`g_pos=563/563`、`g_tokn=200`（源码读完、200 个 token） | 移植 Go 解释器时暴露 / 2026-11-09 核实 | **已修复**（commit `7f8b225`） |
| **70** | **`ce_sim` 的 group3 子指令组只实现寄存器形式 ⇒ 缺内存操作数即中止** | `tests/ce_sim.py` 的 `op_group3()` 开头是 `if mod != 3: raise CeError("group3 memory operand not emitted")`，即 `test/not/neg/mul/imul/div/idiv`（opcode 0xF6/0xF7）**只有寄存器形式**。gcc 为 32 位有符号除法发出的是 **`idiv dword ptr [ebp-0x18c]`**（实测字节 `f7 bd 74 fe ff ff`）⇒ 模拟器直接抛错中止 | 与 #69 叠加后，Go 解释器在 `ce_sim` 下完全无法推进；此前把这一现象整体归因为"解释器死循环"，掩盖了两处**模拟器**缺陷 | 补全 group3：内存操作数形式、8 位变体（0xF6）、32 位除法溢出检测、`test` 的立即数形式；新增 `sx8`/`sx16`/`trunc_div` 辅助函数与 `of` 标志 | 同上 | **已修复**（commit `7f8b225`） |
| **71** | **实机验收脚本自身的三处缺陷 ⇒ 回归报告的 ERR 与内核无关** | ① `kill_qemu()` 用全局 `taskkill /F /IM qemu-system-i386.exe`，会杀掉**整台机器上所有** QEMU 进程（含并发驱动正在跑的 guest）⇒ 幸存方的 HMP monitor socket 以 `WinError 10054`（远程主机强迫关闭了一个现有的连接）断开。② `tests/go_regress.py` **从未把 `.GO`/`.JS` 用例注入磁盘**，解释器必然答 `cannot read script` ⇒ 属**驱动缺陷**而非内核缺陷。③ 内核 shell 的清屏命令是 **`clear`**（`shell.c:1361`），`cls` 不在分发表里、只会打印 `unknown command: cls` 且不清屏 ⇒ 断言会匹配到上一屏的**残留文本**；shell **没有 `cat` 命令**（可用命令见 `shell.c:1378` 起的分发表），确认文件存在只能用 `ls /bin` | 回归报告出现成片 `WinError 10054` 与 `cannot read script`，会被误判为"内核崩溃""移植失败"，实测同一脚本两次跑出完全不同的结果（4/7 vs 2/7） | ① `kill_qemu(proc)` 改为按 PID 精确终止；② 改为**两遍启动**（pass1 格式化 → `inject_any.py` 注入 → pass2 跑验收）；③ 清屏改 `clear`、文件确认改 `ls /bin`；④ 新增 `wait_for_prompt()` 轮询 VGA 文本缓冲等 `catos:<pwd>$` 提示符（固定 sleep 不够，命令会在 shell 监听前被丢弃；提示符匹配必须锚定形状，GRUB 菜单里也有 "catos" 字样） | 移植 Go 解释器时暴露 / 2026-11-09 核实 | **已修复**（commit `7f8b225`；配套 `tests/qemu_driver.py` 抽取共享实现） |
| **72** | **`ce_sim` 列了 `atoi` 却未实现 ⇒ Go 解释器读第一个数字常量即中止** | `tests/ce_sim.py:32` 的 `API_NAMES` 含 `"atoi"`，但 `call_api()` **没有对应分支**，走到末尾抛 `api 'atoi' is not modelled by ce_sim`。Go 子集解释器用 `atoi()` 解析脚本里的数字常量 | 模拟器侧**整个自检语料库不可达**：`TEST.GO` 在第一个数字处就中止，"ALL PASS" 这条最强断言永远跑不到 | 按 `strtol(s, NULL, 10)` 语义实现（跳空白、可选符号、读数字、截断到 32 位）。修复后 `tests/go_cases.py` → **cases=5 ok=5 fail=0**：`TEST.GO` 逐项 `PASS arr-write/arr-read/arr-sum/str-len/str-sub/str-at/atoi/chr/str-concat-atoi` → **ALL PASS**；`SYN.GO` 正确报 `syntax error at line 3 (tok#15/20)` 且 rc=1 | 移植 Go 解释器时暴露 / 2026-11-09 核实 | **已修复**（commit `59617ff`） |
| **73** | **`qemu_regress.py` 的 `shot()` 不等 screendump 落盘 ⇒ 干净克隆下 12 步全报 ERR** | HMP `screendump` 是**异步**的：monitor 先回显命令、guest 再写文件。`tests/qemu_regress.py` 的 `shot()` 固定 `sleep(1.2)` 后直接 `open(ppm)`。在主工作树里磁盘早已格式化过、写入很快，1.2 s 够用；在**干净克隆**里磁盘是刚创建的稀疏空文件、写盘慢 ⇒ 文件尚未出现 | **症状极具误导性**：12 个步骤全部 ERR（`FileNotFoundError: ...00_boot.ppm`），看起来像"guest 挂了/内核崩了"，实际 guest 一路正常跑到最后一步。这个假象曾让我把大量时间浪费在"内核是否在 QEMU 下崩溃"的错误方向上 | `shot()` 改为**轮询等待文件出现**（最多 ~20 s），真没有时报明确的 `screendump produced no file`；同时先删除旧 `.ppm` 避免读到上一帧 | 干净克隆（空白磁盘）终验 / 2026-11-09 核实 | **已修复**（commit `3fd56cb`；修复后干净克隆 12/12 PASS） |


### Go 子集解释器（`go.c`）现状 —— 2026-11-10 合流后更新

> 2026-11-09 的原文写「现项目 `kernel_v25/` 中当前没有 `go.c`」，该描述**已在合流后失效**，
> 以下为当前真实状态（复核于 `D:/CatOs` master `6945860`）。

- **已移植**：`kernel_v25/extapp/go.c`（1,319 行）与 `extapp/bin/go.ce`（26,832 B）均已入库；
  `kernel_v25/build.sh:47` 的 CE 应用列表含 `go`，`build/gen_builtin.py:32` 的 `FILES` 含 `/bin/go.ce`。
  实机 `run /bin/go.ce /BIN/TEST.GO` → `ALL PASS`，`fib(10)= 55`。
- **CE ABI 兼容性**：原项目是 **48 槽**，合流时现项目为 **57 槽**（v2.11 起 61 槽）。
  `go.c` 只调用 7 个槽（`write`/`putc`/`print_int`/`alloc`/`atoi`/`file_exists`/`file_read`），
  索引**全部 ≤39**，落在 append-only 旧前缀内 ⇒ 57 槽、61 槽下均零改动可编译。
- **#53~#60（FIND-G3~G10）**：8 项修复随移植保留（宿主 `tests/go_cases.py` 5/5、
  实机 `tests/go_regress.py` 7/7）。`docs/go-review.md` / `GO-DESIGN.md` §6 仍写"未修"，
  属**原项目的历史记录**，不代表当前源码状态。
- **移植时顺手修掉的一处**：静态 64 KiB arena 兜底原本按 `ARENA_SZ`(192 KiB) 判边界，
  动态 arena 只有 64 KiB 时会越界写；已引入 `g_arena_cap` 让边界跟随实际容量。
  `readfile` 的 20480 B 缓冲与 `file_read` 单次 16 KiB 上限的落差由 `read_whole()` 分块解决。


## 五、未决问题

| # | 问题 | 说明 | 状态 |
|---|---|---|---|
| 22 | 用户曾问 "512k 是哪个" | 已知确认值仅 **512B**（save 上限）；未在历史轨迹中确认 512K 出处（可能是显示/显存或其他资源的 512K，也可能是笔误） | 待查证 |

---

## 扩充规范

1. 遇到新的**内核侧**问题（API 限制、加载器行为、文件系统约束、显示/输入怪癖）→ 在此文档追加条目；
2. 每条必须含：问题、确认值、影响、规避/对策、发现版本、状态；
3. 状态值：已确认 / 已修复 / 已规避 / 待核对 / 环境噪音（非内核）/ 待查证；
4. 工具链问题（fatfs.py、QEMU 等）单独归入"工具链与加载"，与内核问题区分标注。

## 六、ADoFaI 游戏项目新增实测（2026-10-24）

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 23 | delay(ms) 基于 PIT get_ticks（milli_delay），墙钟准确 | milli_delay: `while(((get_ticks()-t)*1000/HZ)<ms)` | delay 可作游戏主时钟（1ms 档） | 游戏时钟=delay(1) 计数 | adofai 探针 | 已确认 |
| 24 | RDTSC 跨状态速率不稳（忙等 vs 内核等待差 36~155 倍） | 忙等≈7.2M tick/s；getch 等待期间虚拟时钟速率剧变 | 不能作为跨 getch 的墙钟 | 弃用 RDTSC；只用 delay | adofai 探针 | 已确认 |
| 25 | CE getch（槽22）阻塞；键先入内核 kb_in 队列 | 无键时挂起，实测等待 60s+ 不返回 | 无超时→无法轮询输入 | 改用 PS/2 端口直轮询（见 #26） | adofai 探针 | 已确认 |
| 26 | CE 可直接端口 I/O（ring0）；屏蔽 IRQ1 后轮询 PS/2 可行 | PIC IMR 0x21 bit1；inb 0x64/0x60；实测 5 键全捕获 | **实时非阻塞输入通道**：任何 make 码=一次按键（帧级 1ms 精确） | 游戏内掩 IRQ1→轮询→退出恢复+排空 | adofai 探针 | 已验证（新能力） |
| 27 | 文本控制台 framebuffer 不在 0xB8000 | pmemsave 0xB8000 为陈旧内容，与 screendump 不符 | 不要用 0xB8000 推断屏幕状态 | 用 screendump / 屏幕回显校验 | adofai 探针 | 已确认 |
| 28 | 读取内核内存 [0x1000000..0x14918b4] 会破坏文本控制台状态 | 全区间扫描后控制台乱码（原因未明） | 避免扫描内核内存 | 游戏输入走端口轮询，不读内核内存 | adofai 探针 | 已确认/已规避 |
| 29 | typecmd.py/type_slow.py 数字键码 off-by-one | DIGITS 映射把 '2' 发成 '3' 的键码（0x02+i 应为 0x02+i-1） | 含数字的命令会输错 | 已修复两个脚本 | adofai 项目 | 已修复（工具） |
| 30 | QEMU 运行中宿主侧 fatfs 注入会被 VM 写回缓存覆盖 | 运行时 add 的文件 VM 看不到 | 测试注入无效 | 注入前停 VM，注入后重启 VM；add_file_safe 重复注入会生成重复条目 | adofai 项目 | 工具备忘 |
| 31 | CE 菜单阶段（内核 getch）按键随机丢失：单独发 '1' 可启动游戏，伴随其它按键或快速连发时 '1' 频繁丢失；游玩阶段屏蔽 IRQ1 直轮询 PS/2 则 100% 可靠 | 疑似 shell 后台任务与 CE 竞争内核 kb_in 队列，或 IRQ1 路径快速按键溢出 | CE 应用应全程屏蔽 IRQ1 直轮询 PS/2（菜单/结果/游玩统一），readline 等内核输入路径临时恢复 IRQ1 | adofai 项目实测 | 已确认/已规避 |
| 32 | mkce.py 的 readelf .bss 列解析 bug（取 offset 当 size）→ CE01 头 bss 字段偏小，内核只清零部分 BSS，静态数组尾部为垃圾值 | readelf -S 第5列 offset vs 第6列 size | 已修复（adofai/mkce.py 取第6列）；**2026-11-06 核对**：本快照两份 mkce.py 无 readelf 解析路径，bss 取自 objdump Size 列，无需同步修；adofai 侧修复保留 | adofai 项目 | 已修复（工具）/本快照已核对 |
| 33 | QEMU monitor screendump 偶发不写文件（同一命令多次重试可成功），且 screendump 实际捕获状态比发出时刻滞后 ~1.5-2s | 工具备忘 | 测试脚本需重试 + 容忍滞后；判断状态用色彩统计（绿=游玩准度条/黄=胜利/红=失败屏） | adofai 项目 | 工具备忘 |
| 34 | QEMU 新实例 PS/2 键盘注入完全失效 | 新启动 QEMU 实例（12+ 实测）键盘 0% 接收率（HMP sendkey 键名/数字、QMP input-send-event、长 hold、bios.bin/bios-256k.bin、v1.8/v1.9 磁盘均无效），仅长时间运行的老实例部分正常（~80%） | VM 交互测试前先单键探测；备用：STARTUP_CMD 自动执行路径可绕过键盘（ot 启动后自动跑）；根因待查（疑似 CatOS 键盘驱动初始化时序/IRQ1 状态） | ot v1.9 | 环境问题/未决 |
| 35 | QEMU sendkey 注入存在不可控秒级延迟 + 键排队（2026-10-24 晚新证据） | sendkey 1 后 0.6s screendump 拍到的是注入前状态（菜单屏），键约 1~2s 后才被 CE 处理（此前 probe 轮"sendkey 1 → 失败屏"即为延迟送达）；菜单阶段 spc/数字键同样丢/延迟；10 连拍轮询 screendump（0.25s 间隔）全拍到同一失败屏=键未送达或排队未消费 | 节奏游戏"进关→LEADIN 1.2s→游玩"窗口截图时机失效：screendump 时游戏还没开始或已失败；精准跟拍判定词/胜利屏基本不可行 | ①screendump 轮询 + 色彩统计判断当前屏（绿=游玩准度条/黄=胜利/红=失败屏，单键即可区分）②连发键等延迟送达后消费 ③STARTUP_CMD 自动执行绕键盘 ④O 扫动准度条只能在 LEADIN 期间出现，属最稀缺画面 | 2026-10-24 adofai 实测 | 环境问题/未决 |

## 七、Catos_Better_better 内核能力现状（2026-10-27 用户上传版，只读核查）

| # | 能力/限制 | 确认值（源码证据） | 对 ot 的影响 | 处置 |
|---|---|---|---|---|
| 36 | 内核态权限系统（uid perms） | task.c:544-620 perm_tab/fs_perm_set/fs_perm_check 强制；SYS_SAVE 走 sys_save_uid（uid 检查失败返 -13）；fs_perm_seed 预设 /home /tmp 0777、/bin /appdevelop /etc 0755 | ot 用户态 ACL 与内核重复 → 已去除 ot 侧 ACL | 已去除重复；内核强制权限接管写保护 |
| 37 | **读权限内核未查**（cat 可读任何文件） | sys_save_uid 只查写；SYS_CAT 无 perm 检查 | ot `cat` 移除 ACL 保护检查后现可读任意路径 | **✅ 2026-10-28 内核 v2.5 已实现**：fs_perm_check 改位掩码 PERM_READ/WRITE/EXEC，SYS_LS/SYS_CAT 查 PERM_READ（实测 uid=0 正常） |
| 38 | **CE 无 uid 查询 API** | ce.h 33 API 无 whoami/uid 槽位；uid 仅 task 内部 | ot 无法显示/对齐内核 uid | **✅ 2026-10-28 已核验/落地**：getuid 槽 33（v2.3 已有），实测返回 0 |
| 39 | save 上限 FS_DATA_MAX=512B 未变 | include/fs.h:13 `#define FS_DATA_MAX 512`；sys_save_uid `if (size > FS_DATA_MAX) return -7` | ot swap/分片协议仍必要（非重复） | 保留 swap；2026-10-28 内核 v2.5 新增 file_write 大块 API（不经 FS_DATA_MAX）后，ot 可评估逐步切换新 API，swap 暂保留 |
| 40 | ls 带回调（结构化目录列表） | include/ce.h:50 ce_ls_cb(is_dir,name,size,ud)；task.c VS_LS_THUNK | ot 已直接用回调式 ls，无重复实现 | 无需动作 |
| 41 | 无磁盘 swap/分页文件 | paging.c 为内存分页（frame allocator）；CE API 无 swap | ot 的"分页文件"（swap）非重复实现 | 保留；用户点名的"分页文件"实测为内存分页 |

## 八、GNU 工具链规划——内核侧需求（2026-10-28 用户提出，只记录不实施）

| # | 能力/限制 | 现状（源码证据） | 对工具链的影响 | 处置 |
|---|---|---|---|---|
| 42 | **大文件读写**：FS_DATA_MAX=512B、无 seek/append/truncate/大块读 | include/fs.h:13 FS_DATA_MAX=512；sys_save_uid `if (size > FS_DATA_MAX) return -7`；SYS_CAT 上限 ~8KB | 编译器/链接器产出 .o/可执行文件均 >512B，无法落盘 | **✅ 2026-10-28 内核 v2.5 已实现**：新增 SYS_FILE_READ(39, offset 簇定位)/FILE_WRITE(40, mode0 截断/mode1 追加)/FILE_DELETE(41)；fs_read_at/fs_write_raw_mode/fat_append_file；QEMU 实测 100KB 写 + offset 32768 读 + delete 全过 |
| 43 | **CE 镜像上限 64KB** | CE_MAX_IMAGE 64K / CE_MAX_BSS 256K（include/ce.h） | 真实 cc1/ld 镜像轻易超 64KB | **✅ 2026-10-28 内核 v2.5 已实现**：CE_MAX_IMAGE→256KB、CE_MAX_BSS→512KB；移除 ce_bounce 大缓冲，spawn 直读首簇解析 header + FAT 链直写映射页 |
| 44 | **无管道/进程间通信 + 退出码** | CE API 33 槽无 pipe/spawn 等待；task_spawn_ce 仅内核 shell 用；CE 无 fork | make 依赖管道（\|）与子进程退出码判断成败 | **✅ 2026-10-28 内核 v2.5 已实现**：PIPE_CREATE(42)/WRITE(43)/READ(44)/CLOSE(45) 8×4KB 环形；SPAWN(46)/WAIT(47) 退出码；shell 重定向 `>`/`>>`/`\|`；实测 gen\|sink 11 行、child exit=0、重定向 0→100B |
| 45 | **可执行文件语义**（exec 位/读保护） | fs_perm_check 只强制写（SYS_SAVE）；无 exec 位 | 工具链需要"可执行文件"概念 | **✅ 2026-10-28 内核 v2.5 已实现**：位掩码含 PERM_EXEC + 读保护（见 #37）；CE spawn 即"可执行"语义 |
| 46 | **标准 syscall 入口面（libc 层）** | CE API 为 33 槽自定义（ce.h），非 POSIX | GCC 编译产物需统一 ABI（exit/read/write/open/malloc） | 用户侧 libc 封装层可先做；内核侧可选提供 POSIX 兼容入口；**本次未做（属下一步，仅记录）** |

## 九、b31 内核修复闭合（2026-10-29，随 node 交付轮）

> **编号命名空间说明（2026-11-09 落档补充）**
> 本节「项」列用的是 **b31 修复闭合表的局部编号**（`#48`/`F1`/`A2`/`#3`），
> 与第一节~第八节的**主清单编号**、第十节的 `#47` **互不引用**。
> 主清单在 2026-11-09 落档后已续编到 `#65`（第一节 `#48`~`#50`、第四节 `#51`~`#65`），
> 第四节同日另续编 `#68`（`u2k_str()` argv 校验缺陷，已修复），
> 引用本节条目时请写"第九节 #48"以免与主清单混淆。

| 项 | 问题 | 根因（源码证据） | 修复 | 状态 |
|---|---|---|---|---|
| #48 | 启动时"Installed built-in apps"覆盖宿主注入文件（test_ab 运行显示 sysinfo 输出） | fat_flush 从内存 fat_cache **整扇区重写** FAT，宿主注入表项与内核改动同扇区被抹；fat_alloc 复用被抹表项 → 簇冲突 | fat_set 脏表项 journal（fat_journal[4096]/fat_jsec）+ fat_flush **脏扇区 RMW**（先 ATA 读盘再补丁写回 FAT#1/#2）+ fat_alloc 分配前 ATA 核对磁盘真 0；read_fat_region/fat_mkfs 清 journal | 已修复（QEMU 实测注入文件连续两轮重启存活） |
| F1 | task_spawn_ce 失败路径泄漏子进程地址空间（反复失败 spawn 累积） | task_space_create 成功后的失败分支（原 -4/-6 direct return）不释放页表/页框 | 失败路径 goto fail_space4/fail_space6 → 先 `task_space_destroy(&t->space)` 再返回 | 已修复（spawn_fail.ce：40 次失败 spawn 后真实 spawn 仍成功） |
| A2 | fat_append_file 中途失败无回滚，内存 FAT 悬挂链残留 | fat_alloc/fat_write_cluster 失败时已写 FAT 链接不还原 | aundo[512] undo 日志逆序回滚全部 FAT 链接，失败后内存 FAT 与调用前一致 | 已修复（test_ab 100KB 追加+偏移读+管道+spawn+delete 全过） |
| #3 | fat_flush 每次整 FAT 区 8MB 全刷（性能） | 旧 fat_flush 写 FAT#1+FAT#2 全区 | 随 #48 改为脏扇区增量 RMW，不再全刷 | 已修复（随 #48 闭合） |

---

## 十、工具链常量与内核一致性核对（2026-11-06）

> 本节条目号 47 为清单序号，与第九节的内核问题编号 #48/#49 不是同一命名空间。
> 执行环境：隔离分支 `workbuddy/todo-20261106-1906`（主工作树 `D:/CatOs` 当时有他人并行改动，未触碰）。

| # | 问题 | 确认值（源码证据） | 影响 | 处置 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 47 | **构建工具镜像上限与内核漂移** | 内核 `include/ce.h:35` 为 `CE_MAX_IMAGE 0x40000`（256 KiB，v2.5 由 64 KiB 提升）；旧版 `kernel_v25/build/mkce.py` 与 `kernel_v25/extapp/build/mkce.py` 第 31 行均判 `len(data) > 0x10000` 即报错。旧版对 80 KiB 镜像实测输出 `error: image too big (81920 bytes, max 65536)` | 任何 >64 KiB 的镜像在宿主构建阶段被拒（自举产物 CC2.CE≈82KB 正落在此区间），表现为「编译器正常、镜像出不来」；与 `task_spawn_ce()` 的 256 KiB 判定不一致 | 两份 mkce.py 运行时解析 `include/ce.h` 的 `CE_MAX_IMAGE/CE_MAX_BSS` 作唯一真值源（找不到时回退内置 256 KiB），并把加载器三条判定（image / bss / api_offset+4）前移到构建期；两份保持字节一致，由 `tests/test_mkce.py` 断言 | v2.5 快照 / 2026-11-06 | 已修复 |

**同源问题：`tests/build_ce.sh` 是失效的旧副本。** 脚本按 `tests/` 拼 `ce.ld`、`ce_stub.c`、`$ROOT/build/mkce.py`，三者均不存在；实测 `sh tests/build_ce.sh spawn_fail.c` → exit 127，从未可用。已改为薄包装：解析源文件绝对路径后 `exec` 转发给 `kernel_v25/extapp/build_ce.sh`，只保留一套真实工具链。实测产出合法 CE01（`image=4408 bss=0 api_off=4404`，api+4 恰等于 image 边界）。

**核对结论**

1. **#17 / #32（readelf .bss 列解析）**：本快照 `build_ce.sh` 用 `objdump -h` 第 3 列（Size），mkce.py 不解析 readelf，无需同步修复；adofai 侧修复保留。
2. **第九节 b31 闭合表**：#48 / F1 / A2 / #49 / #3 五项均已列出，状态与 `problem.md` 的「已修复」记录一致，无需补项。
3. **验收**：`tests/test_mkce.py` 48/48 PASS；`tests/test_cc.py` 8/8 PASS（含指令级模拟器，另 `--no-sim` 8/8）；宿主构建链端到端产出合法 CE01。QEMU 实机仍待云侧环境。
4. **独立复核**：只读复核发现并已修复两处——(a) 测试只比上限数值、不比来源，因回退常量与 ce.h 恰好相等而构成假阳性；(b) 非法/负数/超 uint32 参数抛 Python traceback 而非 `error:`。现已断言 `load_limits()` 的 source 必须指向 `include/ce.h`，并覆盖 0x 前缀、非法 hex、负数、超范围、uint32 回绕等边界。

**本轮发现、合流时一并修复的既有问题（2026-11-10 复核：两条均已闭合）**

| 位置 | 问题 | 实际修复 | 状态 |
|---|---|---|---|
| `src/task.c:1779-1793`（`task_spawn_ce`） | `api` / `img` 为 `uint32_t`，`api + 4 > img` 会回绕：`api=0xFFFFFFFC` 时和为 0，校验被绕过，随后在 `USER_IMAGE_BASE + 0xFFFFFFFC` 写 4 字节 API 表指针 → 越界写 | 已改为防溢出形式 `if (img < 4 \|\| api > img - 4) return -5;`（提交 `e1e9a88`），并在源码注释里记录了回绕成因 | **已修复** |
| `kernel_v25/extapp/build_ce.sh:48-70` | `CFLAGS` 内 `-I$CEBASE -I$ROOT/include` 未加引号，而 `$CFLAGS` 又以无引号形式展开；工作目录含空格时 gcc 收到被分词的错误参数 | `-I` 已移出 `CFLAGS`，在 gcc 命令行写成独立的 `-I"$CEBASE" -I"$ROOT/include"`（`:70`、`:72`），`:51-56` 注释解释了「变量内的引号不会被重新解析」；同轮补上 `-DCC_HOST`（`:69`）（提交 `176c559`） | **已修复** |

> 说明：这两条在隔离分支上刻意未改（当时主工作树有并行会话）。2026-11-10 合流到
> `D:/CatOs` 的 master 后逐行复核，确认均已落地；`merge-harvest-QA-final.md` 的
> 「遗留」表与 S-8 判定同样已同步更正。

---

## 十一、合流后续修 + 声卡驱动（2026-11-10，v2.11）

> 背景：`workbuddy/todo-20261106-1906`（`6945860`，27 个提交）fast-forward 合入
> `D:/CatOs` 的 master（`a7eae5a`），工作目录从隔离 worktree 迁回 `D:/CatOs`。
> 合流后逐条复核代码，发现「遗留缺陷」多数其实已修，真正剩下的只有文档状态滞后
> 与 `task_pages_of()` 一处；同轮新增 SB16 声卡驱动。

| # | 问题 | 确认值 / 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **74** | **`task_pages_of()` 把 VMA 跨度当成已映射页数 ⇒ `top` 的 PAGES 列虚高** | `task.c` 原实现 `n += (v->end - v->start) >> 12`。`heap_vma_init()` 建的是 512 KiB 堆 VMA，实际由 `heap_ensure_range()` 按需逐页映射；任何调过 `api->alloc()` 的 ring3 任务恒被多算 **128 页**，128 KiB 栈 VMA 同理。唯一调用点是 `task_stat()` → `shell.c` 的 `cmd_top()`，`sysinfo.ce` 不打印该值，**不影响实际内存分配** | `top` 的驻留页数不可信，容易被误读成内存泄漏 | 改为逐页 `task_virt_to_phys(&t->space, a)` 判定 PTE present。该函数读的是任务自己的 `pt`/`pt2`（内核恒等映射），不依赖当前 CR3，规避了旧版 raw-PT 扫描"页表被切走就错"的缺陷 | **已修复** |
| **75** | **问题清单 / QA 报告的状态列大面积滞后于代码** | #48/#49/#50/#51/#61~#65 共 9 条仍写「已确认/待修复」，`go.c` 现状段仍写「现项目没有 go.c」，`merge-harvest-QA-final.md` 的遗留表写「本期未改」、S-8 写 PARTIAL 且自相矛盾；逐行复核 **全部已在合流分支修复或验证** | 后来者会重复劳动，或按错误状态回退已修代码（比代码缺陷更危险） | 状态列统一改为「已修复（合流 6945860 / 2026-11-10 复核）」；Go 段改写为真实状态；QA 报告遗留表与 S-8 同步更正 | **已修复** |
| **76** | **QEMU `-audiodev wav` 的 RIFF/data size 字段恒为 0 ⇒ 音频回归误判「无数据」** | `audio/wavaudio.c` 的 `wav_init_out()` 先写 44 字节**全零长度**头，`wav_fini_out()` 在 voice 关闭时才回填；HMP `quit` 不保证走到 fini。实测文件 192,100 B、尾部 `00 de 00 de` 是真实波形，但 `data` chunk size=0，按标准解析会得到「no data chunk」 | 音频明明录到了却报 FAIL，症状酷似驱动没工作 | `tests/snd_regress.py` 的 `wav_check()` 定位 `data` 标记后取到 EOF 的全部字节，并**自己回填** RIFF size(@4)/data size(@40)，再用标准库 `wave` 复验头合法；不依赖 QEMU 是否 finalize | **已规避/已验证** |
| **77** | **不能在 int 0x80 syscall 里等音频播放完成** | `intr.c:202` 把 int 0x80 装成 **0xEE 中断门**，CPU 进入时清 IF；`cpu.s:_isr_0x80` 没有 `sti`。在 `isr_syscall()` 里 `hlt` 会永久挂起，`timer_ticks` 也因 PIT IRQ 被屏蔽而不再递增 → 死等 | 若 `snd_play()` 同步等待 DMA 完成，整个内核会死锁 | 播放设计为**异步**：`snd_play()`/`snd_tone()` 只武装 DMA 立即返回，`snd_irq()` 清 busy 标志；等待放在 shell（ring0，IF=1）或 ring3 的 `delay()` 轮询中 | **已规避/已验证** |

### 11.1 声卡驱动（Sound Blaster 16，ISA）

**选型依据**：内核有裸 PCI 配置空间读写（`hw.c` / `net.c` / `be_vbe.c`）但**没有通用 PCI 驱动框架**，
`intr.c` 的 IRQ 分发是一个硬编码 switch，`intr_enable()` 只解屏蔽 IRQ0/1/2。SB16 是 ISA 设备，
iobase `0x220` / IRQ5 / 8 位 DMA ch1 均固定，只需「一个 `case 5` + 一个 IMR 位」即可打通；
AC97 / ES1370 / intel-hda 都要求 PCI 枚举 + bus master + BDL + PCI INTA 路由同时成立，风险高得多。

| 项 | 值 |
|---|---|
| 驱动文件 | `kernel_v25/include/snd.h`、`kernel_v25/src/snd.c` |
| DSP 流程 | reset → `0xAA` → `0xE1` 读版本 → `0x41` 设输出率（高字节先）→ `0x14` 8 位单次 DMA（`len-1`，低字节先） |
| IRQ 应答 | 读 `0x22E` 清 mixer `0x82` bit0；PIC EOI 仍由 `intr.c:177-178` 统一发 |
| DMA 缓冲 | 物理 `0x520000`，32 KiB，64 KiB 对齐、<16 MiB、不跨 64 KiB 边界；`paging.c` 的 `DMA_HI` 扩到 `0x528000` 记录保留区 |
| 8237 ch1 | mode `0x49`（single / read / increment），addr `0x02`，count `0x03`，page `0x83`，mask `0x0A` |
| ABI | append-only 槽 **57~60**：`snd_info` / `snd_play` / `snd_stop` / `snd_tone`；`SYS_COUNT` 57→61，thunk 区仍 < 0x800 |
| 用户指针 | `SYS_SND_PLAY` 先 `copy_from_user()` → `validate_uaddr()`，再写入内核 DMA 缓冲；ring3 无法让 DMA 指向任意物理内存 |
| shell | `sndinfo`、`sndtest [HZ MS]`（直接调内核，ring0） |
| 无设备时 | `snd_init()` 有界轮询，失败即打印一行并禁用；既有 12/12、4/4、7/7 回归不受影响（已实测） |

**QEMU 参数**（`-soundhw` 在 QEMU 7.1 已移除，11.1.0 只能用 `-device`）：

```
-audiodev wav,id=snd0,path=D:/CatOs/tests/qemu_shots/snd.wav
-device sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0
```

**实机验收**（`tests/snd_regress.py`，独立 monitor 端口 4460，按 PID 清理）：

```
PASS format / bin-has-sndtest / sndinfo / sndtest-tone / ring3-sndtest / wav-non-silent
sndtest      -> snd: irqs=1 busy=0   SNDTEST PASS
ring3 CE     -> snd: ce irqs=1 busy=0   SND CE PASS  [task 1 'sndtest' exit(0)]
WAV          -> 44100Hz/2ch/16bit, 48355 frames, distinct=36, min=0 max=255 (非静音)
```

**已知限制**：仅 8 位无符号单声道单次 DMA（非 auto-init、无双缓冲），单次 ≤32 KiB；
`snd_tone()` 用 32 点整数正弦表（内核 `-mno-80387`）；没有录音（ADC）、混音器音量策略或
16 位/立体声路径。这些都属于后续扩展，不影响「有可用 PCM 输出」这一交付目标。

---

## 十二、用户指针跨页校验（2026-11-10，P0，来源 `.workbuddy/reports/bug11-10.md`）

> 报告由本机另一路检查在 13:40–13:55 生成，第二节报了一个「1 字节校验换 n 字节写入 →
> 整机停机」的 P0 缺陷，并用临时探针 `vprobe.c` 实机复现。该检查把 13:51 修改
> `task.c`（加 Issue #74 注释）的进程判为「他人并发会话」，实际就是本会话上一轮的
> 合流后续修；`D:/CatOs` 随之被标成「当前目录有人在用」。本会话 14:23 起复核时
> master 已停在 `cc631c8`、工作树只有历史遗留的未跟踪杂项文件，遂直接在本目录修复。
>
> 报告的临时探针/脚本当时已删净，缺陷因此**没有任何回归防护**。本轮把探针做成永久资产：
> `kernel_v25/extapp/vprobe.c` → `/bin/vprobe.ce`（进 `build.sh` + `gen_builtin.py`），
> 配 `tests/vprobe_regress.py`（独立 monitor 端口 4461，按 PID 清理）。

| # | 问题 | 确认值 / 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **78** | **多个 syscall 只 `validate_uaddr(t, buf, 1)` 却按 n 字节写/读用户缓冲 ⇒ 任意 ring3 程序可打停内核** | `validate_uaddr()`（`vma.c:140`）按 `len` 逐页 `vma_map_page_on_fault()`；`len=1` 只补映射首页。报告列的 4 处写方向：`SYS_CAT`(≤4096)、`SYS_HTTP_GET`(≤8192)、`SYS_NET_UDP_RECV`(≤2048)、`SYS_FILE_READ`(≤16384)。本轮复核**另发现** `SYS_HW_STR`(≤4096 写)、`SYS_NET_UDP_SEND`(≤1400 读)，以及 `SYS_ATOI`/`SYS_STRLEN`/`SYS_STRCMP`/`SYS_STRCPY`（未知长度字符串跨页读/写）同类 | 第 2 页仍是 not-present PTE，ring0 直接解引用 → `int=0x0e err=0x2 cs=0x08`；`isr_exception()` 对 `cs&3==0` 不恢复，`exc_dump` 后 `hlt` **整机停机**。最小触发：`p=alloc(16); file_read(path,p,16384,0)`（复现 `cr2=0x02101000`）。现有 CE 的 `g_filebuf`/`g_src` 都在随镜像整段映射的 .bss 内，所以历史回归全绿也查不出来 | 统一模型：① 写方向按**真实上界 `max`** `validate_uaddr()` 补页，再用 `copy_to_user()` 经任务页表写入；② 读方向用 `copy_from_user()`；③ 字符串类改用 `u2k_str()` 逐字节取内核快照；④ `SYS_HW_STR` 先在内核缓冲里生成再拷；⑤ `SYS_TIME_STR`/`SYS_NET_DNS`/`SYS_NET_IP`/`SYS_READLINE`/`SYS_PIPE_READ`/`SYS_SAVE`/`SYS_SPAWN` 的裸写裸读一并按页表收敛 | **已修复** |
| **79** | **VMA guard 误伤堆/栈 VMA，导致按长度校验仍无法补页** | `vma_map_page_on_fault()` 无条件把「VMA 起始 + 8 KiB」之前判为 guard 页。但 `heap_vma_init()` 建的堆 VMA 起点就是 `USER_HEAP_BASE`（首块 payload 就在第一页），栈 VMA 亦同；只有 `vma_mmap_anon()` 返回的指针才位于 guard 之后。修好 #78 后 5 个探针全报 `verdict FAIL`，根因即第二页 `0x02101000 < 0x02102000` 被拒，`validate_uaddr()` 返回 -14 | 任何按长度校验的 syscall 都无法写入堆缓冲第二页；也会让栈 VMA 的按需映射失效 | `vma_t` 的闲置 `reserved` 字段改为 `guard_pages`（`paging.h`）；`vma_new()` 默认 0，只有 `vma_mmap_anon()` 显式设为 `VMA_GUARD_PAGES`；`vma_map_page_on_fault()` 按该字段计算 `guard_start`。语义与 `mmap()` 的 guard 设计保持一致 | **已修复** |
| **80** | **`vma_map_page_on_fault()` 把非页对齐 CR2 向上对齐到「下一页」** | 原代码 `if (vaddr & 0xFFF) vaddr = (vaddr & ~0xFFF) + 4096;`。ring3 访问 `0x...1008` 缺页时映射的是 `0x...2000`，iret 重试后仍在 `0x...1000` 缺页 → 反复缺页/任务被杀。`validate_uaddr()` 传页对齐地址，所以不触发；只有真实 CR2 路径受影响 | ring3 首次触碰懒映射页内非首字节时可能反复缺页 | 改为向下对齐 `vaddr &= ~0xFFFu`；guard 判定仍用对齐前的原始地址 | **已修复** |
| **81** | **`SYS_SPAWN` argv 数组上界回绕 + 裸读未校验页** | 原 `if (argv_u + (i+1)*4 > USER_ARENA_END)` 只判上界，`argv_u` 接近 2^32 时求和回绕绕过；随后 `*(uint32_t *)(argv_u + i*4)` 是 ring0 裸读，而 `uaddr_in_vma()` 只查 VMA **不会补页** | 可绕过上界检查；argv 落在懒映射页时 ring0 读缺页停机 | 改为防溢出形式 `argv_u > END-4 || off > END-4-argv_u`，并用 `copy_from_user()` 读 4 字节指针 | **已修复** |
| **82** | **`validate_uaddr()` 的 `addr + len - 1` 回绕** | `addr` 接近 2^32 且 `len` 较大时 `end` 回绕到低地址，若恰落在某个 VMA 内则 `vma_contains(end)` 通过，而逐页循环 `while (p <= end)` 被跳过 → 未补页却返回 0 | 后续页表拷贝拿到 not-present 页，轻则 EFAULT 重则（配合 #78 的旧裸写）停机 | 增加 `if (len - 1 > 0xFFFFFFFFu - addr) return -14;` | **已修复** |

### 12.1 修复前后对照（同一探针、同一 ISO 流程）

```
修复前  python tests/vprobe_regress.py --only fr
        FAIL fr    kernel fault: ... 0x0000000e 0x00000002 ... ring0 kernel fault - system halted

修复后  python tests/vprobe_regress.py
        PASS format / bin-has-vprobe / fr / cat / hw / str / strcpy
        steps=7 ok=7 fail=0
```

`vprobe.ce` 用 `alloc(16)` 拿到「首页已映射、第二页 not-present」的堆缓冲，5 个 case 分别让
`file_read` / `cat` / `hw_str` / `strlen+strcmp+atoi` / `strcpy` 跨越 4 KiB 边界；每个 case 独立
`run`，脚本识别内核 fault banner 并停机归因。修复前首例即停机，修复后 5 例均返回
`VPROBE <case> PASS`。

### 12.2 本轮遗留观察（不阻断，P2）

- `SYS_WRITE`（每 4 KiB 边界重新校验后逐字节裸读）、`SYS_PIPE_WRITE`（按 `len` 校验后逐字节裸读）、
  `SYS_FILE_WRITE`（按 `size` 校验后把用户指针交给 `fs_write_raw_mode()`）仍保留裸指针访问。
  它们的 `validate_uaddr()` 长度已覆盖实际访问范围，**不会再触发缺页停机**，但依赖「syscall
  期间 CR3 恒为当前任务页表」这一前提；后续允许长 syscall 中途让出 CPU 时应统一改成
  `copy_from_user()`。
- 报告第三节提到的 `static char fbuf[16384] / b[4096] / b[8192]` 全任务共享内核缓冲依旧存在，
  目前靠 `task_sleep_ticks()` 里 `if (g_in_syscall) return;` 保证 syscall 不被抢占；若将来允许
  长 syscall 让出 CPU，需改成按 tid 分槽（内核栈仅 8 KiB，不能简单挪到栈上）。


## 十三、v2.12 24bpp 真彩与用户绘图 API（2026-11-10）

本轮交付：`be_vbe24.c` 后端（800x600x24，3 字节 BGR，pitch 2400）、`include/gfx24.h`
内核绘图原语、CE ABI v2.12 槽 61..63、shell `gfx24` 命令、`/bin/gfx24.ce` 探针、
`tests/gfx24_regress.py` 像素级实机回归。**以下 #83 / #84 是修复 24bpp 过程中实测发现的两个真 bug，
#85 是本轮需求闭环。**

| # | 问题 | 确认值 / 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **83** | **VBE 32bpp 后端把 PCI BAR2 当成线性帧缓冲** | `be_vbe.c:find_lfb()` 读 `0x18`(BAR2)。QEMU 11.1 `info pci` 实测 1234:1111 的 **BAR0 = `0xfd000000`[0xfdffffff]（16 MiB 可预取帧缓冲）**，**BAR2 = `0xfebf0000`[0xfebf0fff]（仅 4 KiB MMIO 窗口）**。后端把 1.9 MB 像素写到 MMIO 区之后的地址，`screendump` 恒为 640x480 全黑；文件头还自述 "compiled and linked, NOT verified on QEMU" | 32bpp 真彩后端自合流起从未点亮；`vbe` 命令切过去只有黑屏，历史上被误当成"没实机所以没法验" | `find_lfb()` 改读 `0x10`(BAR0)，`pci_bar_size()` 同步；注释写明两个 BAR 的实测值。修复后 `screendump` 变为 800x600 且非黑（72,484 个非黑字节） | **已修复** |
| **84** | **ring3 syscall 在用户 CR3 下写 VBE LFB ⇒ #PF 连锁 triple fault** | `vbe_lfb_map()` 只把 LFB 的 PDE（0x3F4）装进 `kdir`，而 `task_space_create()` 只复制 PDE 0..15；syscall 执行在**调用者的 CR3** 上，内核代写 VRAM 直接缺页。QEMU `-d cpu_reset` 日志明确 `Triple fault`。触发点：`run /bin/gfx24.ce` 回车后 monitor 立刻 `WinError 10054`；`gfx24` 命令在 ring0（CR3=kDIR）下静置 12 秒完全正常 | 任何从 ring3 触发的显存写入都会打停整机：新增的 `gfx_rect`/`gfx_blit`，乃至图形模式下 `write()` → `con_write()` → 后端 `draw_cell()`。是 v2.7 起就潜伏的通用缺陷 | `paging.c` 记录 LFB PDE 范围；`vbe_lfb_map()` 同步**当前** CR3 对应的目录并只 flush 活动目录（不再无条件切 `kdir`）；`task_space_create()` 为新目录镜像 PDE；`isr_syscall()` 开头对 ring3 调用兜底同步。PTE 始终 `0x003`（无 US 位），ring3 依旧无法访问 VRAM，`task_space_destroy()` 也不释放共享页表 | **已修复** |
| **85** | **缺少 24 位真彩模式与用户态绘图能力（需求）** | 原有后端只有 8bpp 索引 / 32bpp 真彩文本；`display` 明写 `user drawing API : NONE`；CE ABI 无绘图槽 | 用户程序无法输出任何像素、色块或位图 | 新增 `CON_MODE_VBE24=3` + `be_vbe24.c` + `gfx24.h`；ABI append-only 扩到 64 槽；三槽用**描述符结构体**绕开 thunk 只转发 4 参数的限制；用户位图经 `copy_from_user()` 逐行校验（沿用 #78 的模型） | **已实现** |

### 13.1 验证结果（同一 ISO，`tests/gfx24_regress.py`）

```
PASS format + builtin install / /bin/gfx24.ce installed
PASS DISPI switched to 800x600
PASS red/green/blue/white blocks、24-bit backdrop 0x102040、
     256-step gradient midpoint (127,0,128)、black/white bars
PASS gfx_rect from ring3 (cyan)      (530,150) == (0,255,255)
PASS gfx_blit RGB24 row 32 col 30    (630,332) == (120,128,128)
PASS demo pixels still intact after the CE run
0 step(s) failed
```

像素从 QEMU `screendump` 的 PPM 里逐点读取，因此 BGR 顺序、24bpp pitch=2400、
渐变色阶、ring3 描述符与跨页位图拷贝全部被真实硬件状态验证，而不是只看命令回显。

### 13.2 回归总账（v2.12 ISO）

| 套件 | 结果 |
|---|---|
| `tests/gfx24_regress.py` | 0 failed（16 项像素 / 状态断言） |
| `tests/ce_regress.py` | PASS=11 SKIP=13 FAIL=0（新增 `gfx24.ce` 模拟器 SKIP 通过） |
| `tests/test_mkce.py` / `tests/test_cc.py` | 48 / 10 passed, 0 failed |
| `tests/qemu_regress.py` | steps=12 ok=12 warn=0 |
| `tests/vprobe_regress.py` | steps=7 ok=7 fail=0（#78 未被 paging 改动破坏） |
| `tests/snd_regress.py` / `heap_regress.py` / `go_regress.py` | 6/6、4/4、7/7 全 PASS |
| `tests/qa_probe_final.py` | exit=0 |

### 13.3 已知取舍（P2，不阻断）

- `be_vbe24.c` 故意**不启用硬件 pan 滚动**（`VIRT_HEIGHT=600`，`scroll=NULL`）与 MTRR-WC：
  虚拟帧缓冲会让用户矩形滚出视野后又突然出现，语义难解释；WC 与用户驱动写入混用需要更多验证。
  代价是滚动走前端全量重绘、显存为 UC，1.44 MB 规模实测无感。
- 32bpp 后端 `be_vbe.c` 的 BAR 已修（#83），但其硬件 pan + MTRR-WC 路径仍只做过点亮验证，
  未做逐像素回归；24bpp 是默认推荐的真彩模式。
- `gfx_blit` 逐行 `copy_from_user`，内核行缓冲 2400 B；`w*h*3` 无总量上限但受 800x600 裁剪，
  一次全屏 blit = 600 次 syscall 内拷贝，属同步操作，不要在 int 0x80 里期望异步完成。


## 十四、消除劣根性第一批：syscall 边界 / 诊断 / fsck / 应用单一来源（2026-11-11）

本轮目标：上一轮架构评估中「不动设计假设就能增量解决」的四项。**以下 #86 是 12.2 遗留观察的闭环，
#87 / #88 是可靠性补强，#89 是构建链路修复。**

| # | 问题 | 确认值 / 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **86** | **SYS_WRITE / SYS_FILE_WRITE / SYS_PIPE_WRITE 仍是「校验后裸解引用」** | 12.2 遗留：`task.c` 三处——`s[total+i]`（SYS_WRITE）、`(const char*)a2` 直传 `fs_write_raw_mode`（SYS_FILE_WRITE，且 fs 层跑在调用者 CR3）、`ud[wrote+k]`（SYS_PIPE_WRITE，**管道满时 `task_sleep_ticks()` 会切任务，裸指针在别人的 CR3 下被解引用**）。此外 `validate_uaddr(t, a, 1)` 逐边界补页的模式靠人手维护，漏一处就是 #78 复发 | 与 #78 同级：ring3 可让内核 #PF 停机；PIPE_WRITE 还叠加 CR3 错位风险 | 三处全部改为 `copy_from_user()` 内核快照：SYS_WRITE 按「不超过当前页尾」分段快照（字符串可能恰好终结在页边界，多读一页会强求调用者没映射的页）；FILE_WRITE 4 KiB 分块、首块带原 mode、后续 append；PIPE_WRITE 一次性快照 `PIPE_BUF` 到 `pwb[]`。`vprobe.c` 新增 `wr`/`fw`/`pw` 三个跨页 case（wr 还校验屏幕上真的输出了跨页字符串），回归 10/10 | **已修复** |
| **87** | **故障后零诊断信息（劣根性：崩溃即 halt / 单行 kill）** | ring0 fault 只有寄存器 dump + halt；ring3 任务死掉只留一行，事后 shell 完全查不到发生了什么 | 崩溃不可归因；教学内核最常见的"刚才谁崩了"无法回答 | 新增 `diag.c/diag.h`：异常分发器对**致命故障统一记录** `g_fault_last`（向量/ring/tid/任务名/eip/cr2/err/eflags/tick）；shell 新增 `faultinfo`（含 PF 错误码位解码，如实测 `crash.ce` → `err=0x7 present write user`，U/S 位是"访问来自 user mode"而非"页面归属"）；导出 `panic(msg)` 供子系统统一停机入口。`tests/diag_regress.py`（端口 4463）4/4 | **已实现** |
| **88** | **FAT 双 FAT 镜像无任何一致性检查** | `fat_flush()` 同步写两份 FAT，但没有任何启动时校验；掉电/崩溃若撕裂镜像写，坏的那份会静默参与后续操作 | 静默数据损坏（劣根性：无 fsck） | `fat_verify_mirror()`：mount 时只对「FAT#1 缓存中含非零项的扇区」逐扇区读回 FAT#2 比较，不一致则以 FAT#1（分配器唯一对磁盘复核过的那份）修复 FAT#2 并打印；shell `fsck` 命令可手动重跑。`diag_regress` 的 `fsck` 步骤 + 开机自动检查 | **已实现** |
| **89** | **应用清单在 build.sh 与 gen_builtin.py 双处硬编码** | `build.sh:47` 与 `gen_builtin.py` 的 FILES/OPTIONAL 必须同步改；历史教训（"改了不生效"）正源于此；且 `ce_install_builtins()` 每次启动都打印 "Installed ..."（即使什么都没装） | 新增应用要改两处，漏一处即静默丢应用 | 新增 `build/apps.list` 作为**单一来源**：build.sh 读它编译，gen_builtin.py 读它内嵌（特殊产物 hello.elf / node.ce / docs / *.GO 走显式 EXTRA 表）；新增 `tests/test_app_list.py` 守卫（20 个应用、源文件存在、生成表覆盖、build.sh 无硬编码残留）；安装器只在真正安装了文件时打印 | **已实现** |

### 14.1 验证结果（同一 ISO）

```
宿主: test_mkce 48/0  test_cc 10/0  test_app_list PASS(20 apps)
      ce_regress PASS=11 SKIP=13 FAIL=0
实机: vprobe 10/10（新增 wr/fw/pw 跨页 case）
      qemu_regress 12/12（help size 14565→15210 = 新增两行命令说明）
      diag_regress 4/4（crash-killed / faultinfo / fsck，新套件）
      heap 4/4  go 7/7  snd 6/6  gfx24 0 failed
      qa_probe_final exit=0
```

### 14.2 本轮明确不做（与架构评估一致）

- 取消恒等映射 / SMP / 内核抢占：属重写级（12+ 文件 / 147 处全局状态），依赖顺序见 2026-11-10 评估。
- 内存动态化（COW / 换出 / 解除 512 KiB 硬上限）：重构级，`frame_alloc` 固定 64 MiB、每 arena 单页表，
  需要单独一轮；本轮只消掉了它最危险的前置（#86 裸指针）。
- 应用与内核**完全**解耦：✅ 已完成（见 15 节 #90）。不需要 GRUB module 方案——
  宿主侧 `build/mkdisk.py` 直接生成预装 /bin 的 FAT32 磁盘镜像，布局与内核
  `fat_mkfs()` 逐字节同构，文件清单与 `gen_builtin.py` 同源；内核内嵌副本降级为 fallback。
- 12.2 的「全任务共享 static 缓冲」观察依然成立：本轮新增的 `wbuf[4096]` / `pwb[4096]` 同样
  靠 `g_in_syscall` 非抢占保证安全，将来允许 syscall 让出 CPU 时须一并改分槽。

## 十五、应用与内核解耦（#90，2026-11-11）

### #90 应用不再绑死内核镜像 —— 已解决

**背景**：`gen_builtin.py` 把全部 CE 应用编进 `builtin_apps.inc`（~1.5 万行）塞入
kernel.bin，改一个应用要重编内核、重做 ISO——"通用 OS"不合格的一条劣根性。
此前评估认为需要 GRUB module + multiboot info 解析，但本机无 GRUB 工具链。

**实际方案**（比 GRUB module 简单得多）：磁盘才是应用的真实来源。
- `kernel_v25/build/mkdisk.py`：宿主侧复刻 `fat.c:fat_mkfs()` 的**逐字节同构**布局——
  MBR（0x0C 分区 @LBA1）+ DBR + 备份 DBR@7 + FSInfo@2 + 双 FAT + 根目录 + 15 个标准目录；
  文件清单 import `gen_builtin.py` 的 FILES/OPTIONAL（单一来源，与内核内嵌完全一致）。
  `fat_probe()` 只认 DBR 字段，所以内核**无需任何改动**即可挂载。
- 内核侧 `ce_install_builtins()`（#89 已改）：文件已存在即跳过 → 预装盘启动时
  内嵌副本一个都不装，kernel.bin 只剩 bootstrap fallback 语义。
- `build.sh disk` 目标：编译应用 → `mkdisk.py -o catos_disk.img -m 64`；
  启动 `qemu-system-i386 -cdrom catos.iso -m 256 -hda catos_disk.img`。
- 与 `fat_mkfs()` 的唯一有意偏差：簇大小下限 8 扇区（4 KiB）而非小卷的 1 扇区——
  1 扇区簇的根目录簇只有 16 项，装不下 2 点项 + 15 目录 + 文件；`fat_probe()` 从 DBR
  读 spc，不关心 mkfs 惯例。`FAT_CLUSTER_SZ_MAX=32 KiB` 上限内合法。

**坑（都在 mkdisk.py 里修掉）**：
- bytearray 切片赋值会**自动扩容**：dots 底板误写 22 字节 → `d[0:11] = 22字节`
  把 32 字节 dirent 撑成 43 字节。dent() 加 11 字节断言。
- `pe = mbr[446:]` 是切片**拷贝**，分区表写进了副本，MBR 全零 → fat_probe 找不到分区。
- FAT 表只在 write_metadata 落盘一次，alloc() 之后未回写 → 目录项指向的簇在 FAT
  里全 0，`ls` 能看到文件但 `run` err -4。finalize 时回写双 FAT。
- `verify()` 改为从**镜像字节**读 FAT 走链，内存镜像自洽但镜像未回写这类错才抓得住。

**验证**：
```
build/mkdisk.py 宿主自检：MBR 分区项 / DBR / FAT 镜像 / 每文件链 EOC 终止
disk_regress 5/5（端口 4464）：免 format 挂载 / ls /bin 30 项 / hello.ce
  与 vprobe（跨页用户缓冲）从磁盘实跑 / fsck 镜像一致
qemu_regress 12/12（内嵌 fallback 流程不受影响）  test_app_list PASS
build.sh disk 与 iso 两条路径均 EXIT=0
```


## 十六、用户机制与细粒度能力（#91，2026-11-11）

**需求**：多用户账户、权限深度细粒度自定义、初始用户 root。

**方案**（内核侧强制，shell 呈现；**零新增 CE syscall 槽**，64 槽 ABI 不动）：

- 新模块 `src/user.c` + `include/user.h`：持久化 `/etc/passwd`（name:uid:gid:home:shell:caps:passhash）
  与 `/etc/acl`（path:uid:gid:mode，12 位 Unix owner/group/other）。首次启动种子：
  `root`（uid 0，CAP_ALL，初始用户）+ `user`（uid 100，CAP_NET|KILL|AUDIO|DEVICE，
  等价旧 uid-100 行为）。djb2 口令哈希（教学内核无密码学，仅防窥视）。
- `task_t` 增加 `gid`/`caps`；`task_spawn_ce()` 按 uid 查用户库注入；shell/idle 为 root。
- `fs_perm_check(path, uid, gid, need)` 重写为标准 Unix 语义：owner→6-8 位、
  组→3-5 位、other→0-2 位；root 直通；无 ACL 项时读/执行放行、写走 /home//tmp 白名单
  （与旧 `perm_default_write` 行为一致）。旧编码（owner 位在 <<6、无组）废弃。
  创建文件/目录写入创建者 uid/gid + 0644/0755，`chmod`/`chown` 改 `/etc/acl` 并落盘。
- **能力位**：CAP_NET(0x1)/KILL(0x2)/FSADMIN(0x4)/AUDIO(0x8)/USERADM(0x10)/
  DEVICE(0x20)/SYS(0x40)。内核 syscall 分发处强制：网络 25-32 槽、音频 57-60 槽
  缺位返回 -13；`task_kill` 扩为 root|属主|CAP_KILL。shell 的 format/fsck 要
  CAP_FSADMIN、sudo/useradd/userdel/passwd/chcaps 要 CAP_USERADM。
- shell 新命令（ring0 直读用户库）：whoami/id/lsuser(users)/caps/login/su/useradd/
  userdel/passwd/chcaps/chmod/chown；`login`/`su` 切换会话用户（root 可免密 su），
  裸命令与 `run` 以会话用户执行，`sudo` 固定 root。提示符显示 `user@catos:/$`。
- 启动 `shell_run()` 用 `user_db_init()`+`acl_init()` 替代 `fs_perm_seed()`（已删）。

**行为注意**：无网卡的 root `net_init=-1`（真没硬件）、caps=0 的 guest `net_init=-13`
（能力拒绝）——新增 `/bin/captest.ce` 探针区分两者。`perms.ce` 合同不变
（guest 跑仍是 /bin、/etc EACCES + /home OK）。

**验证**：
```
user_regress 15/15（端口 4465）：whoami=root / lsuser / useradd+持久化 / id /
  login guest / sudo+format 拒绝 / perms as guest / captest root(-1) vs guest(-13) /
  su / caps 图例 / 全程无 fault
qemu 12/12、diag 4/4、disk 5/5、vprobe 10/10、snd 6/6、gfx24 0 failed、
heap 4/4、go 7/7、qa_probe_final exit=0
宿主 test_mkce 48、test_cc 10、test_app_list 21 apps、ce_regress 11P/13S/0F
```

## 十七、ot / adofai 老项目收编与 v2.14（2026-11-11）

来源：`catos-ot-game-2026-11-11.zip`（下载目录）——ot（Ocean's Tools）与 adofai 音游两条
2026-10 月的用户态线，当时内核功能不全，项目在用户态做了大量 workaround。本轮原则：
**内核已有的从 ot 去掉；内核没有但有价值的在内核态实现。**

| # | 问题 | 确认值 / 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **92** | **CE 无非阻塞按键 API，应用被迫在 ring0 玩 PIC 手术** | adofai 实测结论：getch（槽 22）阻塞无超时 → 游戏屏蔽 IRQ1（PIC IMR bit1）后直读 0x64/0x60 轮询。副作用：IRQ1 屏蔽期间系统其他键盘消费者全部没输入 | 用户态应用篡改中断控制器；键盘饿死 | CE ABI **v2.14**：追加 slot 64 `int (*pollkey)(void)`（`kb_getkey_timed(0)`：解码待处理扫描码，无键 -1，永不阻塞/让出）。adofai 已改用 pollkey，IMR/端口代码全删 | **已修复** |
| **93** | **shell 缺 cat / rm / cp / mv**（ot 在用户态各写了一份） | ot 的 cat/cd/mkdir/touch/ls/echo/... 全是内核 shell 未提供的岁月产物；ot 的 cp/mv/rm 根本没进命令表（交接文档与实际不符），但诉求真实存在 | 基本文件操作要靠 134KB 用户态工具箱 | shell 新增 `cat`（≤64 KiB 文本打印）、`rm`（=fs_delete+PERM_WRITE）、`cp`（≤16 KiB 分块，创建者 uid/gid + 0644）、`mv`（cp+rm）。ot 命令表中 30 个与 shell 重复的命令全部移除 | **已修复** |
| **94** | **ot 是游离在仓库外的 134KB 单文件 + 旧 ABI 自声明** | ocean_tools.c 自带 33 槽 v2.2 API 结构体、`static __ce_api`（nm 找不到符号）、ce_main 在 `.text.start` 段；打包内核还是 2026-10-27 版 | 无法随系统分发、无法回归 | 收编为 `kernel_v25/extapp/ocntools.c`（+fmgr_port/hexedit_port/CFG/DEMO）：`.text.ce_entry`、全局 `__ce_api`、NULL api 判断 bug 顺手修掉；进 `build/apps.list`（23 应用）→ `/bin/ocntools.ce`（89 KB）；保留独有价值：脚本引擎/fmgr/hexedit/sft/vcs/cedbg/alias/banner/cecheck；help/about 重写并注明"基础命令在内核 shell" | **已修复** |
| **95** | **adofai 游戏无音效、依赖被打包内核的 ring0 特权** | 交接文档 §2.5 已知限制："无音效（无声卡）"——当时内核确实没有；IRQ1 hack 同 #92 | 游戏体验 + 特权依赖 | 收编为 `extapp/adofai.c` → `/bin/adofai.ce`（9 KB）：改用 ce_abi.h 真 ABI，节拍音效走 v2.11 `snd_tone`（Perfect 880/EPerfect 740/Early 520/Miss&Multipress 180；无声卡静音不报错）；关卡 `/bin/DEMO1.TAD`、`/bin/DEMO2.TAD` 随镜像 | **已修复** |

### 17.1 验证结果

```
新套件 tests/ot_regress.py（端口 4466）16/16 ALL PASS：
  help 列出 cat/rm/cp/mv / format / cp+cat 二进制谱面 / mv 改名 / rm 删除 /
  ocntools（PATH 回退启动 → 工具 help → exit 回 shell）/
  adofai（菜单渲染 → ESC 干净退出）/ faultinfo 全程无 fault
全套无回归：qemu 12/12、user 15/15、diag 4/4、disk 5/5、vprobe 10/10、
  snd 6/6、heap 4/4、go 7/7、gfx24 0 failed、qa exit=0
宿主：test_mkce 48、test_cc 10、test_app_list 23 apps、ce_regress 11P/16S/0F
```

### 17.2 本轮教训（测试侧，非内核）

- FAT `ls` 显示 **8.3 名的原创建大小写**（`C1.TAD` → `c1.tad`），断言必须不区分大小写。
- `format` 确认走 `kb_readline`，**y 之后必须发 Enter**；且 format 后内核重装内置应用
  耗时数秒，期间打字会被吞——要等 "Installed missing built-in" 出现再发下一条命令。
- 读屏竞态：`screendump` 落盘有延迟，关键断言前先做一次"沉淀读"再取值。


## 十八、spawn 执行权限检查（#96，2026-11-11，P1 补完）

本轮目标：闭合 ROADMAP P1 的第一项——`task_spawn_ce` 尚不查 `PERM_EXEC`，导致任何可读文件（包括其他用户创建的 CE 二进制）可被任意 uid 运行，绕过 ACL 的写保护只防"改文件"而不防"跑文件"的漏洞。

| # | 问题 | 确认值 / 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **96** | **`task_spawn_ce` 不查 `PERM_EXEC` ⇒ ring3 可 exec 任何可读 CE** | `task.c:1959` 只对路径做 `fs_resolve_file`（文件存在+大小≥4 即放行），无任何 `fs_perm_check`/ACL 调用。`PERM_EXEC` 常量已在 `task.h:126` 定义但从未在 spawn 路径使用。对比：`SYS_SAVE`/`SYS_FILE_WRITE` 查 `PERM_WRITE`，`SYS_CAT`/`SYS_LS` 查 `PERM_READ`，唯独 `SYS_SPAWN` 漏检 | uid=100（user）可以 `run /root/malicious.ce`（只要文件存在且 size≥4），尽管该文件可能是 root 通过 `chmod 000` 保护的恶意二进制。与已关闭的 write/saver 权限门形成不完整的安全面 | ① 在 `task_spawn_ce` 里 `fs_resolve_file` 成功后追加两段检查：(a) `acl_lookup` 判 file mode 是否含 `0o111`（owner/group/other 任一 execute bit），无则返回 -2；(b) `fs_perm_check(path, uid, user->gid, PERM_EXEC)` 按 owner/group/other 语义强制（root 直通）。调用方（`spawn_ce_redirect` / SYS_SPAWN syscall）的错误处理不动，`err -2`/`err -13` 均触发"cannot load"提示 | **已实现** |

### 18.1 代码改动

```diff
--- a/kernel_v25/src/task.c
+++ b/kernel_v25/src/task.c
@@ -1958,6 +1958,28 @@ int task_spawn_ce(const char *path, int argc, char **argv, int uid) {
     uint32_t clus = 0, size = 0;
     if (!path || fs_resolve_file(path, &clus, &size) != 0) return -2;
     if (size < 4) return -3;
+    /* v2.15: enforce PERM_EXEC on the path before loading. */
+    {
+        int fuid, fgid, fmode = 0;
+        if (acl_lookup(path, &fuid, &fgid, &fmode) == 0 &&
+            !(fmode & 0o111)) {
+            return -2;
+        }
+        user_rec_t *u = user_by_uid(uid);
+        if (fs_perm_check(path, uid, u ? u->gid : 0, PERM_EXEC) != 0)
+            return -13;
+    }
```

### 18.2 行为影响评估

- **正常流程不受影响**：`/bin/*.ce` 是 0755（有 x），`/home/*` 下由用户自己创建的文件也是 0644（无 x），但用户在自己的 home 目录创建的新 `.ce` 文件需先 `chmod +x` 才能 run——这符合预期（安全增强而非破坏性变更）。
- **已有回归风险极低**：所有 bundled apps（hello/calc/sysinfo/.../ocntools/adofai）在 `/bin` 目录下，mode=0755，PERM_EXEC 全过。唯一可能受影响的是用户自制的未 chmod +x 的 CE 文件——这是设计意图。
- **错误码选择**：无 execute bit 时返 -2（复用 ENOENT-ish，避免引入新的错误码污染 API 表）；其他 DENY 场景统一返 -13（EACCES，与 syscall 层其他权限拒绝一致）。

### 18.3 待验证（QEMU 实机，云侧环境）

- `tests/user_regress.py` 新增步骤：以 guest（uid=100）`run /bin/hello.ce`（OK）vs `run /home/user/noperm.ce`（应报 err -13，需 root 先 `chmod +x`）
- `tests/qemu_regress.py` 全套 12/12 不应受影响
- `tests/ot_regress.py` 16/16 不应受影响（adofai/ocntools 均在 /bin）

## 十九、截图审计轮：构建链、ACL 绕过、显示与字体（#97~#102，2026-11-12）

用户送来 12 张 QEMU 实机截图要求"检查是否有 bug"。逐张诊断出 6 个独立问题，
全部修复并以 `tests/security_regress.py`（17 用例，端口 4474）锁死回归。

| # | 问题 | 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **97** | **ACL 路径大小写绕过（P0）** | `acl_lookup`/`fs_perm_check` 逐字节比较路径；FAT 目录名实际大小写不敏感，`run /BIN/XTEST.CE`、`run ./xtest.CE` 均能绕过 `chmod 000` | 权限模型被路径拼写击穿 | `fs.c` 新增 `fs_canon_path()`（复用 cwd/path_norm 语义输出规范化绝对路径），`user.c` 的 `acl_lookup`/`acl_set`/`fs_perm_check` 与 spawn 检查统一先规范化再大小写不敏感比较；点号/斜杠变体回归锁死 | **已修复** |
| **98** | **`cmd_chmod` 未初始化变量** | `shell.c` 的 `fuid/fgid/fmode` 未初始化；`acl_lookup` 失败路径下把栈垃圾写进 ACL | chmod 失败仍可能污染 /etc/acl | 三变量显式初始化（fmode=0644, fgid=0）；`task_spawn_ce` 内 `fuid/fgid` 同修 | **已修复** |
| **99** | **构建链骗测（P0，流程性）** | 只跑 `patch_work_iso.py` 重打包、未重编译 ⇒ task.o 里根本没有 #96 检查，测试"通过"了个旧内核。`nm build/task.o` 查符号 + kernel.bin md5 与 ISO 内嵌比对可复现 | 一切"已修复"结论不可信 | `patch_work_iso.py` 增加陈旧 kernel 拒绝打包（比对 src/include mtime，`--force` 可越权）；`build.sh iso` 目标补 `build_apps` 依赖（缺 CE 产物曾直接构建失败） | **已修复** |
| **100** | **vdiag 显示残影/调色板污染** | ① text_diag 的 glyphA hex 输出逐字符 con_write，中途切页把已打印内容顶走（截图误判为字符残影，实为 `con_set_cursor(0,3)` 覆盖）；② attrpal 循环写 index 前未重触发 AC（`inb(0x3BA)`），首个字节把调色板写成黑→紫，像素 diff 65.85% | 诊断工具自身破坏被诊断状态 | ① hex 先收进局部 `char hex[17]`，恢复 VGA 状态后一次性输出；② 每次 `(void)inb(0x3DA)` 重触发 AC 再写 index。vdiag 前后 ls 输出像素一致 + attrpal 恢复 `00 01 02 03…` 由 security_regress 断言 | **已修复** |
| **101** | **CJK 全角标点缺字形（`、。！？` 等显示 `?`）** | `font_hz.inc` 只含 GB2312 汉字子集（SUBSET.hex），标点区（A1/A3 行）无字形；`render.c` 宽字符判定硬编码 `cp>=0x1100` 且 0x80-0xFF 越界读字体表 | 中文标点全部变 `?`，CJK 排版不完整 | ① 新增 `tools/unifont-cjk-punct.hex`（198 个标点字形，GNU Unifont 16.0.01，GB2312 A1/A3 行 + 常用全角标点），`tools/gen_hz_font.py` 合并两份 hex（并修 8x16 半宽 pad），产出 6961 字形的 `font_hz.inc`；② `render.c` 判定改为 `if (be->cjk && render_font_hz(cp)) wide=1; else cp='?'`。字形许可：Unifont GPLv2+ 字体嵌入例外/OFL 1.1，可离线再生成 | **已修复** |
| **102** | **`build.sh iso` 缺 `build_apps` 依赖** | `iso` 目标直接调 patch_work_iso.py；CE 应用未编译时（干净树/新 clone）构建中途失败 | 构建不可重现 | `build.sh` 的 `iso` 目标先调 `build_apps` | **已修复** |

### 19.1 回归与验证（2026-11-12）

- `tests/security_regress.py`（新增）：17 用例 —— chmod 000 正反断言、
  大小写/点号/斜杠路径变体拒绝、`err -13` 语义、vdiag 前后背景像素比较、
  glyphA 内联、faultinfo。17/17 PASS。
- 全套回归（同 ISO `catos.iso`，2026-11-12 构建）：

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/17S/0F
实机: qemu_regress 12/12  user_regress 15/15  ot_regress ALL  diag_regress 4/4
      disk_regress 5/5  heap_regress 4/4  go_regress 7/7  vprobe_regress 10/10
      snd_regress 6/6  gfx24_regress 0 failed  security_regress 17/17
```

### 19.2 澄清：磁盘镜像与 VMware 引导（非 bug）

诊断过程中确认的事实（详见 `vmware/README.md`）：

- `tests/qemu_disk_*.img` / `catos_disk.img` 是**纯数据盘**：MBR 引导区全零
  （mkdisk.py 只写分区表 + 0x55AA），不含任何引导代码。拿它引导在任何机器
  （含 QEMU）都会卡死，与"CatOS 自定义 bootloader"无关。
- 引导介质一直是 `catos.iso`：GRUB isohybrid 双格式（扇区 0 有 isohybrid
  MBR `eb 63 90` + El Torito 引导目录）。已实测当 IDE 硬盘 `-boot order=c`
  引导到 `root@catos:/$`（截图 tests/qemu_shots/hdbootA/B.png）。
- VMware 无需 grub-mkrescue：`vmware/CatOS.vmx` + 两个 raw vmdk 描述符
  （catos-boot.vmdk / catos-data.vmdk）即可启动；推荐光驱 El Torito 引导 +
  数据盘挂 IDE 0:0（内核 ata_identify 取第一个可 IDENTIFY 盘挂 FAT32）。

## 二十、开机体验轮：退格残影与自启动机制（#103，2026-11-12）

| # | 问题 | 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **103** | **退格留下 `?` 残影（用户 VMware 实测发现，QEMU 同样复现）** | `be_text.c` 的 `text_draw_cell` 对空格子（`cp==0`，由退格的 `cell_blank()` 产生）走 `c->cp <= 0xFF ? '?' : ' '` 分支 ⇒ 每次退格在光标处画一个 `?`。回归从未覆盖退格输入，#96~#102 轮都没暴露 | 每次退格留下问号残影，删除操作视觉上"越删越多" | `text_draw_cell` 改为仅 `cp∈[0x20,0x7E]` 显示原字符、否则画空格。验证：输 cba + 3 次退格 → 屏幕干净无残留（tests/qemu_shots/bs2.png）；security 17/17、user 15/15、qemu 12/12 全绿 | **已修复** |

**根因教训**：`cell_blank()` 与 `text_draw_cell()` 对"空格子"的约定不一致（blank 认为空=可重画，draw 把空画成 ?）。渲染层的 cp==0 语义在三个后端应统一为空格。

---
*上一节后继续编号为 #104+。*

## 二十一、VMware 图形轮：v2.20 SVGA II 24 位真彩（#104~#106，2026-11-13）

来源：用户 2026-11-13「下载文件夹有最新版。顺便做 VMware 24 色真彩支持。
我输入 vbe 和 vbe24 都不行」。同时把云主机 v2.19 反向同步回 `D:\CatOs`
（提交 `3ed5cad`，覆盖前备份 `D:\_v219_backup\`）。

| # | 问题 | 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **104** | **VMware 下 `vbe` / `gfx24` 报 "no VBE/Bochs-dispi device found"，完全无图形** | 图形后端只识别 QEMU/Bochs 的 0x01CE dispi 端口；VMware 显卡是 PCI `15AD:0405` SVGA II（BAR0 索引/数据端口、BAR1 LFB、BAR2 FIFO），两者协议完全不同 | VMware 用户无法进入任何 800x600 真彩模式 | 新增 `src/svga.c` / `include/svga.h`：PCI 身份优先选后端、寄存器模式设置、FIFO `SVGA_CMD_UPDATE` 25 Hz 刷新、运行时 pitch/stride/通道偏移。`be_vbe24`/`be_vbe` 双后端共用 | **已修复** |
| **105** | **`vbe24` 命令不存在**（用户输入后无反应/未知命令） | shell 分发链只注册了 `gfx24`，文档与 `cmd_display` 的 "->vbe24" 文案却暗示存在 | 用户按文档输入无效，误以为显卡驱动坏了 | `gfx24` / `vbe24` 双别名指向同一 `cmd_gfx24()`；help 与测试文档同步 | **已修复** |
| **106** | **QEMU `-vga vmware` 模拟的 SVGA II 上屏幕全黑** | QEMU 的 vmware-svga 复用通用 VGA 核，**仍应答 dispi ID**；旧代码优先走 Bochs 路径，`find_lfb()` 找不到 `1234:1111` 就退回硬编码 `0xE0000000`，像素写进不存在的地址 | 用 QEMU 同族设备无法验证 VMware 路径，且会误判为"驱动已坏" | 后端选择改为 **PCI 身份优先**：探测到 `15AD:0405` 就走 SVGA，dispi 仅兜底；`svgainfo` 命令可直接核对 ID/FB/FIFO/UPDATE 计数 | **已修复** |

**连带修正（非缺陷）**：

- `paging.c::vbe_lfb_map()` 原样覆盖 `lfb_pde_first/last`，第二个 BAR 映射会让
  第一个从 ring3 目录镜像中掉出去；改为取区间并集（#84 的 supervisor-only PTE 不变）。
- `extapp/gfx24.c` 原硬断言 `pitch==2400`；SVGA 是 3200，ring3 探针会打印
  `GFX24 CE FAIL (bad info)` 且不绘图。改为 `pitch >= w*3`，逻辑 RGB24 契约不变。
- `gfx_info` 追加只读选择器 `BACKEND(7)` / `HW_BPP(8)` / `STRIDE(9)`
  （append-only，不新增 syscall 槽），`ce_abi.h` 与 `gfx24.h` 同步。

**验证**：`tests/svga24_regress.py`（QEMU 11.1 `-vga vmware`，monitor 4467）
34 项全绿；`gfx24_regress`（`-vga std`）0 failed；qemu 12/12、user 15/15、
security 17/17、disk 5/5、宿主 test_mkce 48 / test_cc 10 / ce_regress 11P /
test_app_list 24 全绿。**真实 VMware Workstation 需用户开机复测**。

**根因教训**：设备能力判定不能只靠"某个 I/O 端口有没有回应"——QEMU 的
VGA 兼容层会让不同显卡都应答同一组端口。PCI vendor/device 才是身份事实。

---
*本节后继续编号为 #107+。*

## 二十二、v2.21 外设与交互轮 + 光标/黑屏修复（#107~#111，2026-11-13）

来源：用户 2026-11-13「我在 VMware 运行 catos 输入 vbe 命令且运行后虚拟机就黑屏了，
vbe24 命令后无法输入，gfx 模式中 光标应该是[当前所在格无字符→变为下划线：为该格增加
一个背景反色样式]且光标应该闪烁，换行后不应该出现光标」。

| # | 问题 | 根因 | 影响 | 处置 | 状态 |
|---|---|---|---|---|---|
| **107** | **VMware 下 `vbe` 进入后整屏黑** | `paging.c::vbe_lfb_map()` 把显存/PCI MMIO 映射成普通可缓存页（`0x003` = P\|RW）。QEMU 忽略缓存位所以看不出来，但真实 VMware 设备读的是**物理内存**，CPU 的写回缓存里它读不到 ⇒ 显存永远读回旧值/零 ⇒ 黑屏 | VMware 真机进不了任何真彩模式 | 映射属性改为 `0x013`（P\|RW\|**PCD**，不可缓存）。真实设备与 QEMU 行为就此对齐 | **已修复** |
| **108** | **光标从不闪烁；VMware SVGA II 屏幕从不刷新** | **`cpu.s` 把 IRQ0 单独接到 `_sched_timer_tick`（`task.c`），根本不经过 `intr.c` 的 `isr_irq`**；而 `con_tick()`（闪烁相位）与 `svga_tick()`（SVGA FIFO `UPDATE`）只挂在 `isr_irq` 的 `case 0` 上 ⇒ **这两段代码从上线起就是死代码** | 图形模式光标永远常亮（用户明确要求闪烁）；VMware 的 SVGA FIFO 从不收到 `UPDATE` ⇒ 显存内容永远不刷到屏幕（黑屏主因之一） | `sched_timer_tick()` 内直接调用 `svga_tick()` + `con_tick()`；`svga_tick` 自带 100 Hz→25 Hz 分频 | **已修复** |
| **109** | **图形模式光标是 8×8 实心块** | `be_vbe` / `be_vbe24` / `be_gfx` 的 `draw_cursor()` 沿用 #58 的"下半格方块"；用户要求"当前格无字符→下划线，有字符→背景反色" | 光标样式与用户预期不符 | 三个后端的 `draw_cursor()` 改为在**格子最后一条扫描线**画 1 px 下划线（窄格 8 px、宽格 16 px）；`render.c::paint()` 对**有字**格子改画 fg/bg 互换的反色块；文本模式硬件光标也由 13–14 扫描线改为 14–15（1 px） | **已修复** |
| **110** | **换行/编辑后留下残光标** | ① `con_set_cursor()` 把 `dcx/dcy` 置 `-1`，`sync_cursor()` 因此**不再重绘旧格子**，旧光标像素留在屏上；② `scroll_one()` 先滚动再擦光标，光标像素随视口一起滚走，在上一行留下一道残影 | 每次退格/编辑/换行都会多出一道光标 | `con_set_cursor()` 保留 `dcx/dcy`；`scroll_one()` 在滚动**之前**先用无光标的 `paint()` 擦掉旧光标 | **已修复** |
| **111** | **`heap_regress` 的 `ls /bin` 断言过期** | v2.18 给 FAT 加了 VFAT **LFN（长文件名）**支持，`ls` 打印的是长名 `allocprobe.ce`；测试仍在断言 8.3 截断名 `allocpro.ce` | 回归假失败（`allocpro.ce=False heaptest.ce=True`），与内核无关 | 测试断言改为长名，注释里写明 LFN 的来龙去脉 | **已修复** |

### 22.1 光标行为逐像素验证（QEMU `-vga std`，同一 ISO）

| 用户要求 | 实测（帧缓冲逐像素） | 结论 |
|---|---|---|
| 当前格**无字符**→下划线 | 变化像素恰好 **8 个、高 1 行**，`y%16==15`（= 格子最后一条扫描线） | ✅ 1 px 下划线 |
| 当前格**有字符**→背景反色 | 整格 8×16 反转，角像素 `(0,0,0)` ↔ `(85,255,255)`（fg/bg 互换） | ✅ 反色块 |
| 光标应该**闪烁** | 相邻帧交替出现/消失（帧间变化像素 0 / 64 交替） | ✅ 在闪 |
| 换行后**不应出现光标** | 换行前后逐帧统计"孤立 8 px 下划线"数量，**始终 ≤ 1** | ✅ 无残光标 |

### 22.2 CJK = 2×ASCII 逐像素断言（新增 `tests/cjk_width_regress.py`，monitor 4493）

`UNDERLINE` 属性在**每个格子的最后一条扫描线**画一条线（窄格 8 px、宽格 16 px），
因此下划线连续段就是一把精确的格子标尺；同屏的纯 ASCII 行背景色给出独立基准：

| 对象 | 组成 | 期望像素 | 实测 |
|---|---|---|---|
| 第 1 行（纯 ASCII，49 格） | 49 × 8 | **392 px** | 392 px ✅ |
| 第 3 行（12 ASCII + **14** CJK） | 12×8 + 14×16 | **320 px** | 320 px ✅ |

⇒ ASCII 步进 = 392/49 = **8 px**；CJK 步进 = (320 − 12×8)/14 = **16 px = 2×ASCII** ✅
（若 CJK 退化成 1 格宽则该行应为 224 px，退化成 3 格宽则应为 416 px，两者都会被判失败。）

### 22.3 回归总账（2026-11-13 同一 ISO）

```
宿主: test_mkce 48  test_cc 10  test_app_list 24  ce_regress 11P/17S/0F
实机(-vga std): qemu 12/12  user 15/15  security 17/17  disk 5/5  diag 4/4
                heap 4/4  go 7/7  snd 6/6  e1000 0F  es1371 0F
                gfx24_regress 0F  svga24_regress 0F  cjk_width_regress 0F(新)
```

`svga24_regress` 报 `SVGA FIFO issued UPDATE commands: updates=382` —— #108 修复前
该计数恒为 0（`svga_tick()` 是死代码），现在 FIFO 真的在推 `UPDATE` 了。

**真实 VMware Workstation 仍需用户开机复测**（#107 黑屏、#108 刷新、#109/#110 光标）。

**根因教训**：IRQ 编号 ≠ ISR 入口。`cpu.s` 为 IRQ0 单独开了一条汇编入口
（`_irq_0` → `_sched_timer_tick`），任何挂在通用 `isr_irq` 上的周期钩子在那条路径上
都是**死代码** —— "写了但没接线"比"没写"更危险，因为它看起来完全正常。

---
*本节后继续编号为 #112+。*

## 二十三、v2.22~v2.24 跨夜轮（#112~#113，2026-11-14）

| # | 严重度 | 问题 | 根因 | 修复 | 状态 |
|---|---|---|---|---|---|
| 112 | 高 | ring0 shell 的 `mkdir` 不做父目录写权限检查，非 root 用户可直接在 /etc、/bin 下建目录 | shell 自身在 ring0，ACL 只在 fs_perm_check 调用点生效，mkdir 从未调用 | v2.24 `perm_write_on_parent()` 统一守卫 mkdir/cp -r（fsrec_regress 6 步断言） | ✅ 已修 |
| 113 | 中 | rm/mv 删除文件后 /etc/acl 留悬空条目，重建同名文件继承旧权限（如旧 chmod 000） | 删除路径不清理 ACL 表 | v2.24 新增 `acl_unset()`，rm/rm -r/mv 删除成功即清除条目 | ✅ 已修 |
| 114 | 低 | `fs_copy_file` 给副本写死 0644（无执行位），副本 .ce 比"无条目=0755"默认语义更受限，`run` 报 err -13 | 创建者 ACL 用了 0644 | v2.24 改为 0755，与文档化默认一致 | ✅ 已修 |

*本节后继续编号为 #115+。*

| 116 | 高 | 无 IDE 硬盘时 `format` 死循环假死（分钟级无响应） | fat_mkfs 对 IDENTIFY 失败 fallback 到虚构 64MiB 卷继续 mkfs，每个扇区写都烧满 10M 迭代超时 | v2.32：fat_mkfs/fs_format 改 int 签名，无盘立即返回 -1，shell 报 "no IDE disk attached" | ✅ 已修 |
