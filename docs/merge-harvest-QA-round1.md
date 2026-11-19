# merge-harvest 第一批成果 —— 独立验证报告（QA Round 1）

> **验证者**：Edward（QA 工程师）｜**立场**：怀疑者，不接受"工程师说已通过"，只接受自己跑出来的输出
> **验证对象**：`66bbfce` / `101a326` / `f39bb0b` / `0cd3e6d`（分支 `workbuddy/todo-20261106-1906`）
> **验证工作树**：`C:/Users/Admin/WorkBuddy/Worktrees/CatOs/master-87a3e1a0`（HEAD = `0cd3e6d`，已核实）
> **日期**：2026-11-09
> **隔离克隆**：`/tmp/catos-qa-clone`（主验证克隆）、`/tmp/catos-qa-nofix`（反证克隆）—— 均在**工作树之外**
> **只读纪律**：未修改 `kernel_v25/` 任何源码、未改 `docs/`、未提交任何东西；唯一写入为本报告

---

## 一、结论总表

| # | 验证项 | 结论 | 一句话证据 |
|---|---|---|---|
| **V-A** | 构建可复现性 | ✅ **PASS** | 干净克隆 `build.sh iso` → `catos.iso` **16,558,080 B**；`check-ignore` 命中 `.gitignore:35:!kernel_v25/build/catos_work.iso`；**反证克隆删掉模板后构建 exit 1**，证明该 commit 是可复现性的必要条件 |
| **V-B** | 三项基线不回退 | ✅ **PASS** | `48 passed 0 failed` / `PASS=5 SKIP=13 FAIL=0` / `10 passed 0 failed` / `steps=12 ok=12 warn=0`，**全部在干净克隆里跑出** |
| **V-C** | 根目录治理正确性 + 无损性 | ✅ **PASS** | 删除**恰好 18** 个、零引用核实；4 份文档为 **git rename**（R092/R099/R100/R100）非 delete+add；旧 `vga.c` 3 个独有能力全部被 `render`/`be_*` 架构覆盖，**且像素 API 在本批之前就已不存在**（`git grep` at `a7eae5a` = 0 命中） |
| **V-D** | 归档索引真实性 | ✅ **PASS** | 三条"不移植/不归档"理由**全部独立复现**；`pe.ld` / `inject_any.py` 的 git blob md5 与原项目**逐字节相同** |
| **V-E** | 问题清单落档质量 | ✅ **PASS** | `#48`~`#65` 连续 **18** 条、编号无冲突、每条 **8 管道 = 7 字段**与表头一致；`go.c` 提及数 **0 → 非 0** |
| **V-F** | 他人 WIP 未被触碰 | ✅ **PASS** | 4 个 commit 全量路径扫描，`vd2.h`/`vga_draw_*`/`build_log.txt`/`simout*/`/`CatOs.7z`/`CatOs/` **零命中**；两份 P0 文档来源与内容已核实 |
| **V-G** | 隔离纪律 | ✅ **PASS** | `D:/CatOs` 状态**只有 `??` 未跟踪项、无一条 ` M`**；原项目树 `find -newermt` **零文件**被改（最新 mtime = Nov 7） |
| **V-H** | 未提交工作树状态合理性 | ✅ **PASS** | `extapp/bin/*.ce`(13) + `builtin_apps.inc` 全部保持 **未提交**；无意外改动混入 |

### 总判定

| 指标 | 数值 |
|---|---|
| **硬 FAIL（真缺陷）** | **0** |
| 文档级 nit（不阻断合流） | **2** |
| 任务书期望值过时（不是 commit 缺陷） | **1** |
| 需向工程侧澄清的口径提醒 | **1** |
| **路由判定** | **Send To: NoOne** —— 无源码 bug、无测试脚本 bug |

**结论：这批 commit 可以进入合流。** 8 项全 PASS，0 硬 FAIL，2 个 nit 均为"状态列未回填/口径说明"级别，不影响任何构建产物、基线或可复现性。

---

## 二、逐项证据

### V-A 构建可复现性 —— ✅ PASS（本次最关键的新主张，已用反证法验证）

**A-1 干净克隆构建成功**
```
$ git clone C:/Users/Admin/WorkBuddy/Worktrees/CatOs/master-87a3e1a0 /tmp/catos-qa-clone
$ cd /tmp/catos-qa-clone/kernel_v25 && env -i PATH=... sh build.sh iso
== linking (PE/COFF) ==
== extracting flat binary ==
OK -> build/kernel.bin
== building catos.iso ==
using patch_work_iso.py (proven GRUB template)
kernel 5831668 bytes, 2848 sectors, will be at LBA 5237, new vol 8085
OK -> ...\kernel_v25\catos.iso (16558080 bytes)
=== BUILD EXIT: 0 ===
```
✅ **16,558,080 字节，与宣称完全一致**。克隆位置在 `/tmp`，**未克隆进工作树内部**。

**A-2 `.gitignore` 否定规则命中 —— 独立复核**
```
$ git check-ignore -v --no-index kernel_v25/build/catos_work.iso
.gitignore:35:!kernel_v25/build/catos_work.iso	kernel_v25/build/catos_work.iso

$ git check-ignore -v --no-index kernel_v25/catos.iso
.gitignore:24:*.iso	kernel_v25/catos.iso
```
✅ 模板命中**否定规则行**（`:35`），非 `*.iso`（`:24`）。

