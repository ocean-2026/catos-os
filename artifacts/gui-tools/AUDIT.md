# 对抗式验收审计报告 —— blkhtml HTML→块树转换工具

- 审计基线：`feat/blkhtml` @ `89584c4ca89b0a571ba25e507c78358aeec731b2`
- 审计时间：2026-11-25（Asia/Shanghai）
- 审计方式：只读复审；所有临时副本 / 一次性脚本均在 `/tmp`，未修改任何被审仓库文件，未 `git commit`，未进入 `gui-hosted` / `gui-fix`，未写 `gui-preview`。
- 复核立场：不采信子代理报告结论；几何 / 二进制 / 像素 / 串口日志均由审计方独立复算或独立解包。

---

## 1. 总判定

**PASS WITH FIXES** —— 工具可跑、测试真实可失败、契约与文档口径一致、内核改动守红线；但 settings 主样例缺内核几何对照证据，且存在一处角色映射与文档 / 内核枚举不符，需修复后复核。

---

## 2. 十七项逐条结论

### A. 工具可跑性与测试真实性

**#1 PASS** —— `python3 -m py_compile tools/blkhtml/*.py` 全过（`COMPILE_OK`）；`html2blk / blk2bin / preview / validate` 四个脚本 `--help` 均 RC=0。
- 关键输出：四个 usage 文本正常打印，无 traceback。

**#2 PASS** —— `python3 tools/blkhtml/tests/run_tests.py` 真跑：`Ran 26 tests in 0.181s`，`OK`，RC=0。
- 非空断言抽查（读源码确认非空壳）：
  - `test_record_is_40_bytes_pad0_zero`：断言 `len(rec)==40`、`REC_BYTES==40`、`pad0==0`、解包元组逐字段相等。
  - `test_touching_sides_zero_corners_and_stroke`：断言贴边侧三角 `radius` 归零、`stroke/shadow` 同侧被移除。
  - `test_unsupported_exits_nonzero`：断言 `float` 输入返回码 `1`。
- 变异测试（副本在 `/tmp/audit_mut/bad.blk.json`，仓库文件未动）：把 id=2 子块移出父框后，`validate.py` RC=1、7 条 FAIL（G1/B2/B1 各若干）；审计方独立脚本同样报出 7 条 issue。测试不是永远 PASS。
- 编译冒烟 `TestEmitCCompileSmoke.test_sample_c_compiles_to_object` 实际运行 OK（非 skip；gcc 与内核 include 均可用）。

**#3 PASS** —— 独立复算（未调用 `validate.py` 结论，脚本 `/tmp/audit_indep.py`，纯 stdlib）：
- `settings.blk.json`（18 节点）、`browser-toolbar.blk.json`（8 节点）：父子内容矩形包含、8pt 量化、贴边角 / 描边 / 阴影规则，独立 issue 数均为 **0**。
- 与 `validate.py` 结论对照：两份 `validate` 均 RC=0、`FAIL=0`；无不一致。

**#4 PASS** —— 独立 `struct.unpack` 解包五个样例的 `recs.bin/strings.bin`（未用工具内部函数）：
- 步长 40 字节；`pad0==0`；`pix_off==-1`；`text_off` 解出字符串与 JSON `text` 完全一致；`kind/layer/req_w/req_h` 与 JSON `ce` 镜像（缺省回退口径）一致。
- 结果：settings 18 / toolbar 8 / notepad 9 / startmenu 20 / fullbleed 6，全部 0 issue。

### B. 契约与文档一致性

**#5 PASS** —— 抽核 15 处 `文件:行号`（要求 12），逐条 `sed -n` / grep 复核全部命中：

| 引用 | 复核结果 |
|---|---|
| `block.h:153-203` | `typedef struct block {` … `} block;` ✓ |
| `ce_abi.h:87-98` | `ce_blk_rec_t` 12 成员定义 ✓ |
| `decl.c:172-173` | `id < 1` / 重复 → `E_DECL_BADID` ✓ |
| `decl.c:176` | `kind < BLK_COLUMN \|\| kind > BLK_CANVAS` → `E_DECL_BADREC` ✓ |
| `decl.c:179` | `layer != LAYER_WORK` → `E_DECL_LAYER` ✓ |
| `block.h:19-35` | `blk_kind` 枚举 ✓ |
| `ce_abi.h:320-325` | 槽 72–76 签名 ✓ |
| `layout.c:35-41` | `grid_snap` 定义，首行 calm 守卫 ✓ |
| `block.h:103-140` | `blk_spec` 结构体 ✓ |
| `paint.c:296-312` | `paint_halo` ✓ |
| `desc.c:28-29` | `pad_t/r/b/l` 拷贝 ✓ |
| `tokens.h:30-31` | `corner_radius` / `corner_mode` ✓ |
| `layout.c:105` | `bleed` 对齐父整框 ✓ |
| `block.h:171` | `block.bleed` 运行时字段 ✓ |
| `decl.c:329-330` | `n * sizeof(rec_scratch[0])` 拷贝 ✓ |

