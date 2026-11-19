# CatOS 合并取精华 —— 评估与执行结论

> **文档定位**：本次「现项目 × 原项目合并取精华」第一阶段（仓库治理 + 资产收割 + 构建可复现性）
> 的**评估结论与执行记录**。
>
> **上游依据**：需求分析见 `docs/merge-harvest-PRD.md`（PM 产出，637 行，全文实测）。
> 本文档**不重复** PRD 的逐条核实过程，只回答四个问题：
> **做了什么** / **依据是什么** / **哪些结论被推翻了** / **下一步谁做什么**。
>
> **执行者**：软件工程师（Kou）｜ **隔离工作树**：`C:/Users/Admin/WorkBuddy/Worktrees/CatOs/master-87a3e1a0`
> （分支 `workbuddy/todo-20261106-1906`）｜ **执行日期**：2026-11-09
>
> **硬约束遵守情况**：`D:/CatOs` 与 `D:/CatOs/Catos_Better_better` **全程只读**
> （V-17：文件级 diff 零越界写入，仅读取与复制出来）；所有写入只发生在隔离工作树内；
> 未 push。

---

## 一、TL;DR

| 项 | 结论 |
|---|---|
| **E1 根目录治理** | ✅ 完成。删除 **18 个**零引用快照文件，迁移 5 份文档入 `docs/`，重写根 `README.md`。根目录从 11 项收敛到 4 项。 |
| **E2 资产归档** | ✅ 完成。归档 5 份原项目文档（逐份判定「是否仍然成立」），把 **11 条**现项目未记录的遗留问题落档进问题清单（表格行数 64 → 82）。 |
| **E3 构建可复现性** | ✅ 完成。方案 (a) 模板入库 + `.gitignore` 否定规则移位。**干净克隆实测出 ISO，12/12 QEMU 回归 PASS**。基线从此具备 CI 意义。 |
| **E4 node/go 移植可行性** | ✅ 结论采信 PRD §2.3/§2.5（PM 已实机跑通）。工程师侧**独立复核确认**，并**补充一条 PRD 未记录的重要发现**（见 §五）。 |
| **基线回归** | ✅ 三项全部保持，无回归（§六）。 |
| **推翻的既有结论** | 3 条（§五）。 |

---

## 二、E1 根目录治理

### 2.1 删除清单（18 个文件，删前逐个全仓 grep 复核）

| 文件 | 大小 | grep 证据 |
|---|---|---|
| `T1.C` `T2.C` `T3.C` `T4.C` `T5.C` `T6.C` | 各 1–37 B | 全仓 `git grep` **零命中**（编译器最小试验碎片） |
| `MIN.C` `MIN2.C` | 各 49–70 B | 全仓 `git grep` **零命中**（最小 CE 程序） |
| `cc_old.c` | 49,134 B | 零命中；且经 diff 判定为现项目 `cc.c` 的**祖先**（757 行唯一公共行），与原项目 `cc.c` 公共行 **0** ⇒ 确为陈旧快照 |
| `cc_old.ce` / `cc_old_ce.exe` | 各 26,952 B | 零命中（前者的编译产物） |
| `vga.c` | 4,992 B | 仅命中**文档叙述**（`docs/CatOS-渲染层前后端分离设计.md:82,161`）与旧根 `build.sh` 注释；功能已下沉到 `kernel_v25/src/vga.c`（27 行瘦包装 → `con_*`） |
| `gfx.c` / `gfx.h` | 15,531 / 1,158 B | 同上，仅文档命中。绘制原语在现项目**不存在** —— 这是 v2.7+ 架构**刻意**不暴露像素 syscall 的结果（`ce_abi.h` 注释："v2.7/v2.8 deliberately expose NO drawing API"），非遗漏 |
| `shell.c` | 49,095 B | 同上，仅文档命中；已并入 `kernel_v25/src/shell.c` |
| `shot-gfx-mode.png` / `shot-text-restored.png` | 4,171 / 1,120 B | 仅 `README.md:44-45` 与 `docs/CatOS-图形模式能力说明.md:170-171` 两处文档表格引用 |
| `Catos_Better_b` | 82,586 B | md5 `d21d66ea5845492dab406d04f666c60e` = 原项目 `build/CC2_NEW.CE` ⇒ 是 Stage-5 自举 cc 的 **CE01 二进制**，不是文档 |

### 2.2 保留与迁移

| 项 | 处置 | 理由 |
|---|---|---|
| 根 `build.sh` | **保留** | 不是残桩 —— 13 行可用委托 shim（`cd kernel_v25; exec sh build.sh "$@"`），实测从根执行能成功产出 iso；且 `docs/CatOS-渲染层前后端分离设计.md:82` 明确记载「`build.sh`（根）改成委托 `kernel_v25/build.sh` 的入口」。PRD §2.9/§4.1 亦判定「⚠️ 保留」。仅更新注释（不再称那些快照文件仍在仓库里） |
| `CatOS-图形模式能力说明.md` | → `docs/` | git mv，内容保留 + 加归档说明 |
| `CatOS-渲染层前后端分离设计.md` | → `docs/` | git mv，更新两处对已删快照文件的引用 |
| `CatOS-进度总结与待办-2026-11-02.md` | → `docs/` | git mv |
| `CatOS_用户反馈分析.xlsx` | → `docs/` | git mv（需求输入，非代码垃圾） |
| 旧根 `README.md` | → `docs/CatOS-v0.3-图形模式成就包.md` | 它是 **v0.3 成就包说明**（Mode 13h + QEMU VGA 字形 bug 根因分析），**不是项目 README**；内容作为历史记录保留 |
| **新**根 `README.md` | 新建 | 归档后仓库顶层无 README 不合适。新 README 描述目录结构 / 构建 / 运行 / 测试 / 文档索引 |

### 2.3 根目录最终形态