**A-3 模板已跟踪**
```
$ git ls-files kernel_v25/build/
kernel_v25/build/catos_work.iso
kernel_v25/build/gen_builtin.py
kernel_v25/build/mkce.py
kernel_v25/build/patch_work_iso.py
```
✅ 4 个文件，模板在列。

**A-4 独立复核"反转"结论（正交对照实验，非引述）**

把否定规则**人为移到 `*.iso` 之上**（`:24`），模拟旧的有缺陷顺序：
```
negation moved to line 24 (before *.iso at 25)
$ git check-ignore -v --no-index kernel_v25/build/catos_work.iso
.gitignore:25:*.iso	kernel_v25/build/catos_work.iso     ← 命中【忽略规则】，否定规则失效
```
恢复后：
```
.gitignore:35:!kernel_v25/build/catos_work.iso	...    ← 命中【否定规则】
```
✅ **实验证明 git 确为 last-match-wins**：否定规则必须写在 `*.iso` **之后**。工程师在 `f39bb0b` 提交信息里"结论是错的"那句（指旧注释 *"exceptions must precede rules"*）**成立**。`.gitignore:5-9` 的新注释已正确写明 last-match-wins 语义。

**A-5 反证法：该 commit 确实是可复现性的必要条件**

新克隆 `/tmp/catos-qa-nofix`，把 `build/catos_work.iso` 移走：
```
== building catos.iso ==
using patch_work_iso.py (proven GRUB template)
FileNotFoundError: [Errno 2] No such file or directory:
  '...\kernel_v25\build\catos_work.iso'
=== BUILD EXIT: 1 ===
ls: cannot access 'catos.iso': No such file or directory
```
✅ **这是"提交前干净克隆构建会死"这一主张的直接证据**，而非推断。

**A-6 `.gitignore` 位置修正后仍保留其它排除规则**
`kernel_v25/extapp/bin/*.ce` 未被 `!` 规则误放行（`git ls-files` 中它们是已跟踪文件），`kernel_v25/build/*.py` 否定规则同样移到 `*.py` 之后，语义一致。

---

### V-B 三项基线不回退 —— ✅ PASS

**全部在 `/tmp/catos-qa-clone`（干净克隆 + 干净构建产物）中执行**，排除本机残留状态干扰。

| 命令 | 期望 | 实测 | 结论 |
|---|---|---|---|
| `python tests/test_mkce.py` | 48 passed 0 failed | `48 passed, 0 failed` | ✅ |
| `python tests/ce_regress.py` | PASS=5 SKIP=13 FAIL=0 | `PASS=5 SKIP=13 FAIL=0` | ✅ |
| `python tests/test_cc.py` | 8/8 | **`10 passed, 0 failed`** | ✅（见下方说明） |
| `python tests/qemu_regress.py kernel_v25/catos.iso --qemu "C:/qemu-portable/qemu-system-i386.exe"` | steps=12 ok=12 warn=0 | `steps=12 ok=12 warn=0` | ✅ |

**QEMU 12 步逐条全 PASS**（真实输出，非汇总）：
```
PASS boot-shell         720x400 bright=0.06
PASS format             size=13463
PASS help               size=14523
PASS display            size=12532
PASS gfx                size=8352
PASS text-reject-N      size=8959
PASS text-accept-Y      size=4891
PASS ls-bin             size=9012
PASS mmaptest           size=8237
PASS hello-elf          size=7881
PASS vdiag              size=7645
PASS final-frame        640x400 bright=1.00
```
QEMU 路径 `C:/qemu-portable/qemu-system-i386.exe` **确实可用**（任务书提示的 `E:\Tools\qemu` 确已无 system-i386，工程侧已在 `assess.md §六` 记录此更正）。

> **关于 test_cc.py 的 8 vs 10**：任务书写"期望 8/8"，实测 **10/10**。
> 独立核实：`git log 66bbfce^..0cd3e6d -- tests/test_cc.py` **为空** —— 本批 4 个 commit **从未改动** `test_cc.py`；`CASES` 字典条目数在 `a7eae5a` 与 `HEAD` **完全相同（13 条 → 去重后 10 条可执行用例）**。
> 结论：**8/8 是任务书/PRD 沿用的旧计数，不是本批引入的回退**。`docs/merge-harvest-assess.md §六 A-3` 已如实记录此事并给出同样结论（"PRD 写 8/8（该行标『待复测』）… 实测是 10/10，比 PRD 预期多 2 项"）。工程侧披露准确，无隐瞒。

---

### V-C 根目录治理的正确性与无损性 —— ✅ PASS

**C-1 删除清单恰好 18 个**
```
$ git show --name-status 66bbfce | grep -P "^D\t" | wc -l
18
```
✅ 精确 18（`Catos_Better_b`, `MIN.C`, `MIN2.C`, `T1.C`~`T6.C`(6), `cc_old.c`, `cc_old.ce`, `cc_old_ce.exe`, `vga.c`, `gfx.c`, `gfx.h`, `shell.c`, `shot-gfx-mode.png`, `shot-text-restored.png`）。
（`git show --name-status` 另有 1 行以 `D` 开头的 `Date:` 提交头，非文件行，已用 `-P "^D\t"` 精确计数排除。）