补充抽核：`decl.c:226-240`（`decl_ensure_root`）、`block.h:113`（`border_w`）、`paint.c:370-372`（DIVIDER 1px line）、`decl.c:317-318`（`decl_commit`）、`decl.c:182-183`（pix 字段拒绝）、`docs/块化GUI-v2-规格.md:134-136` 均命中。

**#6 PASS** —— 文档「内核能力现状表」与 `block.h` 的 `blk_spec` 字段逐一比对：
- 2.3 表映射的 `pad_*/gap/align_*/anchor/px_*/pct_*/min_*/max_*/m_*/overflow/scroll/drag_*/resize_*/z/fg/bg/accent/role_*/border_w/visible/focusable/inert` 均存在于 `blk_spec`（block.h:103-140）。
- 未发现编造字段；`align_self`（存在但无布局消费者）、`bleed`（仅运行时 `block`，不在 `blk_spec`）均已在文档 2.5 如实标注。

**#7 PASS** —— `schema.json :: css_properties.unsupported` 共 27 项，文档 7.1 表 27 行逐条覆盖（#16 与 #24 同指「多行 / 自动折行」，与 schema `unsupported[14]`、`unsupported[22]` 对应）；未发现文档新增无理由的不支持项。

**#8 PASS** —— 三处可疑口径全部正确：
- (a) 步长：文档 1.2/1.3/8.1/P1 均为 **40 字节/条**；出现的「36」仅是 `res` 字段字节偏移（offset 36），不是 36 字节步长。✓
- (b) 贴边方角：文档 §4.3、§4.4 明确「内核当前没有实现这套规则」，`schema.json :: flush_rule.note` 同口径；未声称内核已实现。✓
- (c) 8pt 对齐：文档 8.1 G1 注、9.1、9.3 #1 均写明「坐标 8pt 是生成器 / 产物约束，内核 `grid_snap` 不做坐标对齐」；源码 `layout.c:36` 首行 `if (!gui_style_calm() || v <= 0) return v;` 证实 grid_snap 仅 calm 档归整 padding/gap。✓

**#9 PASS** —— 五个样例 HTML（settings / browser-toolbar / fullbleed / notepad / startmenu）：
- 无写死 `#RRGGBB`（grep 0 命中）；无 `padding/gap: 2`；无 schema 不支持属性（grep 命中均为 `flex-direction` 含 `direction` 的误匹配）。
- 不加 `--allow-lossy` 逐个复跑 html2blk，五份全部 RC=0、无 UNSUPPORTED。
- `_patch_decl.py` 仅存在于 `artifacts/gui-tools/samples/_legacy/`，不在生成链路。

### C. 内核改动是否守红线

**#10 PASS（附 P3 备注）** —— `git diff --name-only` 跟踪文件仅 4 个：`build.sh`、`build/apps.list`、`src/gui/chrome.c`、`src/gui/gui_int.h`；新增内核文件 2 个：`src/gui/blkgen.c`、`extapp/blkgen.c`。与允许清单一致。
- `gui_int.h`：仅追加 1 条注释 + `blkgen_build()` 声明。
- `build.sh`：三处对象 / 源文件列表各加 `blkgen`；`boot.o`、`cpu.o` 仍居链接序最前；未删既有对象。
- `apps.list`：仅加一行 `blkgen`。
- `chrome.c`：枚举末尾追加 `APP_BLKGEN`；新增 `shot-blkgen` / `rest=="blkgen"` 分支；两处既有 `gui_demo_build()` 守卫被扩展为 `&& !blkgen` 合取项（`blkgen=0` 时条件与原逻辑等价，基线行为不变）。严格说这两行不是纯追加行，属兼容式改动（见缺陷 P3）。
- 无新增 CE 槽（`ce.c` / `ce_abi.h` 未改）；无 `ce_blk_rec_t` / `blk_spec` 旧字段语义变更；PE 符号表含 `_blkgen_build`，确认新对象已链接。

**#11 PASS** —— 仓库根无 ISO / 大二进制：
- `kernel_v25/catos.iso`（12,083,200 B）被 `.gitignore` 的 `*.iso` 规则忽略，`git status` 不列出；`kernel_v25/build/catos_work.iso` 为跟踪文件，`git status` 干净、mtime 18:27（基线时刻）。
- 无 >1MB 未跟踪文件；未发现 `catos-v2.80*.iso` 被改动。