```
.gitignore   README.md   build.sh   docs/   kernel_v25/   tests/
```
（`git ls-files | grep -v /` 只返回 `.gitignore` / `README.md` / `build.sh` 三项；
`.workbuddy/` 已被 `.gitignore:64` 忽略。）

---

## 三、E2 资产归档

### 3.1 归档清单与判定

完整索引见 **`docs/orig-archive/README.md`**（5 份逐份判定 + 依据 + 关键文件位置）。
摘要：

| 归档后路径 | 原路径 | 是否仍然成立 |
|---|---|---|
| `docs/GO-DESIGN.md` | `doc/GO-DESIGN.md` | **部分成立** —— 设计/子集规格/自举定义有效；§2「48 槽」过期（现 57 槽，但 go.c 只用 ≤39 槽 ⇒ 无需改 ABI）；§6「未修 FIND-G3~G10」过期 |
| `docs/go-review.md` | `doc/go-review.md` | **方法与 FIND 清单有效，缺陷状态过期** |
| `docs/orig-archive/legacy-TODO_STAGE5.md` | `doc/TODO_STAGE5.md` | **部分成立** —— 自举固定点、128 KiB 栈仍未做 |
| `docs/orig-archive/legacy-BUILD_REPORT_STAGE5.md` | `doc/BUILD_REPORT_STAGE5.md` | **部分成立** —— 工具链根因已解决；3 项待验证仍在 |
| `docs/orig-archive/legacy-HANDOFF_STAGE5.md` | `doc/HANDOFF_STAGE5.md` | **部分成立** —— 128 KiB 栈改动仍在；**forward-call-fix 是否并入现项目待架构师核对** |

未归档的 9 份（原项目 `doc/` 6 份 + 根 3 份）逐条给了理由，见 `docs/orig-archive/README.md` 第一节。

### 3.2 原项目 `tools/` 三脚本：全部实际试跑，全部不移植

| 脚本 | 试跑结果 | 判定 |
|---|---|---|
| `cc_stage45_test.py` | `subprocess.run(['pkill','-9','-x','qemu-system-i38'])` → `FileNotFoundError`（Linux-only）+ 硬编码 `/home/user/Doubao/chats/38448438262014978/catos-qemu` | 不移植（功能被 `tests/qemu_regress.py` 覆盖） |
| `diag2_test.py` | 同上 | 不移植（同上） |
| `inject_any.py` | 与现项目 `tests/inject_any.py` **逐字节相同**；几何参数 `RSVD=33`/`SPC=8`/`FAT_SZ=8192` vs 内核 `src/fat.c:8` `RESERVED 32u` ⇒ 在现项目上必然 `BIN dir not found` | 不移植原文件，**改为修现项目副本**（落档 `#64`） |

`tools/TNC2.C`（1 行，DREL 数据区重定位回归用例）归入移植清单（PRD P1-6），本阶段不重复搬运。

### 3.3 D-1~D-15 落档

`docs/CatOS-内核问题清单.md` 表格行数 **64 → 82（+18）**：

| 位置 | 编号 | 内容 | PRD 关联 |
|---|---|---|---|
| 第一节 | `#48` | `heap_ensure_range()` 缺失 ⇒ 跨页分配 `#PF` | P0-2 |
| 第一节 | `#49` | `USER_STACK_SZ` 64 KiB（应 128 KiB） | P0-3 |
| 第一节 | `#50` | `USER_HEAP_MAX` 256 KiB（应 512 KiB） | P0-3 |
| 第四节 | `#51` | `cc.c` 停在阶段 0，`cc_self.c` 是死代码 | P1-3 |
| 第四节 | `#52` | `CC2.CE` 自举固定点未验证 | P2-1 |
| 第四节 | `#53`~`#60` | Go 解释器 FIND-G3~G10（8 条各占一行） | P1-2 |
| 第四节 | `#61` | `catos_work.iso` 从未入库 | P0-4 |
| 第四节 | `#62` | `build.sh` 应用列表不含 `cc` | P1-4 |
| 第四节 | `#63` | `node.c` 从不被编译 | P1-9/P1-10 |
| 第四节 | `#64` | `tests/inject_any.py` FAT 几何过时 | P1-7 |
| 第四节 | `#65` | `.gitignore` 否定规则位置写反 | P0-4 |

D-4 / D-5 / D-9 / D-10 按 PRD 判定**不新增**（现项目已分别记录于 `#17`、第十节 `#47`、第十节节末表）。

**编号冲突消解**：第九节 `#48`/`F1`/`A2`/`#3` 与第十节 `#47` 本就是**各自独立的局部命名空间**
（第九节是 b31 修复闭合表、第十节是工具链常量核对表，均已在节首自述）。
本次在**第一节**与**第九节**各加了一段命名空间说明，明确「引用时写『第四节 #61』」。

---

## 四、E3 构建可复现性

### 4.1 方案选择

| 路线 | 结论 |
|---|---|
| (b) 现场用 `grub-mkrescue` 生成模板 | ❌ **否决**。`grub-mkrescue` / `grub2-mkrescue` / `grub-mkimage` / `xorriso` / `mformat` / `mcopy` / `genisoimage` / `mkisofs` **全部 NOT FOUND**（无 MSYS2、无 `C:\grub*`、无 `D:\grub*`、无 choco shim）。写不出能在本机真跑起来的 bootstrap 脚本 |
| (a) 模板入库 | ✅ **采用**。一次性 10.7 MB，换任何人 clone 下来都能构建 |

### 4.2 根因：不是「忘了 git add」，是 `.gitignore` 否定规则写反

git 解析 `.gitignore` 是 **last-match-wins**。原文件第 7 行与第 12 行都有
`!kernel_v25/build/catos_work.iso`，而第 27 行是 `*.iso` —— 后者把前者**覆盖**了。
原注释写的 *"exceptions must precede rules"* **结论是错的**。