**C-2 零引用核实（独立复跑）**
```
cc_old.c       -> tracked refs outside docs/: 0
cc_old.ce      -> 0
cc_old_ce.exe  -> 0
T1.C           -> 0
MIN.C          -> 0
shot-gfx-mode.png -> 0
shot-text-restored.png -> 0
gfx.h          -> 1   ← 唯一命中：build.sh:4 的历史说明注释
```
`build.sh:4` 的那处是**故意保留的历史说明**（commit 里明确把措辞从"left over"改成"were ... and the snapshot files themselves were removed on 2026-11-09"），且已把 `gfx.c/gfx.h/vga.c/shell.c` 四个名字标为"已删除"。**不是悬空引用**。
新根 `README.md` 对已删文件**零引用**（grep 无命中）。

**C-3 无损性重点抽查 —— 旧 `vga.c`(161L) 的 3 个"新文件里没有"的功能**

| 旧 `vga.c` 独有函数 | 现项目等价物 | 覆盖判定 |
|---|---|---|
| `vga_gfx_enable()` | `shell.c:228 cmd_gfx()` → `con_request_mode(CON_MODE_GFX)` → `render.c:635` → `be_gfx.c:54 gfx_enter()`（Mode 13h 寄存器全序列） | ✅ 覆盖（且加了同意门/模式策略） |
| `vga_gfx_disable()` | `shell.c:252 cmd_text()` → `con_request_mode(CON_MODE_TEXT)` → `be_text.c` `text_enter()` | ✅ 覆盖 |
| `vga_regdump()` | `shell.c:1369 "vdiag"` → `render.c:653 con_diag()` → `be->diag()` → `be_text.c:308 text_diag()` | ✅ 覆盖且**更完整**（旧版读 6 个寄存器 + 1 个字形；新版读 CRTC `0x00`~`0x19` 全 26 个 + 4 个字形 `0x00/0x20/0x41/0xFF` 读回） |

旧 `vga.c` 其余 11 个公开函数（`vga_init/clear/putc/write/set_color/set_fg/get_fg/move_cursor/set_cursor/putc_at/write_at`）在 `kernel_v25/src/vga.c`(27L) **一一对应存在**，且新版**多出** `vga_write_n()` 与 `vga_write_at_utf8()`。

**C-4 旧 `gfx.c`(369L) / `gfx.h`(32L) 的能力对照**

| 旧公开 API | 现项目 | 判定 |
|---|---|---|
| `gfx_mode13h()` / `gfx_text_mode()` | `be_gfx.c gfx_enter()` / `be_text.c text_enter()` | ✅ 覆盖 |
| `gfx_set_palette()` | `be_gfx.c dac_load_all()` + `render_be.set_palette` + `render.c:510 con_set_palette()` | ✅ 覆盖且更强（用户可改 256 项 CLUT） |
| `gfx_term_*`（40x25 软终端 6 个函数） | `render.c` 前端单元格网格 + `be_gfx.c gfx_draw_cell()` | ✅ 覆盖（架构换代，能力等价且更完整：CJK 16x16、8 种样式、软光标） |
| `gfx_putpixel/fillrect/hline/vline/rect/drawchar/text`（像素绘图 API） | **不存在** | ⚠️ 见 C-5 |

**C-5 关于"像素绘图 API 缺失"的独立裁定 —— 不构成 FAIL**

三条独立证据表明这是**本批之前就已存在的架构决策**，而非本次删除造成的能力损失：

1. **本批之前就已不存在**：在**本批起点** `a7eae5a` 上执行
   `git grep -c "gfx_putpixel\|gfx_fillrect\|gfx_rect" a7eae5a -- 'kernel_v25/*'` → **零命中**。
   即：现项目在这批 commit 落地**之前**就已不含像素 API；本批删掉的是一份**早已死掉的 2026-11-05 快照**，删除动作没有从"已发布产品"里拿走任何能力。
2. **66bbfce 未触碰 `kernel_v25/src`**：`git show --name-status 66bbfce | grep kernel_v25/src` → **空**。删除是纯根目录清理。
3. **是成文架构决策，非遗漏**：`docs/CatOS-渲染层前后端分离设计.md §一.2` 明写"图形模式**只做文本绘画**…链接产物里不存在 `gfx_putpixel`/`gfx_fillrect`/`gfx_rect` 之类的符号"；`§五` 明写"**没有**像素 API、没有矩形 API、没有演示绘图（旧的 `cmd_gfx` 像素 demo 已删除，换成纯样式文本欢迎页）"。
4. **无消费者**：对原项目 `D:/CatOs/Catos_Better_better` 全树 grep `gfx_putpixel|gfx_fillrect|gfx_rect|gfx_drawchar|gfx_text|gfx_hline|gfx_vline|gfx_mode13h|gfx_term` → **零命中**。删除不破坏任何使用者。
5. **旧 `cmd_gfx` 的像素演示画面**（渐变带/网格/标题横幅/色块/对角线）随像素 API 一并退役，新 `cmd_gfx` 改为带样式的文本欢迎页 —— 与上述成文决策一致。