**#12 PASS** —— `python3 kernel_v25/tools/check_link_layout.py kernel_v25/build/kernel.pe --strict`：**RC=0**。
- 尾行为 arena-window 静态提示；`[link][warn]` 共 348 条，均为既有审计提示（与 `run/guard_strict.log` 条数一致），非失败。

### D. 证据可信度

**#13 FAIL** —— 任务要求「从串口日志取 BLKGEN 行，与 `samples/settings.blk.json` 18 块逐块比对（容差 0）」。
- 磁盘上**不存在 settings 18 块的内核几何 dump**：`run2/serial-run.log`（与 `serial-toolbar.log` 同内容）只有 7 块，几何对应 `browser-toolbar`；`run/serial-run.log` 是另一套 7 块 sample。
- 审计方独立解析 `run2/serial-run.log` 对 `settings.blk.json`：18 节点中 root(id=0) 日志缺失，id=2/3/4 不一致，14 个节点无对应记录；`run2/compare_geom.py` 自身默认对 settings JSON 跑，结果 **matched 0/7**。
- 旁证：`run2/settings.recs.bin` 与 samples 版 md5 一致（`e4588128…`），说明 settings 片段被编进了内核，但对应几何串口输出未保留下来。
- 可复现命令：`grep BLKGEN artifacts/gui-tools/run2/serial-run.log`；`python3 artifacts/gui-tools/run2/compare_geom.py`。

**#14 PASS（有保留）** —— PNG 独立解码（PIL）：
- 全部 PNG 均 **800×600**；非黑像素占比 **100.00%**（>50% 阈值满足）。
- `run/` 配对复算：app `6238/480000 = 1.300%`，一致率 `98.700%`；calm `13304/480000 = 2.772%`，一致率 `97.228%` —— 与 `run/REPORT.md` 表内数字**完全一致**（相对误差 0，远优于 0.2% 容差）。
- 保留项：`run2/` 仅有 `qemu-toolbar-app.png` 对 `preview-toolbar-app.png`，差异 `44895/480000 = 9.353%`（一致率 90.647%），**无任何报告数字可供对照**；缺 `qemu-toolbar-calm` 配对。

**#15 PARTIAL** —— 证据-断言对应：
- `samples/REPORT.md` 表内 CE FAIL 数复核吻合：settings 1、startmenu 17、notepad 2、toolbar 6、fullbleed 0（已剔除 `FAIL=N` 汇总行计数）；stats 文本块数 / 池字节 / recs 字节与独立复算一致。
- `run/REPORT.md` ③ 的 B 路线 verdict：`serial-ce.log` 有 `decl reject code=-39`（raw，含 kind=12 button）、`decl commit n=7 ok=1`（proj）、`leaked=0`，与报告「raw rc=-39；proj rc=0, n=7, leaked=0」一致。
- 不符项：`run/REPORT.md` ⑤-4 仍断言 emit-c 角色名缺陷「未改工具」，与当前工具状态矛盾（见第 4 节矛盾 #1）。

### E. 反脆弱

**#16 PARTIAL** —— 交付目录扫描：
- 未发现 `/tmp` 写入引用；无 0 字节 / 空文件；`tools/` 内无 `.bak/~/.tmp` 临时脚本；`_patch_decl.py` 已隔离到 `_legacy/`。
- 7 个 `.pyc` 落在 `tools/blkhtml/__pycache__/`、`tests/__pycache__/`、`artifacts/run/__pycache__/`；`.gitignore` 已含 `__pycache__/` 与 `*.pyc`，不会进仓库（P2 备注）。
- `blkcommon.py:136` 写死兄弟树绝对路径 `/home/user/work/catos-os/kernel_v25/src/font_ascii.inc`（当前环境存在，换检出路径即 `FileNotFoundError`）；fallback 相对路径在 137-138 行。
- 证据目录在审计期间被并发改写：审计初见的 `run2/qemu-app.png`、`qemu-calm.png` 随后被 `qemu-toolbar-app/calm.png` 取代，`serial-run.log` 被 toolbar 版本覆盖；证据缺乏冻结快照与校验清单。
- 生成预览混排在源码样例树内：`tools/blkhtml/out/`、`tools/blkhtml/samples/out/`（HTML + PNG 产物）。

**#17** —— 见第 5 节「下一任维护者会被坑的三个点」。

---

## 3. 缺陷清单

### P0 阻断
无。