```
修复前：git check-ignore -v kernel_v25/build/catos_work.iso
        → .gitignore:27:*.iso          ← 命中的是【忽略规则】，否定规则形同虚设
修复后：git check-ignore -v --no-index kernel_v25/build/catos_work.iso
        → .gitignore:35:!kernel_v25/build/catos_work.iso   ← 命中的是【否定规则】
```

已把两条否定规则移到 `*.iso` / `*.py` **之后**并重写注释（明确写出 last-match-wins 语义）。
**只 `git add -f` 而不移位是假修复** —— 下次有人删了重建该文件会再次被 `*.iso` 忽略。

### 4.3 验收：干净克隆实测

`git clone` 本分支到 `/tmp/catos_clone_test` 后：

```
git ls-files kernel_v25/build/ → kernel_v25/build/catos_work.iso
                                  kernel_v25/build/gen_builtin.py
                                  kernel_v25/build/mkce.py
                                  kernel_v25/build/patch_work_iso.py
build.sh iso → OK -> build/kernel.bin
              OK -> ...\kernel_v25\catos.iso (16558080 bytes)
tests/test_mkce.py   → 48 passed, 0 failed
tests/ce_regress.py  → PASS=5 SKIP=13 FAIL=0
tests/qemu_regress.py→ steps=12 ok=12 warn=0
```

**意义**：在 E3 之前，"基线 12/12 PASS" 依赖 team-lead 手工拷进工作树的**未跟踪**模板，
**不具备 CI 意义**；E3 之后基线可由任何第三方在干净克隆上复现。

---

## 五、E4 node/go 移植可行性 —— 结论与独立复核

> **任务取消**：PM 已在 PRD §2.3 / §2.5 完成 spike 并**亲自在 QEMU 实机跑通**，
> 本节只做**工程师侧独立复核**并补充一条 PRD 未记录的重要发现。

### 5.1 结论（采信 PRD）

| 资产 | 结论 | 依据 |
|---|---|---|
| `go.c` | **可移植，ABI 零改动** | 只用 7 个槽（`write`0/`putc`1/`alloc`11/`print_int`14/`atoi`18/`file_exists`23/`file_read`39），**全部 ≤39 < 48**，落在 57 槽 append-only 旧前缀内。PM 实测：57 槽头编译零错、CE01 头合法、QEMU 输出 `fib(10)= 55` / `demo done` / `go: exit ok` |
| `node.c` | **不需要移植 —— 已经在库里** | `kernel_v25/node/node.c`（1,414 行）与原项目 `node/node.c` diff **0 行差异**；`node.ce` **逐字节相同**（md5 `2101a0c8afe0243d192fd86f27350a94`）；`TEST.JS` 仅 CRLF 差异。**真实缺口是它从不被任何构建脚本编译**（落档 `#63`） |
| 原项目 `src/` | **不是"另一套实现"，是现项目的祖先** | 19 个文件逐一对过，10 个零行差异、7 个现项目更新。"整树合并 src/"是伪命题 —— 精华在 `go.c` + 文档 + 构建链缺口 + 两个被 VMA 重构弄丢的内核修复 |

### 5.2 独立复核发现（PRD 未记录）

**PRD §2.7 与 `go-review.md` 都把 FIND-G3~G10 记为「未修」，但原项目 `extapp/go.c` 源码里这 8 条其实已经全部修好。**

| FIND | 修复证据（`extapp/go.c`，1,300 行 / md5 `f9876f42fa646c58ec21da649ed934e4`） |
|---|---|
| G3 递归深度 | `:783` `g_call_depth` + `:784` `CALL_DEPTH_MAX 200` + `:1048` 入口检查 + `:1053/:1058/:1074` 增减配平 |
| G4 非法字符静默 | `:358-362` default 分支置 `g_perr`/`g_errline` |
| G5 readfile 16 KiB 上限 | `:1085` 新增 `read_whole()`，按 `FS_CHUNK 16384` 分块循环读至 EOF |
| G6 arena 未判空 | `:701` 与 `:732` `if (!s->v) { g_oom = 1; return 0; }` |
| G7 字面量 1023 截断 | `:223-235` 两遍扫描（先数解码长度再按 arena 尺寸分配）；未闭合引号报词法错误 |
| G8 顶层 break/continue | `:188-191` `g_loop_depth`/`g_func_depth` + `:658/:665/:669` 三处 `depth==0` 报错 |
| G9 for `:=` 作用域泄漏 | `:882-899` 每个 for 建独立 `Scope`，结束恢复 `g_scope` |
| G10 数字溢出 | `:302-313` 扫描时检测 `> 2147483647` |

**含义**：用户拍板的「8 个 P2 全部修」，**在源码层面原项目已经完成**。
移植时要做的是**逐条验证修复随行**，而不是重新实现 —— 这直接改变了 P1-2 的工作量估算。

### 5.3 对现项目 `cc.c` 的边际价值（诚实评估）

**结论：把 `node.c` / `go.c` 拉回来的边际价值，不在于「多一个程序」，而在于「多一条独立于内核的验证路径」与「补上现项目缺失的一整条工具链能力」。**

