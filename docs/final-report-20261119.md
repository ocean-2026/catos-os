# CatOS 跨夜交付报告（2026-11-18 19:46 → 2026-11-19）

版本：**v2.56 → v2.59**（本夜新增 6 块能力），基线 `aae8640`（152/152 全绿），收官 HEAD 见文末版本链。
不在本轮范围：对抗测试（红/白/蓝队）按用户指令全停，精力全部投功能交付。

---

## 1. 六块功能对照

| 块 | 版本 | 落地内容 | 关键验收 |
|---|---|---|---|
| **A 开机图形模式** | v2.56 | `/etc/videomode` 配置（`text\|gfx\|vbe\|gfx24`，默认 **gfx24** 800×600×24），缺失/空/未知/超长/二进制一律容错回落默认；VBE 不可用时安全回退文本并分级打印原因（banner 后补打防滚出）；运行时 `text` 命令的物理 Y 同意门不变 | `videomode_regress` **9/9**（默认 gfx24 / text / gfx / vbe / 4 类畸形值 / cirrus 回退） |
| **B 鼠标与滚动** | v2.57 | 8042 aux + IRQ12、3/4 字节包重组、共享 `ps2_drain` 按 status bit5 分流键盘/鼠标；gfx24 12×12 箭头软件光标 + 脏矩形；左键拖选→内核剪贴板（4096B UTF-8）、中键粘贴、右键取消、`clip` 命令；**逐行增量滚动**：单行位移 + 栈顶补行（memmove 行，不整屏重画），PgUp/PgDn = `con_rows()-1` 次单行循环；text 模式同样生效 | `mouse_regress` **18/18**（移动/三键/行步长 ±1/页步长 24/逐行位移比对/边界/选择→clip/中键粘贴） |
| **C 键位命令** | v2.58 | 集中键位表 `keys_table.c` 为唯一描述源，`keys`/`shortcuts` 从代码派生分组输出（编辑/滚动/历史/输入法/控制台/文件），`docs/快捷键.md` 同步 | 实机 `keys` 截图；`docs/快捷键.md` 与命令输出一致 |
| **D 中文输入法（阶段一）** | v2.58 | F2 开关；拼音串**内联在输入处**；候选区固定**最底两行**（倒数第二行整行横线、最后一行 `1.中 2.种 …`，按宽度自适应、整数算法不截半字）；`-`/`=`/`[`/`]` 与左右翻页；Esc 取消、空格/1-9 选字、Enter 上屏原串、Backspace 删拼音；text 与 gfx24 双模式；状态栏两行逐字保存/恢复（切换后端/清屏/改字号同步失效） | `ime_regress` **18/18**（text + gfx24 各一轮：F2、内联、候选、翻页、选字、Esc 无残影、原串上屏） |
| **E 崩溃屏强化** | v2.59 | 纯 `vga_*` facade 自包含绘制（不触堆/FS/shell/IME/字体加载）；**COM1→VGA 双输出**（LSR.THRE 轮询，超时丢弃不阻塞）；**二次异常护栏**（`g_in_panic` + 深度上限 2，嵌套直接 `cli;hlt`）；分级表 `docs/panic-tiers.md`（L0 ring3 杀任务 / L1 受控终止 / L2 ring0 全屏）；摘要页（向量+十六进制/CPU/FATAL/RECOVER/ERR/EIP/CR2/RSP/EFLAGS/栈帧符号化/倒计时）；单键 `R/D/S/M/P/A/C`；bss panic record + `crashlog` / `crashlog --full`；`/etc/panicmode` 支持 `reboot|halt|wait` 与 `reboot:<Ns>` | `panic_regress` **19/19**（含串口内容断言、D/S/M/P/A 翻页、C 拒绝、ring3 隔离、crashlog 无记录、halt 留屏） |
| **F 坏 FAT 目录项净化** | v2.59 | `fat_name_sane()`/`fat_83_sane()`/`fat_lfn_str_sane()` 统一判定；`ls`/`tree`/TAB 补全/`fs_enum_*` 全部走 `fat_list_dir_view()`，非法项默认隐藏并汇总 `N hidden invalid entries (use ls --raw to show)`；`ls --raw`/`tree --raw` 打印 `c=<cluster> o=<offset> first=<hex> raw=<11B>`；`fsck` 递归扫描（深度 6）分类 bad-name/orphan-lfn/bad-cluster，默认只报告、`--purge` 才墓碑化；0x05→0xE5 转义特例补齐 | `fs_badname_regress` **23/23**（四类坏项离线注入 + 默认隐藏计数 + --raw 字段 + fsck 分类 + purge 后 clean） |
| **G 通用分页器** | v2.59 | `-- More` 分页核心（`pager.c`）：`help`/`cat`/`tree`/`ls`（含 `ls -l`）接入；Enter 前进一行、Space 前进一屏、q 退出；重定向/后台/管道/非 tty 直通不阻塞 | `pager_regress`（help 超屏 → More → Enter 逐行 → Space 整屏 → q 退出；0/1 行/正好一屏/超一屏边界；重定向无 More 且立即返回） |

