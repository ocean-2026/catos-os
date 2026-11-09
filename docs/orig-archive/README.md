# 原项目文档归档索引（`docs/orig-archive/`）

> 本目录收录**原项目** `D:/CatOs/Catos_Better_better/`（只读）的历史文档。
> 它们**不参与构建、不参与测试**，唯一用途是保留「当初为什么这么设计」的上下文，
> 供后续实现阶段（`docs/merge-harvest-design.md`）查阅。
>
> **归档原则**（用户拍板的 (b)+(c) 并行策略）
> 1. **(b) 归档**：把有增量价值的原项目文档收进本目录并写明「是否仍然成立」；
> 2. **(c) 入档**：把原项目知道、现项目 `docs/CatOS-内核问题清单.md` 未记录的遗留问题
>    正式写进现项目问题清单（见 `docs/CatOS-内核问题清单.md` 第一节 `#48`~`#50`
>    与第四节 `#51`~`#65`）。
>
> 归档日期：2026-11-09 ｜ 来源目录：`D:/CatOs/Catos_Better_better/`（全程只读，未写入一个字节）

---

## 一、归档清单（5 份）

| # | 归档后路径 | 原始路径 | 大小 | 一句话摘要 |
|---|---|---|---|---|
| A-1 | `docs/GO-DESIGN.md` | `doc/GO-DESIGN.md` | 5,569 B | `go.ce` Go 子集解释器设计文档：CE 契约、词法/语法/求值分层规格、自举定义、QEMU 验收清单、FIND-G1~G10 迭代状态 |
| A-2 | `docs/go-review.md` | `doc/go-review.md` | 16,533 B | `go.c` 只读静态逐行全量审查报告 + 宿主侧独立复验，输出 FIND-G1~G10 汇总表（无 P0，2 P1 + 8 P2） |
| A-3 | `docs/orig-archive/legacy-TODO_STAGE5.md` | `doc/TODO_STAGE5.md` | 703 B | Stage5 待办清单：CC2_NEW.CE vs HOSTCC2.CE 尺寸差异调查、自举固定点验证、QEMU 实机回归、重建 128 KiB 栈 ISO |
| A-4 | `docs/orig-archive/legacy-BUILD_REPORT_STAGE5.md` | `doc/BUILD_REPORT_STAGE5.md` | 742 B | Stage5 构建报告：mingw32 工具链 PATH 根因与解法，遗留 3 项待验证（字节级一致 / QEMU / 自举固定点） |
| A-5 | `docs/orig-archive/legacy-HANDOFF_STAGE5.md` | `doc/HANDOFF_STAGE5.md` | 5,512 B | Stage5 自举环交接单：cc.c 同步、USER_STACK_SZ 提升、HOSTCC2_REF 参考值、4 个阻塞问题、Windows 环境尝试记录表、**forward-call-fix 修复说明**、自举固定点验证标准 |

**未归档的原项目文档（已评估，判定为不归档）**

| 原路径 | 不归档理由 |
|---|---|
| `doc/CatOS-内核问题清单.md` | 现项目同名文件**更新且更长**（现 20,275 B vs 原 19,692 B），已覆盖原条目；原版独有条目已按 §2.7 逐条提取入档（`#48`~`#65`） |
| `doc/PROGRESS.md` | 现项目同名文件更新；原版是云侧 `/home/user/Doubao/...` 工作区的进度快照，无增量信息 |
| `doc/history.md` | 现项目同名文件更新；原版末段与 `PROGRESS.md` 重复 |
| `doc/CatOS-待办与成果汇报-2026-10-31.md` | 云侧阶段性汇报（组织者视角的临时状态），其中的**事实结论**已分别落到 A-1~A-5 与问题清单 |
| `doc/STAGE5_STATUS.md` | 与 A-3/A-4/A-5 内容高度重叠（同一次 Stage5 的三份快照） |
| `doc/KALI_VM_SETUP.md` | Kali Linux 专用环境搭建说明，与现项目 Windows + MSYS2 mingw32 开发环境无关 |
| `CATOS-交接文档.md`（原项目根） | 目录结构说明已被 A-5 + 现项目 `README.md` 取代 |
| `README.md`（原项目根） | v2.5 一次性改造快照说明，对应的改动已全部并入现项目内核 |
| `pe.ld`（原项目根） | 与现项目 `kernel_v25/pe.ld` **逐字节相同**（已 `diff` 核实），无需归档 |

