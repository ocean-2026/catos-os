# REVIEW-aes-final5 — 汉化排版审美审（final5 功能冻结后）

- 审查方：审美审代理（新代理，非实现方/功能审）
- 基线 commit：`b909716`；审查时刻 ISO 为新构建 `kernel_v25/catos.iso`（修复后重出图）
- 对照常识：Windows 开始菜单/任务栏、XFCE/LXDE 面板、macOS 菜单栏
- 范围限定：只看汉化后排版；不改文案/功能/ABI/断言/布局体系

## 结论

**认可**（基线发现 1 项 P1：FM 标题条关闭按钮左贴；本轮已按低风险排版项修复，五门禁复跑全绿，实机复核分列到位，无遗留 P0/P1）。

---

## 逐点判定

### 1) 字距与行高 —— 认可

- 开始菜单 8 行，行高 24px、行间距 2px、左 padding 4px（`chrome.c:1557-1576`），800×600 下中文 8px 宽字形疏密均匀，行与行不挤。
  - 依据：`artifacts/final5/i18n/i18n-menu.png`、`artifacts/audit5/a4-menu.png`；放大复核 `/tmp/menu.png`（菜单 8 行基线齐平、行间留白一致）。
- 终端/记事本正文固定 16px 行高，回看滚动场景行距均匀，无叠字。
  - 依据：`artifacts/audit5/a3-term-mid.png`、`a3-term-bottom.png`、`a3-note-mid.png`。

### 2) 按钮宽度是否被中文撑爆 —— 认可

- 任务栏实例按钮 `pad_l=6 / pad_r=6`（`chrome.c:1265`），最长标签 `浏览器 1`/`终端 2` 均完整容纳，无挤压/截断/换行。
  - 依据：`artifacts/audit5/a6-tb-active.png`（终端 1 / 终端 2 并排）、`a6-tb-two.png`、`artifacts/audit5/aes-fix-tb.png`。
- 浏览器工具条 `后退 / 前进 / 刷新 / 停止` 四按钮等宽排布，未被中文撑破；错误页 `重载`/`后退` 按钮贴合文字、余量足够。
  - 依据：`artifacts/final5/i18n/i18n-error.png`、`artifacts/audit5/a5-br-edit.png`、`a5-br-error.png`。
- FM 工具按钮 `..` / `刷新` 与关闭块（宽 56）容纳两字中文无截断。
  - 依据：`artifacts/audit5/ax-mw-fm.png`、`aes-fix-fm.png`。

### 3) 菜单项对齐 —— 认可

- 8 项左对齐基线一致（统一 x=4 起排）；加速键 `(&N)` 跟在末字之后、字母带下划线，8 行格式统一。
  - 依据：`artifacts/final5/i18n/i18n-menu.png`、`a4-menu.png`；放大后 `记事本(N)/画布演示(C)/文件(F)/图片(I)/浏览器(B)/设置(S)/终端(T)/启动示例(D)` 左缘齐平。
- 中文与 `(` 之间无空格：与英文原案 `&Notepad` 的紧凑口径一致，亦符合 Windows 中文菜单惯例（如“记事本(N)”），判定可接受。
- P2 观察（不阻塞）：若后续追求更松的呼吸感，可在 CJK 与 `(` 之间加 1 个半角空格；本轮不改，避免动菜单行宽口径。

### 4) 状态栏溢出 —— 认可（附 P2 观察）

- 终端状态行 `终端 n/m ↑n/m  cwd`：并排半宽窗口（~384px）下 `终端 2/2 ↑1/1 /` 完整显示，未裁成半字；长 cwd 由文本块裁剪口径兜底。
  - 依据：`artifacts/audit5/a6-tb-active.png`（右窗窄宽状态行）、`a3-term-bottom.png`。
- 记事本状态行 `/notes.txt 行r/N 列c 已保存`：在面板宽度内完整显示。
  - 依据：`artifacts/final5/i18n/i18n-notepad-status.png`、`a3-note-mid.png`。
- FM 状态行仅短消息（`读取失败`/`空目录`/`就绪`），无溢出面。
  - 依据：`ax-mw-fm.png`、`aes-fix-fm.png`。
- 浏览器状态行最紧：URL + `状态= 字节= 毫秒 节点= 块=` 五段标签，最长样本 `http://10.255.255.1/dead` 下文字贴近右缘但仍在窗内，无裁切。
  - 依据：`artifacts/final5/i18n/i18n-error.png`、`artifacts/audit5/a5-br-error.png`、`a5-br-two.png`。
- P2 观察（不阻塞）：浏览器状态行余量偏小；若未来 URL 更长 + 节点/块位数变大，存在顶到右缘的风险。当前样本未溢出，本轮不动格式串（改文案/格式属禁止项边界外的排版，未触碰）。

### 5) 任务栏标签截断 —— 认可