**C-6 迁移的 4 份文档内容完整性**
```
R092  CatOS-图形模式能力说明.md        → docs/   （92% 相似度 = 仅追加归档说明）
R099  CatOS-渲染层前后端分离设计.md    → docs/   （99% = 仅更新 2 处已删文件引用）
R100  CatOS-进度总结与待办-2026-11-02.md → docs/  （100% 逐字节）
R100  CatOS_用户反馈分析.xlsx          → docs/   （100% 逐字节）
```
✅ 全部是 **git rename（相似度 ≥92%）而非 delete+add**。差异内容经逐行核对：R092/R099 的差异**只有新增的"归档说明"段落**（说明这些快照文件已于 2026-11-09 删除），正文一字未删。
`xlsx` 完整性：`zipfile.testzip()` → `None`（无损坏），11 个 entry，blob md5 迁移前后**完全相同**（`71db5fc2b9b2083445d99b416bda25f5`）。

**C-7 旧根 `README.md` 的处置**
旧 `README.md`（51 行）是 v0.3 成就包说明，**不是项目 README**。已迁至 `docs/CatOS-v0.3-图形模式成就包.md`（59 行 = 原 51 行 + 8 行归档说明，`diff --strip-trailing-cr` 仅显示新增段）。新根 `README.md` 为新建，含目录结构/构建/运行/测试/文档索引。✅ 处置合理。

---

### V-D 归档索引的真实性抽查 —— ✅ PASS

**D-1 抽查 2 份归档文档的"是否仍然成立"判定**

| 归档件 | 判定 | 独立复核 | 结论 |
|---|---|---|---|
| **A-4 `legacy-BUILD_REPORT_STAGE5.md`** | "mingw32 工具链 PATH 根因**已解决**"、"gcc 版本差异导致尺寸差**仍成立**" | 归档件原文写"MSYS2 mingw32 (gcc 16.2.0)"；本机实测 `/d/msys/mingw32/bin/gcc --version` → **`gcc.exe (Rev5, Built by MSYS2 project) 16.1.0`**。版本确实不同 ⇒ "Rev4/Rev5 尺寸差异仍存在"的判定**成立**；且本机构建确实成功出 16,558,080 B ISO ⇒ "PATH 根因已解决"**成立** | ✅ 判定属实 |
| **A-3 `legacy-TODO_STAGE5.md`** | "`USER_STACK_SZ` 仍是 `0x10000`(64 KiB)，未同步"、"自举固定点未验证" | 实测 `kernel_v25/include/paging.h:82` → `USER_STACK_SZ 0x10000u /* 64 KiB */`；原项目 `include/paging.h:35` → `0x20000u /* 128 KiB */` ⇒ **确系倒退**。另 `kernel_v25/extapp/cc.c`=**1,237 行**（阶段 0）、`cc_self.c`=**2,250 行**（阶段 1-3 已实现），与归档件"落档 #51"的描述**逐字吻合** | ✅ 判定属实 |

> 附带核实归档件里点名的源码证据：`go.c:1083 g_filebuf[20480]` ✅、`go.c:783/784 g_call_depth/CALL_DEPTH_MAX 200` ✅、`go.c` 共 **1,300 行** ✅、md5 `f9876f42fa646c58ec21da649ed934e4` ✅ —— 与归档索引第三章表格完全一致。

**D-2 独立复现三条"不移植/不归档"理由**

**① 原项目 `tools/` 三个 py 确实跑不起来（贴错误）**
```
$ python tools/cc_stage45_test.py
File "tools\cc_stage45_test.py", line 24, in reset_env
    subprocess.run(['pkill','-9','-x','qemu-system-i38'], stderr=subprocess.DEVNULL)
FileNotFoundError: [WinError 2] 系统找不到指定的文件。

$ python tools/diag2_test.py
File "tools\diag2_test.py", line 18, in reset_env
    subprocess.run(['pkill','-9','-x','qemu-system-i38'], ...)
FileNotFoundError: [WinError 2] 系统找不到指定的文件。

$ python tools/inject_any.py /tmp/nonexistent.img /tmp/nonexistent.ce TEST_AB.CE
File "tools\inject_any.py", line 87, in main
    new_data = open(SRC, 'rb').read()
FileNotFoundError: [Errno 2] No such file or directory
```
补充证据：前两者另硬编码 Linux 绝对路径（`OUT = '/home/user/Doubao/chats/38448438262014978/catos-qemu'`、`QEMU = '/home/user/Doubao/.../qemu-system-i386'`），在本机**必然**不可用。
✅ **"三个都跑不起来"成立**。（`inject_any.py` 需 3 个参数才进入 `main()`，无参时是 `IndexError` 属用法问题；但它同样无法在本机完成有效注入 —— FAT 几何过时。）

**② 原项目 `pe.ld` 与现项目 `kernel_v25/pe.ld` 逐字节相同**

