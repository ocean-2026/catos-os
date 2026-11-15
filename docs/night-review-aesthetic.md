# CatOS 双审闭环 · 第二审：终端界面审美审查报告

- 审查轮次：跨夜迭代双审闭环第二审（审美审）
- 基线 HEAD：`2c47238`（v2.44）；整改后版本 v2.45，commit `e839830`（已推 GitHub main）
- 审查方式：QEMU i386 实机（`-m 256`、纯 PS/2、`-display none`、monitor `screendump` → PNG），逐项截图 + 屏幕文本双取证，逐张 Read 核验
- 对比基准：Windows Terminal Campbell 主题（底 `#0C0C0C` / 正文 `#DCDCDC` / 蓝 `#60A5FA` / 绿 `#7EE787` / 红 `#F85149` / 黄 `#E3B341`）、现代发行版默认终端 + eza/lsd 排版（目录排序、列对齐、目录着色、右对齐尺寸列）、oh-my-zsh 提示符配色
- 取证脚本：`tests/aesthetic_probe.py`；截图目录 `tests/qemu_shots_aesthetic/`；文本取证 `tests/audit_logs/aesthetic_round1.txt` / `aesthetic_round2.txt`

## 一、逐项评分表（第一轮 · 基线 v2.44）

评分档：✅ 认可 / 🟡 可接受有瑕疵 / ❌ 不认可需整改。

| # | 审查对象 | 对比基准 | 第一轮现状（实机取证） | 结论 |
|---|---------|---------|----------------------|------|
| 1 | 开机横幅 / `about` / PS1 提示符 | oh-my-zsh：用户@主机 绿、路径蓝、权限标记另色 | 提示符整段青色；`#` 标记继承路径蓝；`about` 七行 ASCII 横幅顶部四行被滚出网格 | 🟡 |
| 2 | `ls` 目录列表 | eza/lsd：目录排序、目录蓝/文件灰、尺寸列对齐 | 条目按 FAT 原始顺序（node.ce, test.js, ocntools…）；`[DIR ]` 标签与目录名同色；`[FILE]` 标签与文件名同色；尺寸 `(N bytes)` 括号前后不对齐 | ❌ |
| 3 | `help` 命令表 | 现代终端帮助：分组标题着色、命令列/描述列固定宽对齐 | 命令列 24、描述列 26 对齐本身良好；分组标题 `-- ... --` 为默认白，无层级区分；顶部行受 25 行网格限制不可回看（#141 已知限制） | 🟡 |
| 4 | `df` 挂载表 / `mount` 列表 | GNU df：数值列右对齐、表头与数字同列 | total/used/free 左对齐后接 `K`，长短数字起始列参差，与表头错位；mount 设备名宽度未与 df 对齐 | ❌ |
| 5 | `pkg list` / `pkg info` | 包管理器表格：名称/版本/状态/描述分列对齐，状态着色 | 名称 bc(2)/stats(5)/rps(3) 列参差，`[1.0]` 版本括号不对齐；状态 available/installed 无色；info 标签冒号列基本对齐但标签/值均无色层级 | ❌ |
| 6 | `tree` 目录树 | tree(1)：`├──`/`└──` 连接器 + `│` 续行，目录着色 | 仅 `+- `/`- ` 两级缩进，无垂直续行；目录/文件均白色 | 🟡 |
| 7 | cowsay 气泡框 / `cal` / `fortune` / `sl` | cowsay 原生框线对齐；cal 标题居中；fortune 框不溢出 80 列 | cowsay 框顶/底多一个前导空格，与 `<` 行左右不对称；cal 标题左偏未居中；fortune 最长英文格言 78 字符 + 边框溢出 80 列，右竖条被裁；CJK 按字节算框宽导致边框偏短；sl 为朴素 ASCII 列车动画，完成度可接受 | ❌ |
| 8 | 错误信息语义色 | 现代 shell：错误红、提示黄、命令名高亮 | unknown command 整行红；`did you mean: help ?` 为默认灰，问号前多余空格 | 🟡 |
| 9 | `echo -c` 着色输出 | 语义四色可辨识 | red/green/blue 均可渲染，色值随 CLUT；链路正常 | ✅ |
| 10 | 整体配色一致性 | Campbell 主题底/正文/路径蓝/强调绿/语义四色 | 文本后端 DAC 仍是经典 VGA 16 色 ramp（纯黑底、`#AAAAAA` 灰正文、`#5555FF` 蓝、`#55FF55` 绿），现代 CLUT 只在 gfx/VBE 后端生效；文本与图形两套观感 | ❌ |