| 维度 | 评估 |
|---|---|
| **对 `cc.c` 自举路线的直接价值** | **低**。`node.c` 是 JS 解释器、`go.c` 是 Go 解释器，都不是 C 编译器，与 `cc.c` 的自举**无直接依赖**。`cc.c` 的能力缺口（`struct`/`typedef`/`enum`/`switch`/`goto`）只能由 `cc_self.c`（现项目内，2,250 行，已实现）补齐 —— 这才是 `cc.c` 的正确解法 |
| **对「编译能力已废」问题的价值** | **间接但真实**。`cc.c` 编译能力实际已废（P0-1：`api->alloc()` 恒返回 NULL ⇒ 只能编译 <8 KiB 源码）。`go.c` 的 192 KiB arena 与 `cc_self.c` 的编译需求**都依赖同一个堆修复** ⇒ 它们是**同一个 P0 的多个受害者**，不是解决方案 |
| **对内核回归测试的价值** | **中**。`go.c`/`node.c` 是**用户态真实负载**：能跑它们 ⇒ 内核的 alloc/free/fs/console 路径被真实压测；跑不了 ⇒ 可能只是 alloc 坏了。PRD V-5/V-6 的 allocprobe/heaptest 探针是**更直接**的验证手段，但 `go.ce` 是**端到端**的验证 |
| **对文档与知识资产的价值** | **高**。`GO-DESIGN.md` + `go-review.md` 是**唯一**记录「为什么这样设计」的材料；`docs/` 里现项目对 `go.c` 的提及次数原本是 **0** ⇒ 这是一条被现项目主线完全遗忘的开发线 |
| **对 CE ABI 的验证价值** | **中**。`go.c`/`node.c` 都只用 ≤39 槽，是「append-only 策略确实保护了旧程序」的一个**活证据**。这比任何文档声明都强 |

**一句话**：边际价值**真实但不在编译能力上**。`go.c` 的价值是「用户态解释器 + 端到端验证负载 + 设计上下文」；
`node.c` 的价值是「补上构建链的一环 + 一条独立验证路径」。
如果只从「让 `cc.c` 能自举」这个目标看，**拉回 node/go 的价值接近于零** —— 那件事只需要 `cc_self.c` + 堆修复。

---

## 六、基线回归结果（真实输出）

| # | 命令 | 期望 | 实测 |
|---|---|---|---|
| A-1 | `python tests/test_mkce.py` | 48 passed / 0 failed | ✅ **48 passed, 0 failed** |
| A-2 | `python tests/ce_regress.py` | PASS=5 SKIP=13 FAIL=0 | ✅ **PASS=5 SKIP=13 FAIL=0** |
| A-3 | `python tests/test_cc.py` | PRD 写 8/8（该行标「待复测」） | ✅ **10 passed, 0 failed** —— 实测是 **10/10**，比 PRD 预期多 2 项（PRD 该行本就未复测，是沿用旧计数） |
| A-4 | `python tests/qemu_regress.py kernel_v25/catos.iso --qemu "C:/qemu-portable/qemu-system-i386.exe"` | steps=12 ok=12 warn=0 | ✅ **steps=12 ok=12 warn=0** |
| — | `kernel_v25/build.sh iso` | 产出 ISO | ✅ **OK -> catos.iso (16558080 bytes)** |

> **A-4 的 QEMU 路径更正**：任务书给的 `E:\Tools\qemu\qemu-system-i386.exe` 在本机**不存在**
> （`E:\Tools\qemu` 下只有 `qemu-edid/ga/nbd/storage-daemon` + `lib/`）；
> 实际可用路径是 **`C:\qemu-portable\qemu-system-i386.exe`**（QEMU 11.1.0）。
>
> **A-4 逐步输出（主工作树，2026-11-09）**：
> `boot-shell` / `format` / `help` / `display` / `gfx` / `text-reject-N` /
> `text-accept-Y` / `ls-bin` / `mmaptest` / `hello-elf` / `vdiag` /
> `final-frame` —— 12 步全 PASS，`warn=0`。
>
> **干净克隆复测**（`/tmp/catos_clone_test`，E3 后新增的一次验证）：
> `48 passed, 0 failed` / `PASS=5 SKIP=13 FAIL=0` / `steps=12 ok=12 warn=0`
> —— 与主工作树一致，证明基线不依赖任何本地状态。

---

## 七、被推翻的既有结论（3 条）

| # | 原结论 | 出处 | 实际情况 |
|---|---|---|---|
| 1 | 「`node.c` 是待拉回的候选资产」 | 任务书 E4 | **不成立** —— `node.c`/`node.ce`/`TEST.JS` **早已在库**且与原项目等价（`node.ce` 逐字节相同）。真实缺口是**它从不被编译**（`#63`），不是「缺文件」 |
| 2 | 「FIND-G3~G10 全部未修，需实现方逐条修」 | PRD §2.7 / `go-review.md` | **已过期** —— 原项目 `go.c` 源码里这 8 条**已经修好**（§5.2 逐条证据）。工作内容从「实现」变成「验证随行」 |
| 3 | 「`build.sh`（根）是残桩，应删除」 | 任务书 E1 | **不成立** —— 它是**可用**的委托 shim（实测能出 iso），且被 `docs/CatOS-渲染层前后端分离设计.md:82` 明确记载为设计的一部分。已保留并只更新注释 |

> 另有一条**元结论**：E3 的根因不是「忘了 `git add`」，而是 **`.gitignore` 的否定规则位置写反**
> （原注释 "exceptions must precede rules" 方向说反了）。只 `git add -f` 不移位是**假修复**。

---

## 八、提交与交付

| commit | 内容 |
|---|---|
| `66bbfce` | `curation: purge snapshot junk from repo root` —— E1 |
| `101a326` | `harvest: archive legacy docs + record D-1..D-15 in issue tracker` —— E2 |
| `f39bb0b` | `build: make ISO build reproducible from a clean clone` —— E3 |

**未 push**（按任务书要求）。他人 WIP（`kernel_v25/extapp/bin/*.ce` 13 个 + `kernel_v25/src/builtin_apps.inc`
的未暂存改动，源于本地 gcc Rev5 重编、committed 版内嵌 "Rev4"）**未纳入任何 commit**。

### 本阶段**未做**的事（诚实清单）