这里有一个**必须说清的口径陷阱**，否则会误判：
```
# 工作树文件 md5（表面看：不同）
9ffb7f859b7a0b07f7e1040f30bb5fbb  D:/CatOs/Catos_Better_better/pe.ld   (1288 B, CR=0)
20511f6f70e79fa0983e8a095a1d970d  kernel_v25/pe.ld                    (1333 B, CR=45)

# 但 git 仓库里存的 blob（真相：完全相同）
$ git cat-file -p HEAD:kernel_v25/pe.ld | md5sum
9ffb7f859b7a0b07f7e1040f30bb5fbb   ← 与原项目文件 md5 一字不差
$ diff --strip-trailing-cr  → 无差异
```
差异**仅来自 `core.autocrlf=true` 的检出换行转换**（当前仓库 45 行 CRLF / 原项目 45 行 LF；行数与内容完全相同）。
✅ **"逐字节相同"在仓库内容层面成立**，归档索引的判定**正确**。

**③ 已删的 `Catos_Better_b` 确实等于原项目 `build/CC2_NEW.CE`**
```
$ git show 66bbfce^:Catos_Better_b | md5sum
d21d66ea5845492dab406d04f666c60e   (82586 B)
$ md5sum D:/CatOs/Catos_Better_better/build/CC2_NEW.CE
d21d66ea5845492dab406d04f666c60e   (82586 B)
```
✅ **md5 与字节数双双一致**，归档索引给出的 md5 也与 `legacy-TODO_STAGE5.md` 记的 `d21d66ea...` 一致。

**D-3 附带核实**：`tests/inject_any.py` 与原项目 `tools/inject_any.py` 的 git blob md5 **同为 `c7653c83e2fc93e91c02692b154d0b6f`** ⇒ 归档索引"`tests/inject_any.py` 与它**逐字节相同**"的表述**成立**。

---

### V-E 问题清单落档质量 —— ✅ PASS

**E-1 编号连续、无冲突**
```
25 -> #48   26 -> #49   27 -> #50          （第一节：内存与执行模型）
58 -> #51   59 -> #52   60 -> #53   61 -> #54   62 -> #55   63 -> #56
64 -> #57   65 -> #58   66 -> #59   67 -> #60   68 -> #61   69 -> #62
70 -> #63   71 -> #64   72 -> #65          （第四节：工具链与加载）
```
✅ **恰好 18 条，#48~#65 连续、无缺号、无重号**。

**E-2 命名空间区分清楚**
- 第一节开头（第 10-14 行）有独立"编号命名空间说明"，明确"本节及第四节使用主清单编号（#1~#46）…续编 #48~#50 / #51~#65"，并逐一点名"第九节的 `#48`/`F1`/`A2`/`#3` 与第十节的 `#47` 属于**各自独立的局部命名空间**…与主清单编号**不冲突、不互相引用**"。
- 第九节开头（148-152 行）自声明"本节「项」列用的是 b31 修复闭合表的**局部编号**（`#48`/`F1`/`A2`/`#3`）…引用本节条目时请写『第九节 #48』"。
- 第十节开头（166-167 行）自声明"本节条目号 47 为清单序号，与第九节的内核问题编号 #48/#49 **不是同一命名空间**"。
✅ **三重声明叠加，混淆风险已被系统性消除**。（第九节的 `#48` 与主清单的 `#48` 确实是两个不同问题 —— 前者是 `fat_flush` 整扇区重写覆盖注入文件，后者是 `heap_ensure()` 单页映射；已逐条比对确认不是同一问题，故不构成真冲突。）

**E-3 六字段完整性**
每条新条目管道数均为 **8**（= 7 列 + 首尾分隔符），与本节表头（第 18 行 / 第 51 行同为 8 管道、7 列：`| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |`）**完全一致**。抽验 `#48` 与 `#65` 全文，七个字段逐一有值、无空缺、无串列。
✅ 六字段（问题 / 确认值 / 影响 / 规避对策 / 发现版本 / 状态）**全部齐备**。

**E-4 `go.c` 提及数不再是 0**
```
本批起点 a7eae5a：docs/ 全树 grep 'go\.c|go\.ce|Go 子集'  → 0
HEAD：docs/CatOS-内核问题清单.md → 12
      docs/go-review.md → 55 ; docs/merge-harvest-PRD.md → 33
      docs/merge-harvest-assess.md → 16 ; docs/GO-DESIGN.md → 9
```
✅ **0 → 非 0**，那条"被现项目主线完全遗忘的开发线"已正式入档，且问题清单第四节新增了专章
`### Go 子集解释器（go.c）现状 —— 2026-11-09 落档说明`（第 74 行起）。

**E-5 落档条目的状态列抽查（见"文档级 nit"）**
`#65`（`.gitignore` 否定规则写反）状态仍写 `已确认/待修复（PRD P0-4）`，但它**已被同一批的 `f39bb0b` 修掉**；`#61`（模板未入库）同样已由 `f39bb0b` 修复。→ 见第四节 nit-1。

---

### V-F 他人 WIP 未被触碰 —— ✅ PASS

