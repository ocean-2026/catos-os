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