---

## 2. 版本链（`aae8640..HEAD`，40 个 commit，按时间顺序）

```
32c0472 feat(kernel): v2.59 self-contained BSOD module (bsod.c/bsod.h) + build wiring
74fe044 feat(kernel): route panic() and ring0 fatal faults to the BSOD; ring3 unchanged
83a6889 feat(shell): bsod/blue + panicmode builtins; cache /etc/panicmode at boot
24e863c test: panic_regress.py 11-step crash-screen sweep, wired into full runner
8ce7083 docs: v2.59 crash screen progress, history/experience, issue #194/#195
ded0d60 docs: IME 规划(ime-plan.md 464 行) 与系统内测试文档
4ff5060 merge: v2.59-crashscreen 蓝屏崩溃屏(bsod/pnicol/pnicomode/panic_regress)
4910806 fat: bad-directory-entry purification, --raw diagnostics, fsck tree scan, 0x05 escape
194b546 tests: fs_badname_regress 23/23 -- offline injection of four bad entry classes
44c3c75 docs: register #194-#197 bad-entry purification/fsck/0x05 escape
8d42951 merge: 坏 FAT 目录项净化 + ls --raw + fsck（问题清单顺延 #198-#201）
895a7c2 kernel(v2.56): boot display mode via /etc/videomode, default gfx24, safe text fallback
176207f tests(v2.56): force_text driver helper for legacy text-screen suites
3217e51 tests(v2.56): videomode_regress.py five-case boot mode matrix + docs
cbf77c3 merge: v2.56 开机默认 gfx24 + /etc/videomode
8d315c0 feat(kernel) P0-1/P0-2: panic dual-output COM1->VGA + second-fault hard halt guard
d17c5f6 docs: panic-tiers.md 异常分级表
bfb08ce feat(kernel) P0-4: deps audit + ASCII title fallback (#202)
057f0d0 feat(kernel) P1-5/6/7: summary fields, single-key pages, panic record + crashlog
e120737 test(regress): panic_regress 19 steps; force_text for gfx24 text drivers
b38f8d2 feat(kernel): soft-exit guard reset + P2-9 inverse title, narrow view, SMP placeholders, wait mode
9c74bdb docs: issue list #203-#207, PROGRESS §37
a4e0dd5 docs(PROGRESS): same-ISO regression tally
31eb5bd v2.57: PS/2 mouse + IRQ12, gfx24 software cursor, selection/clipboard, incremental scrollback
41e5026 docs(v2.57): key semantics + history/experience
ef18e4d merge: v2.59 崩溃屏 P0/P1/P2 强化
17b69ee merge: v2.57 鼠标/选择/剪贴板/逐行滚动
614b396 test(v2.57): mouse_regress 加 force_text（18/18 PASS）
1e72d36 feat(v2.58): keys/shortcuts builtin from centralized key-binding table
2c0e93e feat(v2.58): two-row fixed IME candidate area with verbatim save/restore
885221f feat(v2.58): stage-1 pinyin IME engine with inline composition
8c29fe1 test(v2.58): IME stage-1 regression, text + gfx24
0b27f17 docs(v2.58): key-binding legend + IME coverage notes
0fe1d03 docs(v2.57): 开机耗时打点表
1f84a69 test/docs: run_full_regress 加 flock + 固定端口表 + 基线豁免登记
6193adf merge: v2.58 keys + IME 两行候选区
cb816c5 feat(v2.59): generic console pager core (pager.c)
2632fb7 feat(v2.59): help/cat/tree pager wiring + ring0 builtin stdout tee
5d333e1 feat(v2.59): ls / ls -l paged listing
d5df1b1 test(v2.59): pager_regress.py
074933b merge: v2.59 通用分页器
```

推送记录（内嵌 token，输出已脱敏）：
```
aae8640..8d42951  main -> main
8d42951..cbf77c3  main -> main
cbf77c3..614b396  main -> main
614b396..1f84a69  main -> main
1f84a69..6193adf  main -> main
6193adf..074933b  main -> main
```
远端校验：`git ls-remote origin main` 与本地 HEAD 一致（逐次核对）。

---

## 3. 回归台账

基线 14 套件（152 checks）：qemu 12/12、shell_guess 16/16、fsrec 12/12、lfn 14/14、recur 10/10、jobctl 12/12、group 10/10、daemon 5/5、caps 12/12、user 20/20、disk 5/5、env 13/13、heap 4/4、size 7/7。