| 未做项 | 原因 |
|---|---|
| E4 的 go.c / node.c **实际移植** | 任务取消（PM 已完成 spike）；且 `gen_builtin.py` 的 `FILES` 列表、`extapp/go.c`、`extapp/cc.c` 明确划归架构师与后续实施阶段，不在本阶段范围 |
| P0-1 / P0-2 / P0-3 的**代码修复** | 属架构师设计与后续实施阶段；本阶段只负责**记录**（`#48`~`#50`），未动 `task.c` / `paging.h` |
| 原项目 `build/` 下 7 个宿主工具链资产（`host_stub_cc.c`/`host_stub_go.c`/`verify_bootstrap.sh`/`mk_cc_self.py`/`cc_qemu_test.py`/`go_qemu_test.py`/`cc_stage_test.py`）的归档 | PRD 把它们归为 P1-8 / P2-2 实施项；本阶段仅在归档索引中记录其存在与价值 |
| 原项目 `extapp/cases/` 11 个回归用例的移植 | PRD P1-5，属实施阶段 |
| 根 `README.md` 是否保留的最终裁决 | 已按「归档旧 README + 新建项目 README」处理，若 team-lead 认为顶层不该有 README，可撤掉新建的那个文件 |

---

## 九、下一步交接

| 责任方 | 事项 | 依据 |
|---|---|---|
| **架构师** | `docs/merge-harvest-design.md`：P0-1（`heap_vma_init` 用 `vma_insert` 而非赋值覆盖）、P0-2（`heap_ensure_range` 前置声明位置）、P0-3（栈/堆常量） | PRD §2.4、本文档 §三 |
| **架构师（核对）** | forward-call-fix（原项目 `extapp/cc.c:1832` 的原型符号复用逻辑）是否已并入现项目 `cc_self.c` | `docs/orig-archive/README.md` A-5 判定表 |
| **实施阶段** | P1-1（`go.c` 进内核 + 移植 5 个 `.GO` 用例，**逐条验证 FIND-G3~G10 修复随行**）、P1-3（`cc_self.c` 覆盖 `cc.c`）、P1-4/P1-9（补构建链）、P1-7（修 `inject_any.py`） | PRD §3、本文档 §五 |
| **工程师（后续）** | `docs/merge-harvest-design.md` 落地后，实施阶段收尾时的回归验证 | — |

---

## 十、第二阶段实施结论（内核层 T02–T05 + 应用层 P1）

> **本节记录**：内核层任务单 T02/T03/T04/T05 的执行结果、应用层 P1 各项的落点，
> 以及本次移植过程中**暴露并修复的 3 个内核/测试层缺陷**。
> 每一项都给出「缺陷 → 源码位置 → 移植后是否仍存在 → 验证方式」。

### 10.1 内核层任务单落点

| 任务 | 内容 | 落点 | 验收（真实输出） |
|---|---|---|---|
| **T02** | P0-1 + P0-2：`heap_vma_init()` 改用 `vma_insert()`；新增 `heap_ensure_range()`；`sys_alloc()` 对 payload 全范围调用 | `kernel_v25/src/task.c:734`（`heap_ensure_range`）、`:781`（`t->vm = vma_insert(t->vm, v)`） | `run /bin/allocprobe.ce` → `ALLOCPROBE: alloc works`；`run /bin/heaptest.ce` → `HEAPTEST PASS`；`qemu_regress.py` 仍 12/12 |
| **T03** | P0-3 常量恢复：`USER_STACK_SZ` 0x10000→**0x20000**、`USER_HEAP_MAX` 0x40000→**0x80000** | `kernel_v25/include/paging.h` | `qemu_regress.py` 仍 12/12；堆探针全绿 |
| **T04** | `cc.c` ← `cc_self.c` 接管（阶段 1-3 编译器）；`-DCC_HOST` 前置；token heap 预算重整 | `kernel_v25/extapp/cc.c`（2,317 行）、`kernel_v25/extapp/build_ce.sh` 的 `CFLAGS` | `tests/test_cc.py` **10/10**（含新增 `struct`/`switch`/`goto` 用例）；`cc.ce` = 48,104 B ≪ 256 KiB |
| **T05** | 构建链两处列表一次改完：`build.sh` 应用列表 + `gen_builtin.py` 的 `FILES`/`OPTIONAL` 同时加入 `go`/`node`/`cc`/`allocprobe`/`heaptest` | `kernel_v25/build.sh`、`kernel_v25/build/gen_builtin.py` | **同一个 commit**（`00b54d8`）—— 两处列表不允许分两次改 |

**T02/T03 顺带修掉的两个隐患**（架构师 §8 指出的）：

| # | 缺陷 | 位置 | 修法 | 后果 |
|---|---|---|---|---|
| **#66** | `sys_alloc()` 整块消费分支缺 `h->size = size;` | `task.c` `sys_alloc()` 的 `else { *pp = h->next; }` | 补 `h->size = size;` | freelist 回收后 size 语义漂移 ⇒ 下一轮分配「以为」自己拥有比用户拿到的更多字节 ⇒ **潜在越界写** |
| **#67** | `cc_alloc()` 失败路径不设 `*cap` | `kernel_v25/extapp/cc.c` `cc_alloc()` | 失败路径补 `*cap = fallback_cap;`（8 个调用点全部补参数） | `g_tok_cap = 0` ⇒ 后续分配直接返回 NULL 而非降级到静态缓冲 |

### 10.2 token heap 预算重整（架构师算式落地）

**问题**：架构师算出 `cc_self.c` 的 **8 路堆工作集 = 1,004,992 B**，而用户拍板的 512 KiB 堆
payload 只有 **524,280 B**（`USER_HEAP_MAX 0x80000` 减去头部/对齐）。`TOK_MAX * sizeof(token_t)`
= 25,000 × 24 = **600,000 B 单笔就放不下** ⇒ `cc_alloc()` 回落 512-token 静态缓冲 ⇒
编译器对**任何源码立即报 "source too complex"**（比 PRD 说的「只能编 <8 KiB 源码」更严重）。

**处置**（按架构师方案）：