**第一轮结论：不认可。** 整改清单见下（每条含位置 / 现状 / 期望 / 验收）。

## 二、第一轮整改清单（v2.45）

| 编号 | 位置 | 现状 | 期望效果 | 验收标准 |
|------|------|------|---------|---------|
| F1 | `kernel_v25/src/be_text.c` `text_dac_reset` / `text_set_palette` | 文本 DAC 加载经典 0x00/0x55/0xAA/0xFF ramp；`set_palette` 空实现 | DAC 0..15 由共享现代 CLUT 编程；调色板调整实时写 DAC | 文本模式底近 `#0C0C0C`、正文 `#DCDCDC`、路径蓝 `#60A5FA`、强调绿 `#7EE787`；text↔gfx 往返后 DAC 重置正确 |
| F2 | `kernel_v25/src/render.c` `render_rgb_to_vga` | 只按 3-bit 阈值表量化，CLUT 蓝会被归入青色槽 | 先在 CLUT 0..15 做最近邻匹配，无近邻才回退阈值 ramp | 主题色在文本模式落到对应 CLUT 槽，不串色 |
| F3 | `kernel_v25/src/fs.c` `fs_list` / 列举回调 | FAT 顺序；标签与名字同色；尺寸参差 | 目录优先 + 名称大小写不敏感排序；标签深灰、目录青、文件灰；尺寸右对齐宽 8 | 截图 `ls /bin` 字母序排列；标签/名字/尺寸三层色可辨；尺寸列对齐 |
| F4 | `kernel_v25/src/fs.c` `df_row` / `fs_mount_list` | 数值左对齐 | total/used/free 宽 7 右对齐，use% 宽 3 右对齐；mount 设备宽 12 | 数字与表头纵向对齐 |
| F5 | `kernel_v25/src/shell.c` `cmd_pkg_list` / `cmd_pkg_info` | 列参差、状态无色 | 名称宽 10、版本宽 8 括号、状态宽 11（installed 绿 / available 灰）、描述；info 标签深灰、名称蓝、状态着色 | 版本括号、状态列纵向对齐 |
| F6 | `kernel_v25/src/shell.c` `tree_walk` | 无连接器/续行，目录白色 | `+-- `/`\-- ` 兄弟连接器 + 每层 `│   ` 续行前缀，目录青、文件灰 | 树缩进体现父子关系；目录着色 |
| F7 | `kernel_v25/src/shell.c` `cmd_cowsay` / `cmd_fortune` / `cmd_cal` | 框按字节算宽；cal 标题左偏；长格言溢出 | 边框按显示宽度（CJK 计 2）；cal 标题在 21 列网格居中；fortune 72 列内换行 | 框线左右对称不裁边；标题居中；CJK 边框对齐 |
| F8 | `kernel_v25/src/shell.c` 未知命令建议块 | 建议灰、`help ?` 多余空格 | 建议行黄、命令名蓝、`help?` 无空格 | 截图：黄行 + 蓝命令 + 紧贴问号 |
| F9 | `kernel_v25/src/shell.c` 提示符 | `#`/`$` 继承路径蓝 | 标记改强调绿，路径保持蓝 | 提示符三段：绿用户@主机 / 蓝路径 / 绿标记 |
| F10 | `kernel_v25/src/shell.c` `cmd_about` | 未清屏，横幅顶部被滚走 | 打印前 `vga_clear()` | 七行横幅完整入格（受 #141 回滚深度限制见遗留 L1） |
| F11 | `kernel_v25/src/shell.c` `help_head` | 分组标题默认白 | 标题亮青着色，打印后还原白 | 帮助小节标题有色层级 |

## 三、第二轮迭代记录（整改 → 实机复验）

### 3.1 已落地并复验通过（v2.45）