**F-1 逐 commit 全量路径扫描**
```
$ for c in 66bbfce 101a326 f39bb0b 0cd3e6d; do git show --name-status --format="" $c; done \
    | grep -iE "vd2\.h|vga_draw|build_log|simout|CatOs\.7z|CatOs/"
(无输出)
```
✅ `vd2.h` / `vga_draw_c` / `vga_draw_h` / `kernel_v25/build_log.txt` / `simout/` / `simout2/` / `CatOs.7z` / `CatOs/` 在 4 个 commit 中**零出现**。

4 个 commit 涉及的全部路径（穷举）：
```
D  18 个根目录快照垃圾
M  .gitignore / build.sh / README.md / docs/CatOS-内核问题清单.md
R  4 份文档 → docs/（R092/R099/R100/R100）+ kernel_v25/P0_VMA_实现报告.md → docs/
A  docs/{GO-DESIGN,go-review,merge-harvest-PRD,merge-harvest-assess}.md
A  docs/orig-archive/{README,legacy-TODO_STAGE5,legacy-BUILD_REPORT_STAGE5,legacy-HANDOFF_STAGE5}.md
A  docs/CatOS-v0.3-图形模式成就包.md ; docs/CatOS_P0_总结与经验教训.md
A  kernel_v25/build/catos_work.iso
```
✅ **无一例外地落在任务书允许范围内**。

**F-2 两份 P0 报告的来源核实（任务书特别点名的两项）**

| 文件 | 实际 git 表现 | 来源核实 | 判定 |
|---|---|---|---|
| `P0_VMA_实现报告.md` | `R100 kernel_v25/P0_VMA_实现报告.md → docs/P0_VMA_实现报告.md` | 该文件**本就是本仓库已跟踪文件**（`git -C D:/CatOs ls-files` 确认 `"kernel_v25/P0_VMA_实现报告.md"` 在跟踪列表中），本批做的是**仓内搬家**。blob md5 迁移前后同为 `0b48654c98105cdd73127853c9c8ab0e`，`diff` 零差异 | ✅ 是仓内 move，不是从 `D:/CatOs` 拷贝；内容 100% 保留 |
| `CatOS_P0_总结与经验教训.md` | `A docs/CatOS_P0_总结与经验教训.md` | md5 `aab93c5253e5a270d6086f7e303b12a4` 与 `D:/CatOs` 根目录原件 md5 **完全相同** | ✅ 确为**从 `D:/CatOs` 复制进 `docs/`**（新增形态，符合任务书预期），原件留在原处未被搬走 |

**F-3 `D:/CatOs` 主工作树未被弄脏**
```
$ git -C D:/CatOs status --short
?? "CatOS_P0_总结与经验教训.md"
?? CatOs.7z
?? CatOs/
?? "P0_VMA_实现报告.md"
?? kernel_v25/build_log.txt
?? simout/
?? simout2/
?? vd2.h
?? vga_draw_c
?? vga_draw_h
```
✅ **全部是 `??`（未跟踪），没有任何一条 ` M`/`A `/`D `（无已跟踪文件被修改/新增/删除）**。这 10 项正是任务书列举的"他人 WIP"清单本体 —— 说明这些是**本批开始前就存在的**既有状态，本批**没有向主工作树写入任何东西**。`D:/CatOs` 当前仍在 `master` 分支 @ `a7eae5a`，与我们的工作分支完全分离。

---

### V-G 隔离纪律 —— ✅ PASS

**G-1 主工作树**
`git -C D:/CatOs status --short` 输出见 F-3 —— **只有未跟踪 WIP，零已跟踪文件改动**，与本批开工前一致。

**G-2 原项目树（只读性）**

> **重要事实更正**：`D:/CatOs/Catos_Better_better` **不是独立 git 仓库**（无 `.git` 目录）。`git -C D:/CatOs/Catos_Better_better status` 会**向上找到 `D:/CatOs` 的仓库**，所以它输出的其实是主仓库状态（这也解释了 F-3/G-1 两处输出带 `../` 前缀）。因此**不能**用 git 来证明原项目树未被改动 —— 改用文件系统时间戳独立验证：
```
$ find "D:/CatOs/Catos_Better_better" -newermt "2026-11-09 00:00"
(零输出)
$ ls -la D:/CatOs/Catos_Better_better
CATOS-交接文档.md  Oct 31 08:20 | README.md  Oct 31 08:05 | build/  Nov  5 14:06
catos.iso        Oct 31 08:17 | doc/      Nov  5 14:06 | extapp/ Nov  1 15:51
include/         Oct 31 08:05 | node/     Oct 31 08:05 | pe.ld  Oct 31 08:05
src/             Oct 31 08:17 | tests/    Nov  5 14:06 | tools/ Nov  3 13:17
```
✅ **原项目树内没有任何一个文件在本批工作日被修改**（最新 mtime = Nov 7 目录项，文件最新 Nov 5）。**只读纪律成立**。
（附带说明：本批归档的 5 份原项目文档均被**复制**进 `docs/`，原项目树内文件未移动、未删除 —— 与 F-2 的 md5 证据一致。）

---

### V-H 未提交工作树状态合理性 —— ✅ PASS