| 项 | 数值 | 说明 |
|---|---|---|
| `TOK_HEAP`（新增） | **4800** | 512K/8 路全过的精确上限是 4,968，取 4,800 留余量 |
| `sizeof(token_t)` | **24 B** | `kernel_v25/extapp/cc.c` |
| 单笔 token 缓冲 | 4,800 × 24 = **115,200 B** | |
| 8 路工作集合计 | **520,192 B** | 131072+98304+115200+65536+65536+28160+8192+8192 |
| 对 524,280 B payload | **≤**，slack **4,088 B** | 全部 8 路同时活跃也放得下 |
| `TOK_MAX`（保留） | **25,000** | 编译期**源码语言上限**（token 数组的静态维度），与堆预算解耦 |

**取舍必须写明（用户级决策）**：现役编译器能力 = 阶段 1-3 的 `struct`/`typedef`/`enum`/`switch`/`goto`
回来了，但 **token 容量受限于用户拍板的 512 KiB 堆**（原项目是 20,000 token / 需约 1 MiB 堆）。
若将来要恢复 20,000 token，需把 `USER_HEAP_MAX` 提到 1 MiB —— **这是用户级决策，已向上汇报，
工程师不自行改堆上限**。

**验收（实的）**：用 `cc.ce` 编译**超过 8 KiB 的源码**，确认能出 `.ce` 且**不是** "source too complex"。
`tests/test_cc.py` 的 10 个用例全部编过，其中 `struct`/`switch`/`goto` 三例合计源码 >8 KiB。

### 10.3 P0-6：`u2k_str()` 越界校验 —— **原项目固有的内核缺陷，移植时暴露并修复**

> **这是本次合并的正收益**：不是移植引入的缺陷，而是移植过程**暴露**了它并**顺手修好**。

**现象**

`run /bin/go.ce /BIN/TEST.GO` 在实机上回答 `go: cannot read script: /BIN/TEST.GO`，
但 `/BIN/TEST.GO` **确实在 FAT 卷上**（用独立写的宿主侧 FAT32 解析器逐簇核对过内容正确）。

**决定性对照实验**（`kernel_v25/extapp/readtest.c` 探针，同一个路径、两种传参方式）：

```
LITERAL path=/BIN/DEMO.GO exists=1   file_read=592    <- 字面量（在 image VMA 内）成功
ARGV    path=/BIN/TEST.GO exists=0   file_read=-3     <- argv（在用户栈顶）失败
```

⇒ **同一个路径，字面量可用、argv 传参不可用** ⇒ 缺陷与 Go 解释器无关、与文件是否在盘上无关，
**在内核的用户字符串拷贝路径上**。

**根因**（`kernel_v25/src/task.c:668`，修前）

```c
static int u2k_str(char *dst, const char *us, int max) {
    task_t *t = cur_task;
    if (validate_uaddr(t, (uint32_t)us, max) != 0) return -1;   /* ← 缺陷 1 */
    for (int i = 0; i < max - 1; i++) {
        dst[i] = us[i];                                          /* ← 缺陷 2 */
        if (!us[i]) return 0;
    }
    ...
}
```

1. **窗口校验越界**：`max = FS_PATH_MAX = 160`，`validate_uaddr()` 会检查
   `[us, us+max-1]` **整段**是否落在同一个 VMA 内。而 `task_spawn_ce()` 把 argv 字符串
   **放在用户栈顶**（`sp = USER_STACK_TOP - 0x10`，逐个 `sp -= l + 1`）⇒ 160 字节窗口
   **必然越过 `USER_STACK_TOP`** ⇒ `vma_contains(t->vm, end)` 失败 ⇒ 返回 -14 ⇒
   `u2k_str` 返 -1 ⇒ `SYS_FILE_READ` 返 **-3**、`SYS_FILE_EXISTS` 返 **0**（"不存在"）。
   算术核验：`argv[0]` 落在 0x021FFFE5，窗口末 0x02200085 > 栈顶 0x02200000。
2. **ring0 直接解引用用户指针**：`dst[i] = us[i]` 用的是**内核地址空间**。
   CE image 恰好被恒等映射所以能 work，用户栈**没有**恒等映射 ⇒ 这条路径本身就是坏的，
   只是被第 1 点掩盖了。

**归属判断：原项目固有缺陷，不是移植引入**

原项目（只读复核 `D:/CatOs/Catos_Better_better/src/task.c:618`）用的是**另一种**校验：

```c
static int u2k_str(char *dst, const char *us, int max) {
    if ((uint32_t)us < USER_IMAGE_BASE || (uint32_t)us >= USER_ARENA_END) return -1;
    for (int i = 0; i < max - 1; i++) { dst[i] = us[i]; if (!us[i]) return 0; ... }
    ...
}
```

—— 是**单点 arena 边界检查**（`USER_IMAGE_BASE ≤ us < USER_ARENA_END`，栈在 arena 内所以放行），
**没有 160 字节窗口**，因此原项目**不会**触发本缺陷。原项目当年验证 go 时用的是**无参的内置 demo 回退**
（`go.c:1215`），从未走过 argv 传参路径 ⇒ 缺陷一直潜伏。

**影响面（同一处修复覆盖 7 条 syscall 路径）**：`SYS_FILE_READ` / `SYS_FILE_EXISTS` /
`SYS_FILE_WRITE` / `SYS_SAVE` / `SYS_SPAWN` 的 argv 拷贝 / 网络调用的 `url`/`host`/`ip`
三张参数表 —— **全部是同一个 bug 的受害者**。

**修法**（commit `38fee6a`）：逐字节校验 + 经任务页表翻译读取，不再从 ring0 直接解引用：

```c
for (int i = 0; i < max - 1; i++) {
    uint32_t a = u + i;
    if (validate_uaddr(t, a, 1) != 0) { dst[i] = 0; return -1; }
    uint32_t pa = task_virt_to_phys(&t->space, a & ~0xFFFu);
    if (!pa) { dst[i] = 0; return -1; }
    char ch = *(char *)(pa + (a & 0xFFF));
    dst[i] = ch;
    if (!ch) return 0;
}
```

（内核把低 64 MiB 恒等映射，`pa + offset` 是合法的内核别名 —— 与既有的 `k2u_copy()` 同一模式。）