- **F1/F2 文本 DAC 现代化**：`text_dac_reset` 改为遍历 `render_clut(i)` 写 6-bit DAC（R/G/B >> 2）；`text_set_palette` 落地单条目实时写 DAC；`render_rgb_to_vga` 增加 CLUT 最近邻（距离阈值 `24²·3`）匹配，未命中再回退阈值 ramp。复验：`00_boot` / `19_prompt_cd` 文本底色、正文灰、路径蓝、标记绿与 gfx 观感一致。
- **F3 列表排序与分层着色**：`fs_list` 先收集到 `ls_buf[96]`，目录优先、大小写不敏感插入排序后再打印；`[DIR]`/`[FILE]` 标签深灰（`#8B949E`），目录名青、文件名灰，字节数右对齐宽 8。复验 `03_ls_root`（home…var 字母序）、`04_ls_bin`（ocntools→zhdemo 字母序）、`05_ls_long` 尺寸列右对齐。
- **F4 df/mount 对齐**：`df_put_k()` 宽 7 右对齐 + `K`，use% 宽 3；mount 设备宽 12。复验 `07_df` 数字落在表头列下，`08_mount` 设备列对齐。
- **F5 pkg 表格**：list 名称宽 10 蓝、版本宽 8 括号深灰、状态宽 11 绿/灰、描述灰；info 标签深灰、名称蓝、状态按安装态着色。复验 `09_pkg_list` / `10_pkg_info`。
- **F8 建议语义色**：建议行黄、命令名蓝、`?` 紧贴。复验 `15_unknown`：`catos: unknown command: helo` 红，`catos: did you mean: help?` 黄 + 蓝 `help`。
- **F9 提示符三段色**：用户@主机绿、路径蓝、`#`/`$` 绿。复验 `19_prompt_cd`：`root@catos:/bin#` 三段可辨。
- **F10 about 清屏**：`cmd_about` 起始 `vga_clear()`。
- **F11 帮助标题着色**：`help_head` 亮青，打印后还原白。
- **F7 部分落地**：cowsay 框线改显示宽度并左右对称；cal 标题在 21 列网格居中；fortune 边框按显示宽度计算、竖条对齐。复验 `12_cowsay` / `13_cal` / `14_fortune`。
- **F3 附带**：tree 目录名青、文件名灰（连接器方案见 3.2 回退说明）。

### 3.2 回退与遗留（第二轮复核如实记录）

- **F6 tree 连接器方案回退**：首版递归实现带 `+-- `/`\-- ` 兄弟连接器与 `│   ` 续行前缀，并加了排序；实机 `tree /` 刷屏输出同一串 `?????t?????+--` 乱码行（栈/枚举回归），后续 fortune 帧被污染。定位到当前内核 shell 栈容量与 `fs_enum_entries` 24 槽枚举下，该递归 + 排序改动不安全；**已回退到原始 `+- `/`- ` 两级缩进实现**，仅保留目录青/文件灰着色。连接器与垂直续行列入遗留 L2，待树遍历改为堆上缓冲后再做。
- **F7 fortune 自动换行回退**：首版 72 列按词换行实现把引文丢进了空框（`|                                 |`），栈上三行 `flines[3][76]` + token 缓冲组合在该环境下不可靠；**已回退单行框**，仅保留显示宽度边框修正。长格言换行列入遗留 L3。
- **F10 横幅截断未根治**：清屏调用已加，但 `about` 七行横幅顶部仍有少量行离开网格——根因是行编辑器锚定/回滚缓冲深度（#135/#141 同源），非清屏本身；列入遗留 L1。

### 3.3 回归证据（整改后实机复跑）

| 回归套件 | 结果 | 取证 |
|---------|------|------|
| `qemu_regress.py` | **12/12 PASS**（boot-shell / format / help / display / gfx / text-reject-N / text-accept-Y / ls-bin / mmaptest / hello-elf / vdiag / final-frame） | `tests/audit_logs/` 本轮运行；截图刷新于 `tests/qemu_shots/` |
| `fsrec_regress.py` | 3/3 PASS（rm-dir-needs-r / rm-root-refused / rm-r-tree-gone） | 本轮实机输出 |
| `caps_regress.py` | 3/3 PASS（tui-list / caps-view / screen-restored） | 本轮实机输出 |
| `user_regress.py` | 3/3 PASS（perms-guest / cap-denied / su-root） | 本轮实机输出 |
| `env_regress.py` | **13/13 PASS** | 本轮实机输出 |
| `heap_regress.py` | 4/4 PASS | 本轮实机输出 |
| `disk_regress.py` | **5/5 PASS**（mount / ls-bin / hello / vprobe / fsck） | `tests/audit_logs/aesthetic_regress_round2.txt` |
| `size_regress.py` | PASS（dirty-skip-grew 等） | 本轮实机输出 |
| `ce_regress.py` | 11 FAIL 为 ce_sim 既有基线（未建模 opcode），非本轮回归 | 第一审已记录 |