---

## 二、逐份「是否仍然成立」判定

> 每份文档都**实际读过**（不凭文件名猜）。判定依据写在最后一列。

### A-1 `docs/GO-DESIGN.md` —— **部分成立（设计有效、ABI 与缺陷状态已过期）**

| 章节 | 判定 | 依据 |
|---|---|---|
| §1 目标、§3 子集规格表、§4 自举定义 | ✅ 仍成立 | `extapp/go.c` 的分层实现与规格表逐项对得上；`extapp/cases/` 里 `MINI_GO.GO`/`MINI_IN.GO` 两个自举用例真实存在 |
| §2 CE 契约（48 槽） | ⚠️ **已过期** | 现项目 ABI 已是 **57 槽**（`kernel_v25/include/ce_abi.h`）。但 go.c 实际只调用 `write/putc/print_int/alloc/atoi/file_exists/file_read` 7 个槽，全部 ≤39 号，落在 append-only 旧前缀内 ⇒ **57 槽下无需改 ABI** |
| §3 容量（`g_filebuf[20480]`、arena 192 KiB） | ✅ 仍成立 | 实测 `go.c:1083` `g_filebuf[20480]`、`go.c:81` `a_alloc` 走 `api->alloc` |
| §5 QEMU 验收清单（4/4 PASS） | ✅ 仍成立（作为原项目历史事实） | 与原项目 `go-review.md` 的宿主复验结果互相印证 |
| §6「未修（P2）」FIND-G3~G10 | ❌ **文档已过期：源码里这 8 条全部已修** | 逐条源码证据（见下方「FIND-G 逐条核实」） |
| §6「已修复（v1.1）」FIND-G1/G2 | ✅ 仍成立 | `go.c:171` `Tok.line` + `:197` `t->line = g_line`（G1）；`go.c:980` 字符串拼接按需 arena 分配（G2） |

### A-2 `docs/go-review.md` —— **审查方法与 FIND 清单仍有效，缺陷状态已过期**

| 项 | 判定 | 依据 |
|---|---|---|
| 审查方法声明（只读静态逐行 + 宿主侧独立复验） | ✅ 仍成立且方法学可复用 | 与现项目 `tests/host_shim.c` 的宿主桩思路一致，可作为 `go.c` 移植后的宿主侧验证模板 |
| 「总体判定 PASS，无 P0」 | ✅ 仍成立 | 宿主复验输出与 QEMU 实测宣称逐行一致 |
| FIND-G1~G10 清单（**清单本身**） | ✅ **仍有效** | 10 条缺陷的**识别**准确，是移植 `go.c` 时必须逐条回归的检查表 |
| FIND-G1~G10 的**修复状态** | ❌ **已过期**（审查对象是 1,161 行旧版；库中 `go.c` 已 1,300 行且 G1~G10 全部带 FIND 注释修复） | 见下方「FIND-G 逐条核实」 |
| §D「48 槽逐一对齐」结论 | ⚠️ **需按 57 槽重做** | 实测 go.c 只用 7 个槽（≤39），兼容性结论比原文档更强，但表格本身须重算 |
| §E 内存安全结论 | ✅ 仍成立 | `g_filebuf` 复用安全、字符串 NUL 终止等结论在 1,300 行版本中依旧成立 |

### A-3 `docs/orig-archive/legacy-TODO_STAGE5.md` —— **部分成立（3 条仍成立且现项目未记录）**

| 待办项 | 判定 | 依据 |
|---|---|---|
| 「CC2_NEW.CE vs HOSTCC2.CE 大小差异」 | ✅ **仍成立** | 原项目记录 82,586 B vs 81,522 B；`build/CC2_NEW.CE` md5 = `d21d66ea5845492dab406d04f666c60e`（= 已从根目录删除的 `Catos_Better_b`），`HOSTCC2.CE`/`HOSTCC2_REF.CE` md5 = `eeec32f838fbab4c111d428df22aa409` |
| 「QEMU 实机回归测试」 | ⚠️ 原项目成立 / **现项目已做** | 现项目 `tests/qemu_regress.py` 已有 12/12 PASS |
| 「验证 CC2.CE 自举（CC2 → CC3 字节级一致）」 | ✅ **仍成立且现项目未记录** | 落档为 `docs/CatOS-内核问题清单.md` 第四节 `#52` |
| 「重建内核 ISO（USER_STACK_SZ=0x20000）」 | ✅ **仍成立** | 现项目 `kernel_v25/include/paging.h:82` 仍是 `0x10000u`（64 KiB），未同步；落档为第一节 `#49` |
| 技术笔记「工具链 /c/msys64/mingw32（gcc Rev4 16.2.0）」 | ❌ 已过期 | 现项目构建链用 `/d/msys/mingw32`（gcc 16.1.0 Rev5），实测可出 ISO |