**验证方式**：见 §10.5 的 `run /bin/go.ce /BIN/TEST.GO` → `ALL PASS`，
以及 `run /bin/node.ce /BIN/TEST.JS` → `ALL PASS`（**两条都依赖 argv 传参**，
修前必然失败，修后通过 —— 这就是该缺陷的直接验收信号）。

**同时落档**：作为内核老问题续编号写入 `docs/CatOS-内核问题清单.md` **#68**。

### 10.4 FIND-G3~G10 逐条随行验证

> 任务书要求：「移植后 8 条修复必须**逐条随行且有效**」，每条写一行
> 「缺陷 → 源码位置 → 移植后是否仍存在 → 验证方式」，并**至少对其中 2-3 条做实机验证**。

| FIND | 缺陷 | 源码位置（`kernel_v25/extapp/go.c`） | 移植后是否仍存在 | 验证方式 |
|---|---|---|---|---|
| **G3** | 递归深度无上限 → 栈溢出 | `:783` `g_call_depth` + `:784` `CALL_DEPTH_MAX 200` + `:1048` 入口检查 + `:1053/:1058/:1074` 增减配平 | **不存在**（修复随行） | **实机**：`tests/go_regress.py` 的 `go-test` 跑 `TEST.GO`（含深递归用例）→ `ALL PASS`。**宿主桩**：`host_stub_go.c` 编译 `go.c` 后跑 `TEST.GO` 全绿 |
| **G4** | 非法字符被静默吞掉 | `:358-362` default 分支置 `g_perr`/`g_errline` | **不存在** | **实机**：`run /bin/go.ce /BIN/SYN.GO` → 报 `syntax error at line 3`（不是静默通过） |
| **G5** | 读文件 16 KiB 上限 | `:1085` `read_whole()`，`FS_CHUNK 16384` 分块循环读至 EOF | **不存在** | **静态核对**：`read_whole()` 循环 + `go_regress.py` 的 `go-test` 覆盖多段读取路径 |
| **G6** | arena 分配未判空 | `:701` 与 `:732` `if (!s->v) { g_oom = 1; return 0; }` | **不存在** | **宿主桩**（注入 OOM）：分配失败时置 `g_oom` 并返回 0，解释器不崩、报 OOM 而非野指针 |
| **G7** | 字面量 1023 截断 | `:223-235` 两遍扫描（先数解码长度再按 arena 尺寸分配）；未闭合引号报词法错误 | **不存在** | **静态核对**：两遍扫描代码路径存在；`go-test` 用例含字符串字面量 |
| **G8** | 顶层 break/continue 未报错 | `:188-191` `g_loop_depth`/`g_func_depth` + `:658/:665/:669` 三处 `depth==0` 报错 | **不存在** | **宿主桩**（注入用例）：顶层 `break` 被拒绝并报错，不越出函数边界 |
| **G9** | for `:=` 作用域泄漏 | `:882-899` 每个 for 建独立 `Scope`，结束恢复 `g_scope` | **不存在** | **静态核对**：`Scope` 创建/恢复代码路径存在 |
| **G10** | 数字溢出 | `:302-313` 扫描时检测 `> 2147483647` | **不存在** | **宿主桩**（注入用例）：溢出数字被拒绝并报错 |

**额外发现并修复的移植侧缺陷（不在 G3~G10 之内）**：`go.c` 的 `a_alloc()` 边界判断写死
`ARENA_SZ`（192 KiB），但静态兜底 arena 只有 64 KiB ⇒ 越界隐患。**已修**并实测各用例 arena 峰值。

### 10.5 `ce_regress` 的 `go.ce` FAIL —— 判定为**模拟器建模缺陷**，非移植引入

**现象**：`tests/ce_regress.py` 从 `FAIL=0` 变成 `FAIL=1`，FAIL 项是 `go.ce`，
报 `[ce_sim] ERROR: step limit 3000000 hit at pc=0x02002d05`。

**对照实验（主理人执行）**：用**原项目未经改动的 `go.c`**（md5 `f9876f42fa646c58ec21da649ed934e4`）
走**完全相同**的构建路径（`build_ce.sh` → image=26812 / bss=0x292c0 / api_off=0x68b8）与
同一个 `ce_sim`（6 M 步预算）⇒ 原版 `go_orig.ce` 报 `pc=0x02002f25`，
移植版报 `pc=0x02002f23`，**输出停点完全一致**（`CatOS go subset v1.0 / ---- / script: built-in demo`）。
20 M 步仍然撞上限。⇒ **移植没有引入这个失败**。

**根因（本次确证）**：`tests/ce_sim.py` **漏维护 CF 标志**。
模拟器为 `0x38`/`0x39`/`0x3A`/`0x3D` 四种 `cmp` 形式都正确设置了 `self.cf`，
**唯独 `0x3C`（`cmp al, imm8`）从不设置**。而 gcc -O2 把 `is_idc()` 内联成：

```
and $0xffffffdf,%eax / sub $0x41,%eax / cmp $0x19,%al (0x3C) / jbe (0x76)
```

`jbe` 读的是 **CF|ZF**，CF 残留自上一条指令 ⇒ 模拟器在词法器里走错分支。

**证据链**：
1. PC 直方图：3,000,000 步内 57,883 次重复同一段循环（151 个不同 PC），输出恒停在 banner 之后；
2. 读解释器全局：`g_pos = 24`（冻结）、`g_tokn = 4096`（= `TOK_MAX`）、`g_line = 2`、
   `src[24:] = b'func fib(n int) int {\n    if n < 2 { ret'` ⇒ 词法器停在第 24 字节；
3. 反汇编热点 PC：`0x02002f21: 3c 19`（`cmp $0x19,%al`）后紧跟 `0x02002f23: 76 6b`（`jbe`）；
4. `ce_sim.py` 的 `0x3C` 分支确实缺 `self.cf = ...`。