### P1 应修

**P1-1　settings 主样例缺内核几何对照证据（对应 #13）**
- 位置：`artifacts/gui-tools/run2/`（串口日志、QEMU 截图）。
- 现象：任务要求的 settings 18 块 BLKGEN 串口 dump 不存在；现存 dump 是 toolbar 7 块；`compare_geom.py` 对 settings 跑 0/7 匹配。
- 复现：
  ```bash
  grep BLKGEN artifacts/gui-tools/run2/serial-run.log
  python3 artifacts/gui-tools/run2/compare_geom.py
  ```
- 建议修法：重跑 settings 内置页 QEMU 链路，留存 BLKGEN 串口输出与同档截图；在证据报告中逐块列出 JSON rect vs 内核 rect；若存在恒定 y 偏移（toolbar 日志 y=280 vs JSON y=0），需归因并写明口径。

**P1-2　`panel` 角色映射与文档 / 内核枚举不符（对应 #6/#8 旁证）**
- 位置：`tools/blkhtml/blk2bin.py:130`（`ROLE_ENUM`）；对照 `docs/HTML-块树-转换规范.md:405,472`、`kernel_v25/include/gui/tokens.h:45`、`paint.c:144`。
- 现象：内核枚举存在 `ROLE_PANEL=3`，`paint.c` 对 `ROLE_PANEL` 返回 `tok->panel`；但工具 `color_expr('panel')` 输出 `('INHERIT_COLOR', 'ROLE_BG')`，把 panel 并入 BG。内置页里 `fill=panel` 的块会退化为 bg 色，丢掉 panel 阶梯色。
- 复现：
  ```bash
  python3 -c "import sys; sys.path.insert(0,'tools/blkhtml'); import blk2bin; print(blk2bin.color_expr('panel'))"
  # 当前输出: ('INHERIT_COLOR', 'ROLE_BG')；文档口径应为 ROLE_PANEL
  ```
- 建议修法：二选一——(a) 工具映射 `panel → ROLE_PANEL`，与文档 3.8 表对齐；(b) 若内核有意只保留 ROLE_BG，则同步修订文档 / schema 角色表并删除 `ROLE_PANEL` 路径引用。当前工具与文档两头不一致，必须收口。

### P2 建议