本夜新增套件：`videomode` 9/9、`fs_badname` 23/23、`mouse` 18/18、`panic` 19/19、`pager`（见终验日志）、`ime` 18/18、`diag` 4/4。

合计：**152 + 9 + 23 + 18 + 19 + pager + 18 + 4**（终验数字见 `tests/regress_logs/final_*.log`）。

基线豁免（与本轮代码无关，详见 `docs/regression-ports.md` §2）：
- `gfx24_regress` 1 步：`/bin/gfx24.ce installed`（39 条 `ls` 溢出 25 行窗口，基线同样 FAIL）
- `svga24_regress` 2 步：旧主题色 `(16,32,64)` vs 现 `(12,12,12)`（断言过期）
- `ce_regress` 11 步：`ce_sim` 未建模 opcode（宿主侧解释器既有缺口）
- `size_regress` large 帧偶发：端口争用/既有 screendump 时序，固定端口串行后稳定

纪律：所有 sweep 串行、端口固定（见 `docs/regression-ports.md`），`run_full_regress.sh` 用 `flock` 互斥。

---

## 4. 问题清单状态（`docs/CatOS-内核问题清单.md`）

| 编号 | 摘要 | 状态 |
|---|---|---|
| #194 | BSOD TSC 超时倍率算错千倍（30 s → 30 ms） | ✅ 已修 |
| #195 | 崩溃屏直写 0xB8000 被前端 redraw 覆盖 | ✅ 已修（纯前端绘制） |
| #198 | 坏目录项在 ls/tree 显示乱码 | ✅ 已修（统一净化 + 隐藏汇总） |
| #199 | 无目录树自检手段 | ✅ 已修（`fsck` 分类报告） |
| #200 | 递归扫描复用静态簇缓冲导致遍历中断 | ✅ 已修（按深度分缓冲） |
| #201 | 首字节 0x05 转义特例未处理 | ✅ 已修 |
| #202 | 崩溃屏中文标题缺字模（子集不含"严/重/问/新/启/动"） | ✅ 已处理（降级 ASCII + 字模证据链） |
| #203 | 软退出未复位护栏标志，第二次蓝屏被误判嵌套 | ✅ 已修 |
| #204 | 崩溃屏符号化缺内建符号表 | ✅ 已落地（`tools/gen_symtab.py` + 二分查表） |
| #205 | panic 路径不得写 FS | ✅ 已落地（bss panic record + 下次启动 `crashlog`）；固定扇区转储 deferred |
| #206 | gfx24 下旧文本套件读屏为空 | ✅ 已修（驱动 `force_text()`） |
| #207 | SMP 未接入但需预留崩溃屏接口 | ⏳ deferred（接口占位已就位） |

累积 deferred（不属本轮范围）：#188 TID 代际、#189 守护直写控制台、#190 组件名超长、#191 `n` 否定语义、#192 管道句柄无属主、#193 跨会话任务枚举，以及取消恒等映射 / SMP / 内存动态化三项重写级设计。

---

## 5. 已知限制

1. **IME 仅阶段一**：单字、无词组/联想/模糊音；词库为 GB2312 子集（6763 字 / 400 音节），覆盖范围见 `docs/快捷键.md`。词组、联想、模糊音在 `docs/ime-plan.md` 阶段二/三。
2. **SMP 未启用**：崩溃屏/调度接口已留位（per-CPU 缓冲、停核标记、`[CPU n]` 前缀），实现 deferred。
3. **text 后端 CJK 缺字**：内建 GB2312 子集外的汉字落 `?` 兜底；崩溃屏因此用 ASCII 标题。
4. **滚轮 dz 通道**：QEMU 6.2 板载 ps2mouse 不转发 dz，内核 4 字节解析与 ±3 行映射已实现，guest 端覆盖 deferred。
5. **图形模式文本平面不可读**：dispi/LFB 下 0xB8000 不是可读镜像（`docs/problem.md` #50），旧文本套件靠 `force_text()` 切回文本后端验证。

---

## 6. 工程基建（本轮新增）

- `docs/regression-ports.md`：固定端口分配表 + 基线豁免登记（终验判绿依据）。
- `tests/run_full_regress.sh`：`flock` 互斥 + 串行 + 固定端口；新增 7 个套件接入（videomode/badname/mouse/panic/pager/ime/diag）。
- `docs/boot-timing.md`：K0–S9 打点，冷启动 ≤120 ms，唯一 >10 ms 阶段是 CPU 校准（有意等待，不做优化）。
- `docs/panic-tiers.md`：L0/L1/L2 分级表与代码分支逐一对应。
- `docs/快捷键.md`：与 `keys` 命令同源的键位说明。
- `docs/ime-plan.md`：输入法三阶段设计（本轮落阶段一）。
