# 控制台"错误换行 / 仅行偏移"排查与修复（fix/con-wrap）

基线：`66264e2`（origin/main，含 IME 修复）。工作树：`/home/user/work/catos-conwrap`。

## 1. 现象与用户的定性

用户在 VMware 里跑 `curl https://baidu.com`（301，57 字节）和第二条请求后，屏幕上的文字
出现字块错位：`curl` 被画成 `wurl`，`Moved Permanent` 缺 `ly`、`ly` 出现在上一行右端，
`locati` 与 `.replace(...)` 之间多出几个空格缺口，`<m` / `eta` 分处两行。

用户纠正了第一版"字形被啃（丢字）"的判断：**不是丢字，是错误的换行——第 n 行第 m 列的
字符可能跑到第 n+1 行第 m 列，而且只发生行偏移，列不变**。

这一句把方向从"字体/字形槽"改成了"写屏行游标 + 滚动记账"。

## 2. 可控复现装置（tests/console_row_offset_probe.py）

判定需要把三件事分开：①写屏/折行引擎对不对；②网格（cells）对不对；③像素（LFB）对不对。
屏幕截图只能看到 ③，所以做了一个能同时约束 ①+②+③ 的探针：

1. 宿主起 HTTP 服务（slirp，guest 侧 `10.0.2.2:8099`），提供 8 组页面：
   - `c0` 百度 301 页原样文本（就是出事那页的形状）；
   - `c1` 同一内容换成 CRLF 行尾；
   - `c2` 定长行 79/80/81/99/100/101/120/160/200（折行边界逐列逼近）；
   - `c3` 一条超长段落（只靠自动折行，没有任何 `\n`）；
   - `c4` 60 行输出（强制多次整屏滚动）；
   - `c5` 含 Tab 的行；
   - `c6` 含空行/纯空格行；
   - `c7` 40 条 100 字长行（每行必折一次，跨滚动边界）。
2. guest 在**默认 gfx24 + `-vga vmware`**（与用户同一条 VMware SVGA 代码路径）下逐组
   `clear` + `curl`，各抓两张 screendump（间隔 1.6 s，用来判断"画面是否已经稳定"）。
3. 用 `kernel_v25/src/font_ascii.inc` 里的 IBM VGA 8x16 字形表，把 100×37 个格子
   **逐格 OCR 回字符**（先取格内众数色当底色，再按 7×15 inked box 做位图匹配）。
4. 用 Python 复刻 render.c 的布局规则（行首缩进 2 空格 → 按 `cols` 折行 → 触底滚动），
   生成期望网格，再与 OCR 结果逐行逐字比对；同时校验两张截图是否一致（settled）。

探针不依赖网络外部资源，可重复执行：

```
python3 tests/console_row_offset_probe.py kernel_v25/catos.iso [--vga vmware|std]
```

## 3. 结果

| ISO | c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7 | settled |
|-----|----|----|----|----|----|----|----|----|---------|
| 基线 `66264e2` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **1** | 全部 True |
| 修复版 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 全部 True |

- **折行/缩进/滚动引擎本身是对的**：79/80/81/99/100/101/120/160/200 这些边界行，
  每行折点、续行不缩进、滚动后残行全部与模拟一致；60 行滚动、CRLF、Tab、空行也都一致。
- 基线唯一一处不一致：`c7` 第 14 行 ` X`（列 0 的 `X` 没了，列 1 还在）——位置在
  一次滚动之后的续行。这是"同一列少一格"的最小样本，不是整段错位，量级只有 1 格。
- 这说明用户在 VMware 上看到的整段错位，**不是布局算法算错**，而是像素层（或滚动时
  像素搬运的结果）与网格/脏格缓存（`painted[][]`）不匹配：网格里那一格是对的，所以
  `paint()` 的 dirty-cell skip 一直跳过它，错误像素就永久留在屏幕上。

## 4. 修复：命令边界的整屏自愈（render.c / keyboard.c）

```c
/* render.c */
static uint32_t s_scroll_events;
void con_resync_frame(void) {
    if (!be || s_scroll_events == 0) return;
    s_scroll_events = 0;
    repaint_all();          /* write-only：不调 be->clear()，不会闪 */
}
```

- `scroll_one()` 末尾 `s_scroll_events++`；
- `kb_readline()` 一进 prompt 就调用 `con_resync_frame()`：本命令期间只要滚过，就按
  cell 网格把整屏重画一遍，把任何"没落到正确像素位置"的滚动结果覆盖掉。
- 代价：每条命令一次 3700 格的写-清重绘（无 `clear`、无闪），量级与一帧相当。

这条修的是**用户看得见的那一刻**：命令跑完、停在 prompt 上的画面一定是网格的真值。
它修不到"命令输出进行中"的错位（那时画面还在动），也掩盖不了真因——真因还要在
VMware 现场证明：把 `painted[][]` 与 LFB 实际像素逐格比对（像素级自检），若不一致就是
滚动像素搬运/宿主同步的问题，而不是布局。

## 5. 回归

- `tests/console_row_offset_probe.py`：修复版 8/8 cases，diffs=0，settled 全 True。
- `tests/ime_autotoggle_probe.py`：ALL PASS（IME 三场景未被本改动破坏）。
- 门禁（vprobe 10/10、E3、F11、`ime_regress.py`）留到 v2.80 合并态统一跑。

## 6. 证据

- `artifacts/con-wrap/console-probe-baseline.log`（基线 ISO，8 组）
- `artifacts/con-wrap/console-probe-fixed.log`（修复 ISO，8 组全绿）
- `artifacts/con-wrap/ime-autotoggle-after.log`（IME 探针 ALL PASS）
- `artifacts/con-wrap/*.ppm`（各组两次截图原始帧，可直接复核 OCR 结论）

## 7. 未解决 / 下一步

1. **像素级自检**：加一个诊断命令，把每个格子的 LFB 实际像素与网格应有的字形位图比对，
   报出不一致的格子数与坐标。在用户的 VMware 上跑一次即可判定"错位发生在像素层还是网格层"。
2. **滚动像素搬运的接管面**：`v24_scroll()` 直接 memcpy LFB；`mc_save/mc_restore` 的
   12×12 精灵保存/恢复也在同一块内存上，两者的先后顺序在"滚动 + 鼠标"同时发生时没有
   显式互斥；`painted[][]` 不知道这些像素写入。若自检证实不一致，优先在这一处加互斥/重新标脏。
3. `curl` 不跟随 3xx（含 https→http 降级）是另一个独立缺陷，不在本轮范围。