> 测试基建同步：`disk_regress.py` 的 `ls-bin` 断言原先依赖 FAT 序尾部条目（`node.ce`/`syn.go`），排序后尾部变为字母序末位；断言改为校验字母序末位 `zhdemo.ce` + 条目计数行（与排序意图一致，非放宽内核要求），复跑 5/5 PASS。

## 四、遗留清单（第二轮复核后仍未关闭）

| 编号 | 级别 | 遗留项 | 说明 | 建议处置 |
|------|------|--------|------|---------|
| L1 | 低 | `about` ASCII 横幅顶部行截断 | 清屏已加，残留与行编辑器锚定/回滚缓冲深度同源（#135/#141） | 加深 scrollback 或 about 前独立分页 |
| L2 | 低（审美） | tree 仍为 `+- `/`- ` 朴素缩进，无 `├──`/`└──` 与 `│` 续行 | v2.45 连接器递归方案触发栈/枚举乱码，已回退 | 树条目收集改堆上/静态大缓冲后再做连接器 |
| L3 | 低 | fortune 未按词换行；极少数 >72 显示宽度的格言右竖条仍可能贴边 | 换行实现栈上缓冲不可靠，已回退单行 | 静态缓冲 + 逐字符宽度断行 |
| L4 | 低 | 文本模式仅 16 个 CLUT 槽，渐变/中间色仍比 gfx 后端粗 | 16 色属性限制，非本轮可解 | 后续文本后端上 256 色属性 |
| L5 | 观感 | ls 文件尺寸 `(  N bytes)` 括号内留白略宽 | 右对齐宽 8 的副作用 | 后续收窄为宽 6 |

## 五、最终结论

**第二审（审美）结论：认可通过（带遗留）。**

- 对照 Windows Terminal Campbell 主题与现代发行版 eza/lsd 排版：目录排序、目录/文件分层着色、表格列对齐、提示符三段色、错误/建议语义色、包列表分列对齐、文本 DAC 现代主题色均已落地并实机复验；无粗糙感、无颜色错乱、无框线裁边的主流程界面不落伍于同类终端。
- 遗留 L1–L5 均为低级别观感/已知限制，不阻塞闭环；按上表在后续迭代处置。

### 截图路径清单（第二轮实机取证）

- `tests/qemu_shots_aesthetic/00_boot.png` — 开机横幅与初始提示符
- `tests/qemu_shots_aesthetic/02_about.png` — about 信息页（横幅截断见 L1）
- `tests/qemu_shots_aesthetic/03_ls_root.png` — 根目录排序 + 目录着色
- `tests/qemu_shots_aesthetic/04_ls_bin.png` — /bin 字母序 + 尺寸右对齐
- `tests/qemu_shots_aesthetic/05_ls_long.png` — `ls -l` 尺寸列
- `tests/qemu_shots_aesthetic/06_help.png` — 帮助表对齐 + 标题着色
- `tests/qemu_shots_aesthetic/07_df.png` — df 数值右对齐
- `tests/qemu_shots_aesthetic/08_mount.png` — mount 设备列对齐
- `tests/qemu_shots_aesthetic/09_pkg_list.png` — pkg 分列 + 状态色
- `tests/qemu_shots_aesthetic/10_pkg_info.png` — pkg info 标签/值层级
- `tests/qemu_shots_aesthetic/11_tree.png` — 目录树（朴素缩进，见 L2）
- `tests/qemu_shots_aesthetic/12_cowsay.png` — 气泡框对称
- `tests/qemu_shots_aesthetic/13_cal.png` — 月历标题居中
- `tests/qemu_shots_aesthetic/14_fortune.png` — 格言框对齐（见 L3）
- `tests/qemu_shots_aesthetic/15_unknown.png` — 错误红 / 建议黄 / 命令蓝
- `tests/qemu_shots_aesthetic/16_echo_red.png` / `17_echo_green.png` / `18_echo_blue.png` — 语义色输出
- `tests/qemu_shots_aesthetic/19_prompt_cd.png` — 提示符三段色
- `tests/qemu_shots_aesthetic/20_sl.png` — sl 列车动画帧
- `tests/qemu_shots_aesthetic/21_gfx.png` — gfx 256 色模式对照帧