### A-4 `docs/orig-archive/legacy-BUILD_REPORT_STAGE5.md` —— **部分成立（根因已解决，3 项待验证仍在）**

| 项 | 判定 | 依据 |
|---|---|---|
| mingw32 工具链 PATH 根因分析 | ❌ **已解决，不再适用** | 现项目用 `/d/msys/mingw32` 且实测构建成功产出 16,558,080 B ISO；原文档描述的「链接失败 exit 49」在本机不复现 |
| 「CC2_NEW.CE 与 HOSTCC2.CE 字节级一致」待验证 | ✅ 仍成立 | 落档为第四节 `#52` |
| 「QEMU 实机回归」待验证 | ✅ 仍成立（原项目）/ 现项目已做 | 见 A-3 |
| 「CC2.CE 自举固定点」待验证 | ✅ 仍成立 | 同 `#52` |
| 「gcc 版本差异导致尺寸差」观察 | ✅ 仍成立 | 82,586 vs 81,522 B；现项目同样存在 Rev4/Rev5 差异（committed 的 `.ce` 内嵌 `Rev4`，本地重编内嵌 `Rev5`） |

### A-5 `docs/orig-archive/legacy-HANDOFF_STAGE5.md` —— **部分成立（含 2 条高价值信息）**

| 项 | 判定 | 依据 |
|---|---|---|
| `USER_STACK_SZ` 0x10000 → 0x20000 的**改动与理由** | ✅ **仍成立且现项目未同步** | 落档为第一节 `#49` |
| **forward-call-fix（L1829）修复说明** | ⚠️ **待架构师核对** | 原项目 `extapp/cc.c:1832` 有「原型符号复用」逻辑（`gfind` 找到则复用、否则 `sym_new`）；现项目 `kernel_v25/extapp/cc_self.c:1879` 直接 `sym_new()`、`:421` 有 `gfind()` 但未见同一处复用 ⇒ **该修复是否已并入现项目需逐行核对**（我未确认，属待核对项，不作断言） |
| `HOSTCC2_REF.CE` 参考值（81,522 B / md5 `eeec32f838fbab4c111d428df22aa409`） | ✅ 仍有用 | 作为 P2-1 自举固定点验证的比对基准 |
| 4 个阻塞问题（宿主编译器过旧 / mingw32 链接失败 / QEMU 未装 / ISO 陈旧） | ❌ 前 3 项在现项目不成立 | 工具链可用、QEMU 在 `C:\qemu-portable\qemu-system-i386.exe`；仅「ISO 陈旧」部分仍需关注 |
| Windows 环境尝试记录表（5 种 GCC 路径 × 失败原因） | ❌ 历史记录，现已解决 | 保留作参考，不再是阻塞 |
| `build/verify_bootstrap.sh` 一键验证脚本 | ⚠️ **值得移植** | 脚本逻辑（验 md5 → 查栈配置 → 编宿主 → 自举 → 比对 md5 → 回归）仍有效；仅路径硬编码需改（PRD P2-2） |

---

## 三、FIND-G 逐条核实（对原项目 `extapp/go.c`，md5 `f9876f42fa646c58ec21da649ed934e4`，1,300 行）

> **重要更正**：`go-review.md`（2026-10-30）审查的是 **1,161 行**的旧版；
> `GO-DESIGN.md` §6 把 FIND-G3~G10 列为「未修」，但库中 `go.c`（2026-10-31）
> **已带 FIND 注释逐条修复**。即：**缺陷识别有效，缺陷状态已过期**。
> 用户拍板的「8 个 P2 全部修」在**源码层面已由原项目完成**，移植时需要做的是
> **逐条验证修复被一并带过去**，而非重新实现。