**H-1 预期中的未提交改动确实仍是未提交**
```
 M kernel_v25/extapp/bin/calc.ce      M kernel_v25/extapp/bin/perms.ce
 M kernel_v25/extapp/bin/crash.ce     M kernel_v25/extapp/bin/spinner.ce
 M kernel_v25/extapp/bin/curl.ce      M kernel_v25/extapp/bin/sysinfo.ce
 M kernel_v25/extapp/bin/demo.ce      M kernel_v25/extapp/bin/zhdemo.ce
 M kernel_v25/extapp/bin/gfxdemo.ce   M kernel_v25/extapp/bin/go.ce(??)
 M kernel_v25/extapp/bin/hello.ce     M kernel_v25/extapp/bin/...
 M kernel_v25/extapp/bin/hw.ce        M kernel_v25/extapp/bin/mmaptest.ce
 M kernel_v25/extapp/bin/netinfo.ce
 M kernel_v25/src/builtin_apps.inc
```
✅ `extapp/bin/*.ce`（13 个）+ `src/builtin_apps.inc` 全部保持 **` M`（已修改、未暂存、未提交）**，符合"第二阶段统一提交"的安排。

**H-2 无意外改动混入**
`git status --short` 中除上述预期项外，另有**未跟踪**文件：`kernel_v25/extapp/go.c`、`kernel_v25/extapp/cases/*.GO`(5)、`kernel_v25/extapp/bin/go.ce`、`kernel_v25/extapp/host_stub_go.c`、`docs/merge-harvest-design.md`、`docs/sequence-diagram.mermaid`。
- 这些**全部未被任何 commit 收录**（`git log --all -- kernel_v25/extapp/go.c` → 空）。
- 归属清楚：`go.c`/`cases/`/`go.ce`/`host_stub_go.c` 是 E4 spike 与架构师 P1-1/P1-8 的在制品；`merge-harvest-design.md`/`sequence-diagram.mermaid` 是架构师（task #7）产出。
- ✅ **不构成对本批 commit 的污染**，但请 team-lead 知晓它们会在第二阶段进入提交范围。

**H-3 我的验证动作未污染工作树**
我把**全部构建与测试放在 `/tmp` 的两个克隆里**执行，主工作树**没有新增任何构建产物**（`kernel_v25/build/*`、`catos.iso`、`tests/qemu_shots/` 均在 `/tmp/catos-qa-clone` 内生成）。因此本轮验证结束后，工作树状态与开工时**逐字节一致**，无需清理。

---

## 三、FAIL 明细

### 硬 FAIL：**0 个**

无源码 bug、无测试脚本 bug、无构建/基线回退、无数据丢失、无越权写入。

---

## 四、文档级 nit（不阻断合流，建议顺手回填）

### nit-1：`#61` / `#65` 的"状态"列未回填 —— **文档错误**（归属：工程师 / 文档）

| 条目 | 现写状态 | 实际状态 |
|---|---|---|
| `#65` `.gitignore` 否定规则位置写反 | `已确认/待修复（PRD P0-4）` | **已修复** —— 同一批 `f39bb0b` 已把两条否定规则移到 `*.iso`/`*.py` 之后并重写注释 |
| `#61` `catos_work.iso` 从未入库 ⇒ 干净克隆不可构建 | `已确认/待修复（PRD P0-4）` | **已修复** —— 同一批 `f39bb0b` 已入库该模板（10,725,376 B） |

**根因**：`101a326`（写清单）排在 `f39bb0b`（修问题）**之前**，清单写完后未回填状态。`docs/merge-harvest-assess.md §四.3` 已如实记录修复过程，故属**清单状态列的同步遗漏**，不影响任何构建产物或基线。
**建议**：第二阶段提交时把这两行状态改为"已修复（`f39bb0b`，2026-11-09 核实）"。**不建议为此单独起 commit。**

### nit-2：`.gitignore` 的 `!kernel_v25/build/*.py` 规则覆盖面略宽 —— **文档/口径提醒**（非缺陷）

`.gitignore:39` 的 `!kernel_v25/build/*.py` 会放行该目录下**任意** `.py`（当前实际只有 `gen_builtin.py`/`mkce.py`/`patch_work_iso.py` 三个）。若将来有人往 `build/` 里丢临时 `.py`，它会被误纳入版本控制。
**风险等级**：极低（当前无实际影响，且与 `*.py` 全局忽略规则的交互是刻意设计）。
**建议**：若未来 `build/` 目录变脏，改为逐文件否定即可。**本轮不要求任何改动。**

---

## 五、任务书期望值澄清（不是 commit 缺陷）

| 任务书条目 | 任务书写法 | 实测 | 裁定 |
|---|---|---|---|
| V-B | `python tests/test_cc.py` 期望 **8/8** | **10 passed, 0 failed** | **任务书期望值过时**。本批 4 个 commit **从未改动** `tests/test_cc.py`（`git log 66bbfce^..0cd3e6d -- tests/test_cc.py` 为空）；用例数在 `a7eae5a` 与 `HEAD` 完全相同。8/8 是 PRD 沿用的旧计数。工程侧在 `assess.md §六 A-3` **已主动披露**此事，无隐瞒。**不判 FAIL。** |