- 多实例标签 `终端 1`/`终端 2`/`文件 1`/`浏览器 1` 均整字显示，未出现半个字被切；按钮底色与文字层次清楚。
  - 依据：`artifacts/audit5/a6-tb-two.png`、`a6-tb-active.png`、`a6-tb-close.png`、`aes-fix-tb.png`。
- 标签缓冲 24 字符（`chrome.c:1260`），当前中文最长 3 字 + 空格 + 序号，余量充足。

---

## 本轮发现并修复的 P1

### P1-1 FM 标题条关闭按钮未右对齐，与标题文字贴死（基线不认可，已修复）

- **现象（基线）**：FM 窗口标题条左端标题 `文件 1` 与关闭按钮 `关闭` 紧贴，连成 `文件 1关闭`，中间无间隙；关闭块没有被推到标题条右缘。
  - 依据：`artifacts/final5/i18n/i18n-window-titles.png`、`i18n-taskbar.png`、`artifacts/audit5/a4-titles.png`、`a6-tb-two.png`、`a6-tb-close.png`、`ax-mw-fm.png`；放大 `/tmp/title.png` 可见 `文件 1` 后紧接 `关闭`。
- **对照**：终端（`term.c:389`）、记事本（`notepad.c:278`）、设置（`settings.c:177`）标题条均设 `align_main = ALN_SPACE_BETWEEN`，标签居左、Close 居右分列；唯独 FM 缺失。
  - 依据：`artifacts/audit5/a6-tb-active.png`（终端 1 / 终端 2 关闭按钮贴右缘，左右分列清楚）、`i18n-notepad-status.png`。
- **根因**：`kernel_v25/src/gui/filemgr.c` 标题行规格未设主轴对齐，子块按默认 START 堆叠。
- **修复（低风险排版项，未动文案/逻辑/ABI）**：
  - `kernel_v25/src/gui/filemgr.c:385-386` 补 `title.align_cross = XALN_CENTER;` 与 `title.align_main = ALN_SPACE_BETWEEN;`，与终端/记事本/设置同口径。
- **修复后实机复核**：
  - `artifacts/audit5/aes-fix-fm.png`：双 FM 窗口，`文件 1`/`文件 2` 居左，`关闭` 居右贴右缘内边距，间隙清楚。
  - `artifacts/audit5/aes-fix-tb.png`：单 FM 窗口标题条同样左右分列；任务栏 `终端 1`/`文件 1` 标签完整。
  - 放大 `/tmp/fmtitle-l.png`、`/tmp/fmtitle-r.png`：关闭文字距右缘约一个 padding，未贴边、未与标题叠压。

## 五门禁复跑记录（修复后构建）

| 门禁 | 命令 | 结果 | RC |
|---|---|---|---|
| 链接布局 | `python3 kernel_v25/tools/check_link_layout.py kernel_v25/build/kernel.pe --strict` | PASS（仅既有 arena window 告警） | 0 |
| E3 回归 | `python3 tests/e3_regress.py` | badptr PASS / conc PASS，steps=2 ok=2 fail=0 | 0 |
| 像素回归 | `python3 tests/gui_pixel_regress.py --group {dark,light,hc,menu,drag,crop} --port 4723` | 6/6 组全部 PASS | 0 |
| VProbe 回归 | `python3 tests/vprobe_regress.py` | steps=10 ok=10 fail=0 | 0 |
| 拖拽探针 | `python3 tests/gui_drag_probe.py` | 首跑 `gui drag` 因 `frame_us=4034` 超 4000µs 时序阈值 `result=0`（与基线 `serial-a1-drag.log` 同口径：damage_area=768 / subtree=2 / b_same=1 几何口径全满足，仅性能阈值噪声）；重跑 `gui deny` PASS / `gui drag` PASS，2/2 | 0（重跑） |

## 本轮改动清单（文件:行）

- `kernel_v25/src/gui/filemgr.c:385-386`：FM 标题条补交叉轴居中 + 主轴两端分列（`XALN_CENTER` / `ALN_SPACE_BETWEEN`）。
- 新增取证：`artifacts/audit5/aes-fix-shot.py`（一次性截图探针，ISO 指向新构建 `catos.iso`）、`aes-fix-fm.png`、`aes-fix-tb.png`；串口日志 `serial-aes-fm.log` / `serial-aes-tb.log`（`*.log` 被 gitignore，留盘未入库）。

## 未认可项清单

- P0：无。
- P1：无（基线 P1-1 已修复并复核）。
- P2（观察项，不阻塞交付，留待后续迭代）：
  1. 浏览器状态行余量偏小，超长 URL + 多位节点/块计数时可能顶到右缘（`browser.c:392-410`）。
  2. 菜单加速键 `(&X)` 与末字之间无空格，属紧凑口径；如需更松呼吸感可后续统一加半角空格（`chrome.c:1567-1585`）。