| FIND | 级别 | 描述 | 原项目 `go.c` 中的修复证据 | 现状 |
|---|---|---|---|---|
| G1 | P1 | 语法错误行号恒为文件总行数 | `go.c:171` `int line;`（Tok 增行号字段）+ `:197` `t->line = g_line` + `:226` `start_line` | ✅ 已修 |
| G2 | P1 | 字符串 `+` 拼接栈上 `tmp[200]` 硬截断 199 字节 | `go.c:910` display-length 辅助 + `:980` 拼接缓冲按两操作数长度动态分配 | ✅ 已修 |
| G3 | P2 | 无递归深度上限 | `go.c:783` `g_call_depth` + `:784` `CALL_DEPTH_MAX 200` + `:1048` 入口检查 + `:1053/:1058/:1074` 增减配平 | ✅ 已修 |
| G4 | P2 | 非法字符被词法 default 静默吞 | `go.c:358-362` default 分支置 `g_perr`/`g_errline` → 报词法错误 | ✅ 已修 |
| G5 | P2 | `readfile` 请求 20479 > 内核单次 16384 上限 | `go.c:1085` 新增 `read_whole()`，按 `FS_CHUNK 16384` 分块循环读至 EOF | ✅ 已修 |
| G6 | P2 | arena 扩容 `s->v`/`a->v` 未判空 | `go.c:701` `if (!s->v) { g_oom = 1; return 0; }` + `:732` 同款 `a->v` 判空 | ✅ 已修 |
| G7 | P2 | 字符串字面量 >1023 静默截断 / 未闭合引号不报错 | `go.c:223-235` 两遍扫描：先数解码长度再按 arena 尺寸分配；未闭合引号报词法错误 | ✅ 已修 |
| G8 | P2 | 顶层 break/continue/return 静默停跑 | `go.c:188-191` 解析期嵌套计数器 + `:658` return / `:665` break / `:669` continue 三处 `depth==0` 报错 | ✅ 已修 |
| G9 | P2 | for init `:=` 变量泄漏外层作用域 | `go.c:882-899` 每个 for 循环建独立 `Scope`（parent = 调用者作用域），结束恢复 `g_scope` | ✅ 已修 |
| G10 | P2 | 数字字面量溢出无检测（int32 UB） | `go.c:302-313` 扫描时检测 `> 2147483647` 并置词法错误 | ✅ 已修 |

---

## 四、原项目 `tools/` 三个 Python 脚本评估（**已评估，全部不移植**）

> 三个脚本**全部实际试跑**过（在隔离工作树内、只读引用原项目），不是凭文件名判断。

| 脚本 | 行数 | 试跑结果 | 判定 |
|---|---|---|---|
| `tools/cc_stage45_test.py` | 196 | ❌ **跑不起来**：`subprocess.run(['pkill','-9','-x','qemu-system-i38'])` → `FileNotFoundError`（Linux-only 命令，Windows 无 `pkill`）；另硬编码 `/home/user/Doubao/chats/38448438262014978/catos-qemu` | **不移植** —— 功能已被现项目 `tests/qemu_regress.py`（QEMU monitor 驱动，12/12 PASS）覆盖 |
| `tools/diag2_test.py` | 129 | ❌ **跑不起来**：同上 `pkill` + 同款硬编码 Linux 路径 | **不移植** —— 诊断 `parse_type`/`accept_op(OP_STAR)` 的价值已由现项目 `tests/test_cc.py` 覆盖 |
| `tools/inject_any.py` | 156 | ⚠️ 脚本本身可执行（无参时报 `IndexError` 属正常用法问题），但**几何参数在现项目上必然失败**：`RSVD=33`/`SPC=8`/`FAT_SZ=8192` vs 内核 `src/fat.c:8` `RESERVED 32u` 且 `fat_sz`/`spc` 由卷大小动态决定 ⇒ 报 `BIN dir not found` | **不移植原文件；改为修现项目副本**（`tests/inject_any.py` 与它**逐字节相同**，等于现项目继承了一份失效工具）→ 落档为第四节 `#64` |

**原项目 `tools/TNC2.C`（1 行，`char *gv; int main(void){ gv="hi"; return 0; }`）**
—— 极小但有效的 DREL 数据区重定位回归用例。**归入移植清单（PRD P1-6），不在本阶段归档**，
以免与实现阶段重复搬运。

---

## 五、相关文档

- 现项目问题清单（本次落档 D-1~D-15）：`docs/CatOS-内核问题清单.md`
- 需求分析与移植清单（PM 产出，本归档的判定依据）：`docs/merge-harvest-PRD.md`
- 合并期评估与执行结论：`docs/merge-harvest-assess.md`
