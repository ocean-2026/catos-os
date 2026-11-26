# P1-3 · B7 text 模式中文降级（#117）— 作业记录（v2.82）

分支：`v282/p1-2-3-shell-text`，基线 `fbab3a7`。

## 1. 现象（基线复现）

文本模式下 `lang zh` 后，中文标签「语言已设为 」整段被画成空白单元格：

```
root@catos:/# lang zh
             zh
```

字库表（`font_hz.inc`）里这些汉字（语 U+8BED / 言 U+8A00 / 已 U+5DF2 /
设 U+8BBE / 为 U+4E3A）都在表内，但 text 后端的动态字体槽路径在实机上
没有把字形刷进 plane-2，宽字符落在空白槽位上，表现为静默吞字、不留 `?`、
不换行。窄字符路径（`be_text.c` 原 162-165 行）的
`cp >= 0x20 && cp <= 0x7F` 过滤把所有非 ASCII 一律画成空格，是直接嫌疑点。

## 2. 改了什么（仅 `kernel_v25/src/be_text.c`）

- `text_draw_cell()` 文本后端降级规则：
  - **宽字符（`CELL_F_WIDE_L`）**：每个非 ASCII 汉字渲染**一个** `?`，
    右侧搭档单元格（`WIDE_R`）留空——不画第二个 `?`。宽度口径沿用既有
    宽度表（CJK 占 2 格），只是字形降级为单 `?`。
  - **窄字符**：`cp==0` / 控制码（`<0x20`）仍画空格（保留 #103 退格不留
    残影的修复）；可打印 ASCII 原样；其余 `cp>0x7F` 渲染 `?`，不再吞成
    空格、不掉行。
- 新增 `text_note_cjk()`：每个 text 会话**首次**遇到非 ASCII 时打印一行
  提示 `note: 切 gfx 模式可显示中文 (switch to gfx mode to display CJK)`；
  静态标志位在 `text_enter()` 复位，同一会话不重复打印。
- GFX / VBE / VBE24 后端一行未动；`render.c` 宽度表、UTF-8 解码、单元
  缓冲均未改。

> 备注：`text_load_hz()` / `loaded_cp[]` 槽位缓存保留在文件内但 text 降级
> 路径不再调用（与本树既有的若干 unused 静态函数同性质，构建 -Wall 不
> 报 RC 失败）；后续若恢复 text 动态槽字形，可直接复用。

## 3. 验证命令与真实输出

构建 / 护栏（本次复核实跑，同一工作树）：

```
cd kernel_v25 && sh build.sh iso  -> RC=0, kernel.bin 1,533,252 B, catos.iso 12,259,328 B
python3 tools/check_link_layout.py build/kernel.pe --strict -> RC=0
```

P1-3 回归（`tests/p13_text_cjk_regress.py`，文本模式实机，端口 4474）：

```
PASS hint line emitted on first non-ASCII hints=['  note: ?  gfx ? ? ? ? ? ? ?  (switch to gfx mode to display CJK)']
PASS 5 hanzi degrade to 5 '?'             ? count=13
PASS locale value still printed (line kept) zh present
PASS hint emitted only once per session   hints after clear=[]
PASS second run still shows '?' for hanzi ? count=5
PASS switched to gfx mode                 gfx frame captured

steps=6 ok=6 fail=0
P13_RC=0
```

字宽回归（`tests/cjk_width_regress.py`，GFX/VBE 实机，800x600）：**脚本与现行 shell 存在既存陈旧漂移，未能跑到像素断言**（非本次改动引入，详见 §4 / §5）。本次 GFX 路径中文渲染由 `03-gfx-welcome-unchanged.png` 目检覆盖：金色「中文显示测试：你好，世界！」真实字形、CJK 占两 ASCII 格。

连带门禁（同一 ISO 复跑）：

```
python3 tests/p12_pipe_regress.py            -> steps=7 ok=7 fail=0, P12_RC=0
python3 tests/e3_regress.py kernel_v25/catos.iso  -> steps=2 ok=2 fail=0, E3_RC=0
python3 tests/vprobe_regress.py kernel_v25/catos.iso -> steps=10 ok=10 fail=0, VPROBE_RC=0
```

实机抓屏（`artifacts/v282/p1-3/`）：

- `01-text-lang-zh-question.png` — text 模式 `lang zh`：5 个汉字各一个 `?`，
  提示行出现一次，`zh` 取值行保留。
- `02-text-lang-zh-again-no-repeat.png` — 第二次 `lang zh`：提示行不重复，
  汉字仍为 `?`。
- `03-gfx-welcome-unchanged.png` — 切到 gfx 后欢迎行
  「中文显示测试：你好，世界！」仍以真实金色字形渲染，GFX 行为不变。

## 4. 未完成 / 降级项

- 提示行本身含汉字，在 text 模式下按同一规则降级为 `?`；ASCII 部分
  （`note: gfx ... switch to gfx mode to display CJK`）保持可读。
- text 模式动态字体槽路径未修复（本轮按验收口径直接降级，不恢复槽字形）；
  真实汉字显示仍需切 gfx/VBE。
- 组合字符 / 零宽字符未单独建模：前端宽度表未单列，按既有 UTF-8 解码
  口径处理，未额外占用单元格。
- `tests/cjk_width_regress.py`（v2.21 快照脚本）与现行 shell/启动默认存在三处
  既存漂移，本次未改脚本（超出 P1-3 只动 `be_text.c` 与提示接线的范围）：
  1. 启动默认已是 gfx24（v2.56 起），脚本 `boot()` 后未调 `force_text()`，
     `wait_for_prompt()` 读 0xB8000 文本缓冲，gfx24 后端不镜像该缓冲，直接
     报 `FAIL shell prompt never appeared`。同目录 `e3/vprobe/p12` 脚本均已在
     boot 后补 `force_text(s)`，唯独本脚本漏改。
  2. 脚本键入裸 `vbe`，现行 shell 分发只认 `gfx`，VBE 目标是子命令
     `gfx vbe`；裸 `vbe` 落到未知命令，屏幕停留文本 640x400。
  3. 即便切到 VBE，金色/绿色标尺 banner（「CatOS VBE 800x600x32…」
     「16.7M 色…」）只在 `gfx` 无参轮询分支打印；`gfx vbe` 子命令分支只
     打印模式切换提示，不打 banner，像素标尺行不在屏上。
  临时本地补丁（`force_text` + `gfx vbe`）后可进入 800x600，但标尺行仍缺，
  故像素断言未跑成。本次 P1-3 改动只碰 text 后端 `draw_cell`，未触达
  `render.c` 宽度表与 GFX/VBE 后端；GFX 中文渲染由 p13 抓屏目检确认。

## 5. 新发现的内核限制 / 坑

- text 后端的 plane-2 动态槽字形在当前实机路径上不可靠：字库表内有字形
  也可能落到空白槽。诊断 text 中文问题时不要先怀疑字库缺字，先看槽位
  装载/重绘路径。
- 后端 `draw_cell` 内可一次性调用 `con_write` 发提示：标志位防重入，
  嵌套 `render_flush` 不会重复触发。
- （测试债）`cjk_width_regress.py` 停留在 v2.21 命令语法：需要补
  `force_text(s)`、把 `vbe` 改 `gfx vbe`，并确认 VBE banner 的打印路径；
  否则该像素级字宽断言在 v2.56 之后的启动默认下一直是红的。