**处置（走任务书的路线 (a)「判定慢还是死循环」，而不是 (b) 放宽判据）**：
修 `ce_sim.py` 的 CF 闩锁（commit `7f8b225`），**未删任何测试、未放宽任何判据**。
修后 `go.ce` 在 `ce_sim` 下 **COMPLETED，246,041 步，`g_pos=563/563`，`g_tokn=200`**
（源码读完、200 个 token）⇒ **证实 Go 解释器本身没有死循环**。

同时补齐了 group3（`0xF6`/`0xF7`）的**内存操作数**形式 —— gcc 为 32 位有符号除法发的是
`idiv dword ptr [ebp-0x18c]`，只有寄存器形式的模拟器会直接 abort 而不是执行它。

**归属**：`ce_sim.py` 的建模缺陷（测试层），**不是** `go.c` 的 CE 侧缺陷、**不是**移植引入。
**对照证据**：同一份 `go.ce` 在**真实 QEMU** 上完整跑出 `fib(10)= 55` / `demo done` / `go: exit ok`。

### 10.6 ISO 内置 vs 磁盘 FAT 卷（V-8 到底验的是哪一份）

**结论：两者内容同源、载体不同；V-8 验的是「磁盘 FAT 卷上的 `/bin/go.ce`」。**

| 载体 | 生成方式 | 谁读它 |
|---|---|---|
| **ISO 内置表** | `kernel_v25/src/builtin_apps.inc`（由 `build/gen_builtin.py` 生成，`FILES` 必需 + `OPTIONAL` 缺失则跳过） | `ce_install_builtins()` 在 `format` 之后与开机时**拷贝**到 FAT 卷 |
| **磁盘 FAT 卷** | `ce_install_builtins()` 写入 IDE 磁盘的 FAT32 卷 | 内核 shell 的 `run /bin/x.ce` —— **shell 从 FAT 卷读，不是从 ISO 内置表读** |

⇒ `run /bin/go.ce` 实机跑的是**磁盘上那一份**。它由 ISO 内置表拷贝而来，所以内容同源，
但**载体不同**（`run` 命令读的是 FAT32 文件）。**注意**：`ce_install_builtins()` 里
`fs_write_raw` 失败即 `return` ⇒ 后续文件不再安装 —— 这是 FAT 卷损坏时 `/bin` 缺文件的机制。

**FAT 8.3 名截断**（影响所有 `ls /bin` 断言）：`fat_name83()` 取**最后一个**点号分扩展名、
base 截 8、ext 截 3、**全部大写**；`fat_name_show()` 输出时转小写 ⇒ `allocprobe.ce` 显示为
`allocpro.ce`、`readtest.ce` 显示为 `readtes~1.ce`。断言必须用 8.3 名。

### 10.7 argv 通道：ABI 零改动（任务书要求核实的三条之一）

| 核实项 | 结论 | 证据 |
|---|---|---|
| `ce_load_ce()` 是不是 GRUB 启动主路径？ | **不是**。P0-5 因此**不成立**。 | `grep -rn "ce_load_ce" kernel_v25/src/` 两棵树都 **0 命中**；CE01 magic 校验在 `task.c:1677`，对象是 FAT 读上来的用户态 `.ce`，与 GRUB 加载的 `kernel.bin` 无关。`ce_load_ce` 唯一的历史调用点是 `load_ce.bin`（一条**不走 GRUB** 的路径） |
| go.c 无参数时脚本路径怎么传？ | **不传** —— 走**内置 demo 回退**：`go.c` 的 `ce_main()` 检查 `argc >= 2 && argv[1][0]`，否则用 `file_exists("/BIN/DEMO.GO")` 或内嵌 demo 源码 | `kernel_v25/extapp/go.c` 的 `ce_main()` 路径选择分支 |
| CE ABI 有没有 argv 槽位？ | **有，且早已有** ⇒ **零 ABI 改动** | `kernel_v25/include/ce.h:20-26` 的 argv 契约 + `task.c:1783-1800` 压栈（`argv_ptr[]` + `argv_arr` → 入口帧 `[esp]=VS_EXIT_STUB, [esp+4]=argc, [esp+8]=argv`）+ `shell.c:576-579` 切分 + `go.c:1199` 已是 `int ce_main(int argc, char **argv)` |

**go.c 只用 ≤39 号 CE 槽**（`write`0/`putc`1/`alloc`11/`print_int`14/`atoi`18/`file_exists`23/`file_read`39），
全部落在 57 槽 append-only 的**旧前缀**内 ⇒ **不需要改 ABI**。

### 10.8 共享机器纪律（本次协作事故的记录）

**事故**：探针脚本用 `taskkill /F /IM qemu-system-i386.exe`（**按镜像名全局杀**）做开机前清理，
在共享机器上把**其他驱动正在跑的 guest 一起杀掉** ⇒ 幸存者的 HMP monitor socket 被复位
（`WinError 10054`）⇒ 表现为「同一个脚本两次跑出完全不同的结果」（4/7 vs 2/7）的随机失败。

**根因**：按镜像名清理在共享机器上是**禁止动作**；正确做法是**按 PID** 精确清理自己启动的进程。

**处置**：
- 新增 `tests/qemu_driver.py` —— 共享的、**PID 安全**的 QEMU 驱动模块，
  统一提供 `kill_qemu(proc)`（按 PID）、`boot()`（返回 `(proc, sock)`）、读屏、`sendkey`、
  以及 `clear_screen()`；
- `tests/heap_regress.py` 与 `tests/qemu_regress.py` 改为按 PID 清理；
- **附带修正两个 shell 事实**：CatOS shell 的清屏命令是 **`clear` 不是 `cls`**
  （`cls` 会打印 `unknown command: cls` 且不清屏 ⇒ 断言会匹配到上一屏的残留文本）；
  CatOS shell **没有 `cat` 命令**（确认文件存在只能用 `ls /bin`）。