**P2-1　`run/REPORT.md` ⑤-4 结论过时**
- 位置：`artifacts/gui-tools/run/REPORT.md` 第五节第 4 条。
- 现象：仍称 `blk2bin --emit-c` 把角色名写成 C 字符串字面量、不能编译、「未改动 tools/blkhtml/*」；当前工具 B6 已修（角色名 → `role_*` 枚举 + `INHERIT_COLOR`，无字符串字面量），`test_emit_c_uses_role_not_string_literal` 通过。
- 建议修法：在旧报告上标注「已被 `samples/REPORT.md §3.2 B6` 收口」，避免后续读者误判工具仍带该缺陷。

**P2-2　证据目录未冻结，审计期间发生改写**
- 现象：`run2/` 内 PNG 与串口日志在审计过程中被另一轮 toolbar 运行覆盖；无可变产物校验清单。
- 建议修法：证据生成完毕后打快照（tar + sha256 清单），报告内引用快照路径；运行中不要回写同一证据文件名。

**P2-3　字体路径写死兄弟树绝对路径**
- 位置：`tools/blkhtml/blkcommon.py:136`。
- 现象：`/home/user/work/catos-os/kernel_v25/src/font_ascii.inc` 为硬编码绝对路径；当前环境存在，换机器 / 换检出路径预览即失败。
- 建议修法：优先相对仓库根解析，其次读环境变量，最后才回退候选绝对路径。

**P2-4　run2 对照产物不齐 / toolbar y 偏移未归因**
- 现象：缺 `qemu-toolbar-calm` 与对应预览；toolbar 串口 7 块 x/w/h 与 JSON 一致，但 y 恒为 280（JSON y=0/8），报告未解释该运行时偏移。
- 建议修法：补 calm 配对截图；在几何比对说明中写明 280 偏移来源（窗口定位 / 布局落点）或更正口径。

### P3 记录

**P3-1　`chrome.c` 两处既有条件行被扩展合取项**
- 位置：`kernel_v25/src/gui/chrome.c`（`gui_demo_build()` 两处守卫）。
- 现象：非纯追加行；`blkgen=0` 时条件与原逻辑等价、基线行为不变，但与「纯追加分支」的严格措辞有出入。
- 建议修法：维持现状可接受；后续评审按「兼容式守卫扩展」归类，不必回滚。

---

## 4. 与复核结果不符的子代理结论

1. **`run/REPORT.md` ⑤-4（emit-c 角色名缺陷未修）—— 复核为过时。**
   - 原话摘引：「`blk2bin --emit-c` 把 fill/fg/accent 的角色名以引号字符串写进 `uint32_t fg/bg/accent` 字段（见 `run/sample.c`：`.bg = "panel"`），不能直接编译；角色应进 `role_*` 枚举。本证据链用只读辅助脚本 `run/gen_blkgen.py` 直接消费 JSON 生成正确的 `blkgen.c`，未改动 `tools/blkhtml/*`。」
   - 复核：当前 `blk2bin.color_expr('panel') = ('INHERIT_COLOR', 'ROLE_BG')`，`emit_c` 产物不含 `"bar"` / `"panel"` 字符串字面量；单测 `TestFixB6RoleColorEnum.test_emit_c_uses_role_not_string_literal` 通过。该缺陷在工具侧已收口，报告结论停留在旧链路。

2. **`run/REPORT.md` ②「两档各 7/7 块全部一致」—— 属实但对象被误置。**
   - 原话摘引：「两档（app / calm）各 7/7 块坐标、尺寸全部一致」。
   - 复核：7/7 一致成立，但对照对象是 `sample.blk.json`（7 块）；任务书指定的 `settings.blk.json`（18 块）在 run2 证据中没有对应 BLKGEN dump。报告未区分两套 fixture，易被读成 settings 已通过内核几何对照。

3. **CE 路线 verdict（无矛盾，记录在案）。**
   - `run/REPORT.md` ③ 称「raw rc=-39 (E_DECL_BADREC)；proj rc=0，n=7，leaked=0」；`serial-ce.log` 实测 `decl reject code=-39`、`decl commit n=7 ok=1`、`decl leaked=0`，且 `extapp/blkgen.c` raw 批次含 kind=12（button），与 `decl.c:176` 的 `COLUMN..CANVAS` 区间拒绝一致。此项断言有日志支撑，复核通过。

4. **samples 报告 CE FAIL 计数（无矛盾，记录在案）。**
   - `samples/REPORT.md` §1 表：settings `--ce` FAIL=1、startmenu 17、notepad 2、toolbar 6、fullbleed 0；审计方逐文件 grep `^FAIL` 剔除汇总行后计数吻合。

---

## 5. 下一任维护者会被坑的三个点

1. **`--ce` 白名单与内核 `decl.c validate()` 区间强耦合。**
   工具侧 `validate.py :: CE_KIND_OK`（column/row/stack/text/rect/image/surface/spacer/divider/canvas）必须与内核 `decl.c:176` 的 `kind < BLK_COLUMN || kind > BLK_CANVAS` 区间保持同步；内核一旦扩充可提交种类，工具白名单、文档 1.5 表、schema `ce_projection` 三处都要跟着改，否则 CE 投影校验会与内核拒绝码脱节。

2. **`grid_snap` 只在 calm 档归整 padding/gap，app 档不吸档；坐标 8pt 是生成器约束，不是内核行为。**
   `layout.c:36` 首行 `if (!gui_style_calm() || v <= 0) return v;`；不要以为内核会把像素坐标吸到 8pt 网格。样例间距之所以不用档值 2，是因为 calm 档会把 2 上吸到 4；app 档连这个归整都不做。写 UI 时 8 的倍数必须由 html2blk 产物保证，不能依赖内核兜底。

3. **`bleed` 在运行时 `block` 上，不在 `blk_spec`；CE 通道也不可表达。**
   内置页 `emit-c` 片段无法经 `blk_spec` 成员置位 bleed，建块后需按 `chrome.c` 既有模式直接写 `block.bleed`；同理容易踩的是角色色：`panel` 在内核枚举里是独立 `ROLE_PANEL`，不要想当然并入 `ROLE_BG`（见 P1-2）。排障时先分清「字段存在」「布局消费者存在」「绘制消费者存在」三件事，文档 2.5 / 9.3 已列同类陷阱。

---

## 6. 附：审计复现入口

- 独立几何 / 二进制复算脚本：`/tmp/audit_indep.py`
- B1–B7 最小反例复跑脚本：`/tmp/audit_fixes.py`
- 证据几何 / PNG 复算脚本：`/tmp/audit_evidence.py`
- 仓库内只读证据：`artifacts/gui-tools/run/`、`artifacts/gui-tools/run2/`、`artifacts/gui-tools/samples/`、`artifacts/gui-tools/v2/`

报告落盘路径：`/home/user/work/gui-tools/artifacts/gui-tools/AUDIT.md`