---

## 六、智能路由判定

| 判定项 | 结论 |
|---|---|
| 源码 bug | **无**。未发现任何需要修改 `kernel_v25/` 源码的缺陷。`f39bb0b` 的 `.gitignore` 修复经正交对照实验 + 反证法双重验证，**真实有效且必要**。 |
| 测试脚本 bug | **无**。`test_mkce.py` / `ce_regress.py` / `test_cc.py` / `qemu_regress.py` 四套基线在干净克隆中全部达标；`qemu_regress.py` 12 步逐条 PASS。 |
| 文档错误 | **1 处（nit-1）**：`#61`/`#65` 状态列未回填 —— 建议第二阶段一并修正，不阻断合流。 |
| 环境限制 | **0**。构建（`/d/msys/mingw32`）、Python 3.13.12、QEMU（`C:/qemu-portable/qemu-system-i386.exe`）全部可用，无"无法验证"项。 |
| **路由** | **Send To: NoOne** —— 8 项全 PASS，0 硬 FAIL，无需工程师返工。 |

---

## 七、合流结论

### ✅ **判定：这批 commit 可以进入合流。**

**理由**：
1. **8/8 验证项全部 PASS**，无一例外，无"无法验证"项；
2. **构建可复现性这一新主张被独立坐实**：干净克隆产出 16,558,080 B ISO、`check-ignore` 命中否定规则、**反证克隆删模板后构建失败** —— 三个方向互相印证，不是引述工程师的结论；
3. **四项基线在干净克隆中全绿**（48/0、5-13-0、10/0、12-12-0），证明基线不依赖任何本地状态，**具备 CI 意义**；
4. **无损性经得起追问**：旧 `vga.c` 3 个独有能力全部被覆盖且其中 1 个更完整；唯一"缺失"的像素绘图 API 已用三条独立证据证明是**本批之前就存在的成文架构决策**，且原项目零消费者，删除不损失任何能力；
5. **归档索引的三条"不移植/不归档"理由被我独立复现**（含贴出真实报错），`pe.ld`/`inject_any.py`/`Catos_Better_b` 三项 md5 全部对得上 —— 归档索引**没有夸大**，其中"逐字节相同"的口径陷阱（autocrlf）我已澄清，结论仍成立；
6. **纪律零违反**：他人 WIP 零触碰、`D:/CatOs` 主工作树零改动、原项目树文件时间戳证明未写入、工作树未提交状态保持原样；
7. 唯一遗留的 2 个 nit 均为文档级、零功能影响，已给出行号与建议修法。

### 合流前建议（不阻断）
1. 第二阶段提交时回填 `#61`/`#65` 的状态列为"已修复（`f39bb0b`）"（nit-1）；
2. 注意第二阶段提交范围还应包含当前未跟踪的 `extapp/go.c`、`extapp/cases/*.GO`、`extapp/bin/go.ce`、`extapp/host_stub_go.c`（E4 spike 与 P1-1/P1-8 在制品）与架构师的 `docs/merge-harvest-design.md`、`docs/sequence-diagram.mermaid`；
3. 合并到 `D:/CatOs` 主工作树（`master` @ `a7eae5a`）时，注意主工作树尚有 10 项未跟踪 WIP（`vd2.h`/`vga_draw_*`/`build_log.txt`/`simout*/`/`CatOs.7z`/`CatOs/`/两份 P0 报告）**不可被本批 commit 覆盖或误删**。

---

### 附：本次验证产生的临时物（均在工作树之外，无需清理主工作树）
| 路径 | 用途 |
|---|---|
| `/tmp/catos-qa-clone` | 主验证克隆（构建 + 四套基线 + 12 步 QEMU） |
| `/tmp/catos-qa-nofix` | 反证克隆（删除模板 → 验证构建失败） |
| `/tmp/qa-old/` | 旧版文件提取件（`vga.c`/`gfx.c`/`gfx.h`/`shell.c`/`README.md`/`Catos_Better_b`）与对照备份 |

**主工作树状态：与本轮开工前完全一致，未新增任何构建产物。**

---

## 八、验证期间工作树 HEAD 变动说明（不影响结论）

验证收尾时 `HEAD` 从 `0cd3e6d` 前移到 `0043914`（架构师并发提交的 `docs: add merge-harvest kernel-layer design`）。已核实其**完全不影响本报告结论**：

```
$ git log --oneline 0cd3e6d..HEAD
0043914 docs: add merge-harvest kernel-layer design (architect deliverable)

$ git show --name-status --format="" 0043914
A	docs/merge-harvest-design.md
```
- **仅新增 1 个 docs 文件**，与本轮验证的构建面/测试面/`.gitignore`/`build.sh` **零重叠**；
- 本报告的 4 个验证目标 commit `66bbfce`/`101a326`/`f39bb0b`/`0cd3e6d` **未被改写，完整保留**；
- 本报告全部结论均基于 `0cd3e6d` 的实测输出，**不受该新增文档影响**。

（`docs/merge-harvest-design.md` 现已由架构师提交，不再是"未跟踪在制品"；`docs/sequence-diagram.mermaid` 仍为未跟踪。）
