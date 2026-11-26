# v2.82 审美 R1 修复说明（简要）

分支：`v282/aesthetic-r1-fixes`（起点 main `e29bf77`）。评审原始清单见
`artifacts/v282/aesthetic-r1/审美评审-R1.md`（R1 总评 2.7/5，P0 2 条 + P1 9 条）。

## 1. 逐条对应

| R1 编号 | 改动 | 落点 |
|---|---|---|
| **P0-1** shell/GUI 主题不同源、实时不刷新 | `cdeconf_save_theme()` 写盘同时刷新内存 scheme；新增 `gui_theme_sync_scheme()` 导出（light→TOK_LIGHT 否则 TOK_DARK，`gui_theme_set` + 整树标脏 + 强制下一帧）；`cmd_theme()` 保存后调用 | `src/cdeconf.c`、`src/gui/chrome.c`、`include/gui/gui.h`、`src/shell.c` |
| **P0-2** 重启后 shell 报 light 但画布暗底 | 新增 `con_restamp_roles(old,new,n)`：按角色把存量单元格 fg/bg 旧色映射到新色；`pal_apply_scheme()` 先快照旧 RGB，应用新方案后、重绘前重印 | `src/render.c`、`include/render.h`、`src/palette.c` |
| **P1-1** 任务栏时钟未右对齐 | 任务栏行改 `ALN_SPACE_BETWEEN`、左右各 8px padding，Start 最左、时钟钉右缘 | `src/gui/chrome.c` |
| **P1-3** help 描述 80 列截断/折行不齐 | `help_row()` ASCII 分支按词折行（描述列 26、宽 54），续行缩进 26 列；超长 token 硬切 | `src/shell.c` |
| **P1-4** help 标题与 `uname` 硬编码 v0.3 | 新增单一来源 `CATOS_VERSION "2.82"`，help 标题 / about Version / `cmd_uname()` 中英两串统一引用；`lang.c` 键同步 | `src/shell.c`、`src/lang.c` |
| **P1-6** 切 light 后旧行亮绿前景未重刷 | 与 P0-2 同一 `con_restamp_roles` 方案覆盖 | 同上 |

## 2. 验证（命令 + 真实输出）

```
$ cd kernel_v25 && sh build.sh iso                 -> RC=0, kernel.bin 1,541,564 B
$ python3 tools/check_link_layout.py build/kernel.pe --strict -> RC=0

$ python3 tests/theme_sync_probe.py kernel_v25/catos.iso
PASS P0-1a gui after theme dark is dark            dominant=#1E1E26 lum=30
PASS P0-1b gui after theme light is light (no reboot) dominant=#ECECF1 lum=236
PASS cde.conf persisted theme=light
PASS P0-2 console canvas light after reboot        dominant=#F0F7FF lum=245
PASS P0-2 shell reports light after reboot         light
n/a  P0-2 text console colour                      dominant=#080F17 lum=13 (16-colour VGA text backend)
PASS version banner v2.82
PASS help renders without truncation artefact
steps=7 ok=7 fail=0
```

`n/a` 那条的理由：VGA 文本后端是固定 16 色模式，无法表达任意配色方案，
深底不是回归；只要求控制台报 `light` 且可用。

回归（同一次构建）：
- `tests/cdeconf_probe.py`：7/7 PASS
- `tests/e3_regress.py`：2/2 PASS
- `tests/vprobe_regress.py`：10/10 PASS
- `check_link_layout.py --strict`：RC=0

## 3. 截图

`artifacts/v282/aesthetic-r1-fixes/`：
- `01-gui-after-theme-dark.png` / `02-gui-after-theme-light.png` —— P0-1 前后对照（同一会话内实时切换，不重启）
- `03-console-after-reboot.png` —— P0-2 冷启动后的 gfx 画布（浅色）
- `04-text-console-light.png` —— 文本后端（16 色，颜色不可表达，仅存证）
- `05-help-wrapped.png` —— P1-3 折行效果
- `boot1-cdeconf.txt`、`boot2-theme.txt`、`boot2-about.txt`
- 修复前对照：`artifacts/v282/aesthetic-r1/13-shell-light.png`、`14-desktop-light.png`、`16-after-reboot-shell.png`

## 4. 本分支未做的 R1 项（如实登记，不算已修）

| R1 编号 | 状态 | 理由 |
|---|---|---|
| P1-2 窗口缺标题钮、文件夹无图标、常驻灰底选中框 | 未做 | 属块树新增节点与图标资产工作，超出「主题同步」这条线的范围；登记为已知项，下一版随桌面外壳推进 |
| P1-5 `echo -c COLOR` 被 `cli_validate` 拒绝但 help 宣传 | 未改（已定位） | `posix.c` 的 echo SPECS 短选项表为空、`cli_validate` 拒 `-c`，而 `shell.c cmd_echo()` 实现了它。属命令契约问题，二选一（SPECS 登记 `-c/-n` 或 help 撤下）需产品决定 |
| P1-7 字模 `|` 与 `l` 混淆 | 未做 | 8x16 位图字库的取舍，改字模会影响全部文本界面；登记为已知项 |
| P1-8 中文报错串半翻译中英杂糅 | 未做 | 属汉化表 B5 的后续（外置语言表），本轮不动 |
| P1-9 gfx 演示帧样式行叠压、单词中途折行 | 未做 | `gfxdemo` 展示帧的排版，非主界面；登记为已知项 |
| P2/P3（ls 多列、about 首行裁切、诊断残留、空态文案、light 边框对比、solarized/mono 无 GUI token、圆角/阴影/动效、图标占位、缩进风格、后端画幅混用） | 未做 | 按本轮口径「P1 允许修了或如实登记」，这些属可选优化，不影响 P0 判定 |
