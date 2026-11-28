# 批次 D · GUI 全面汉化 方案（只读勘察落纸，02:00 资源释放后才编译/起 QEMU）

基线 HEAD：`4fa9fb76511165b30da5f18fb7bf83839624fc79`（起手 `git log -1`）。
他人未提交改动（be_vbe24.c / compose.c / gui.h / filemgr.c 末尾追加 / notepad.c 末尾追加 / 若干 artifacts）**不回退、不覆盖**；提交时只暂存本批 i18n hunk。

## 0. 既有机制（沿用，不新造 i18n 结构、不引第三方库）

- `trs(const char *en, const char *zh)`（lang.c:388）：`g_lang==LANG_ZH && zh` 返回 zh，否则 en。shell.c / fs.c / posix.c 已在用。
- `tr(id)` 索引表（22 条）、`help_zh[]` 帮助描述表（168 条）；本批 GUI 文案**不走** help_zh，统一用 `trs(en, zh)` 内联对。
- 语言开关：`lang zh` → 写 `/etc/lang` + 注册表；`lang_init()` 启动读取；GUI 与 shell 同进程，`g_lang` 共享。英文为默认，英文路径文案逐字不变。
- GUI 源码当前 `trs(` 调用数 = 0：本批是 GUI 首次接入。
- 基线既有翻译量：`trs()` 内联对 390 处（shell/fs/posix）、`help_zh[]` 168 条、`tr(id)` 索引 22 条。

## 1. 字形覆盖率（本批最大风险，已摸底）

- 字库 `kernel_v25/src/font_hz.inc`：6961 个码点（GB2312 6763 汉字 + CJK 标点/全角符号），按 Unicode 二分查找；缺字回落 ASCII `?`（宽字按窄字画）。
- 候选汉化文案去重后 **118 个汉字 / 全角标点，缺字 0**；全角标点 `，。、：；？！（）“”—…·` 等均在库。
- 结论：**无需扩字形、无同义替换表**。新于 lang.c 既有用字 29 个（事传偏刷和响块型外布循截抓柔桌毫求浏满片画类舒被观览达连阻），逐一在字库中命中。
- 风险兜底：若实机截图出现 `???`/豆腐块，按既有字库机制（`tools/unifont-*.hex` + `gen_hz_font.py`）只补缺字；补不动则把该处换成已覆盖字同义表述，替换表进最终报告。**不塞临时点阵凑数花屏。**

## 2. 可见字符串清单（按组件；现状全英文硬编码，trs 覆盖 = 缺）

### 2.1 任务栏 / 桌面 / 菜单（chrome.c）
| # | 英文原文 | 位置 | 拟译 zh | 备注 |
|---|---|---|---|---|
| 1 | `Start` | 任务栏 Start 按钮 | 开始 | 像素门禁只断言非底色，不断言字形 |
| 2 | `Empty` | 空桌面提示 | 空桌面 | |
| 3 | `CatOS Sample` | 演示样本窗标题 | 示例窗口 | 默认桌面即挂样本窗 |
| 4 | `demo content area` | 样本窗正文 | 演示内容区 | |
| 5 | `&Notepad` | 菜单 1 | `&记事本` | 保留 `&n` 加速键 |
| 6 | `&Canvas demo` | 菜单 2 | `&画布演示` | 保留 `&c` |
| 7 | `&Files` | 菜单 3 | `&文件` | 保留 `&f` |
| 8 | `&Images` | 菜单 4 | `&图片` | 保留 `&i` |
| 9 | `&Browser` | 菜单 5 | `&浏览器` | 保留 `&b` |
| 10 | `&Settings` | 菜单 6 | `&设置` | 保留 `&s` |
| 11 | `&Terminal` | 菜单 7 | `&终端` | 保留 `&t` |
| 12 | `Launch &demo` | 菜单 8 | `启动 &示例` | 保留 `&d` |

> 菜单加速键口径：`txt_accel_char()` 取 `&` 后 ASCII 字母；`&` 本身不计宽、不绘制。探针 `gui menu-accel` 断言 Alt+N 直达首项 accel `n`，故译标签必须保留 `&N`。

### 2.2 终端（term.c）
| # | 英文原文 | 拟译 zh | 备注 |
|---|---|---|---|
| 13 | 标题 `Terminal N` | **保持英文** | 实例标识，配合任务栏 |
| 14 | 状态行前缀 `Terminal N/M   cwd` | 前缀保持英文，cwd 路径不动 | 实例标识 |
| 15 | `Close`（关闭按钮） | 关闭 | 各组件通用 |
| 16 | `terminal limit reached (4)` | 终端已达上限（4） | 错误提示 |
| 17 | 欢迎行 `CatOS block terminal (builtins: help pwd ls cd echo clear history)` | CatOS 块终端（内置：help pwd ls cd echo clear history） | 命令名保留英文 |

### 2.3 记事本（notepad.c）
| # | 英文原文 | 拟译 zh | 备注 |
|---|---|---|---|
| 18 | 标题 `Notepad` | 记事本 | |
| 19 | `Close` | 关闭 | |
| 20 | 状态行片段 `  R` | `  行` | |
| 21 | 状态行片段 ` C` | `  列` | |
| 22 | `  dirty` | `  未保存` | 三态之一 |
| 23 | `  saved` | `  已保存` | |
| 24 | `  full` | `  已满` | |
| — | 默认路径 `/notes.txt` | 保留英文 | 路径 |

### 2.4 文件管理器（filemgr.c）
| # | 英文原文 | 拟译 zh | 备注 |
|---|---|---|---|
| 25 | 标题 `Files N` | `文件 N` | |
| 26 | `Close` | 关闭 | |
| 27 | `..`（上级目录按钮） | 保留英文 | 路径实例标识 |
| 28 | `Refresh` | 刷新 | |
| 29 | `read failed` | 读取失败 | 错误 |
| 30 | `empty` | 空目录 | 空态 |
| 31 | `ready` | 就绪 | |
| 32 | `unsupported type` | 不支持的类型 | 错误提示 |

### 2.5 图片查看器（images.c）
| # | 英文原文 | 拟译 zh | 备注 |
|---|---|---|---|
| 33 | 标题 `Images` | 图片 | |
| 34 | `unsupported format` | 不支持的格式 | 错误 |
| 35 | 状态片段 `  off=` | `  偏移=` | |
| 36 | 状态片段 `  decode=` | `  解码=` | |
| 37 | `x`（宽×高分隔）、`us`（微秒单位） | 保留英文 | 单位/符号 |
| — | 文件名、`/samples/...` | 保留英文 | 路径 |

### 2.6 浏览器（browser.c）
| # | 英文原文 | 拟译 zh | 备注 |
|---|---|---|---|
| 38 | `Back` | 后退 | 工具栏 |
| 39 | `Fwd` | 前进 | |
| 40 | `Refresh` | 刷新 | |
| 41 | `Stop` | 停止 | |
| 42 | `out of memory` | 内存不足 | 错误 |
| 43 | `network not ready` | 网络未就绪 | 错误 |
| 44 | `redirect https blocked` | 阻止跳转 https | 错误 |
| 45 | `dns failed` | DNS 解析失败 | 错误 |
| 46 | `connect failed` | 连接失败 | 错误 |
| 47 | `http status error` | HTTP 状态错误 | 错误 |
| 48 | `no reply` | 无响应 | 错误 |
| 49 | `redirect loop` | 跳转循环 | 错误 |
| 50 | `truncated body` | 正文被截断 | 错误 |
| 51 | `body needs resume` | 正文需续传 | 错误 |
| 52 | `fetch error` | 抓取失败 | 错误 |
| 53 | `stop: v0 single request` | 停止：v0 单请求 | |
| 54 | 状态片段 `  status=` | `  状态=` | |
| 55 | 状态片段 `  bytes=` | `  字节=` | |
| 56 | 状态片段 `ms  nodes=` | `毫秒 节点=` | |
| 57 | 状态片段 `  blocks=` | `  块=` | |
| 58 | 占位 `[img: unsupported ` | `[图片: 不支持 ` | 低优先 |
| 59 | 占位 `[img: ` | `[图片: ` | 低优先 |
| — | 地址栏 URL、样本页 HTML 正文 | 保留英文 | 网页内容/路径 |

### 2.7 设置（settings.c）
| # | 英文原文 | 拟译 zh | 备注 |
|---|---|---|---|
| 60 | 标题 `Settings` | 设置 | |
| 61 | `Close` | 关闭 | |
| 62 | 分类 `Appearance` | 外观 | |
| 63 | 选项 `App custom` | 应用默认 | |
| 64 | 选项 `Calm & soft` | `柔和 &舒缓` | 保留 `&s` 加速键 |

### 2.8 画布（canvas.c）
| # | 英文原文 | 拟译 zh | 备注 |
|---|---|---|---|
| 65 | 标题 `Canvas demo` | 画布演示 | |

### 2.9 不在本批范围 / 已中文 / 探针专用
- **SAK 安全模式面板**（sak.c）：标题「安全模式」、`返回桌面/块列表/锁定/重新启动` 已是硬编码中文，不动；`SAK unlock: password:` / ` denied` 为文本模式口令提示，低优先可砍。
- **blkgen.c** 探针页按钮 `Back/Fwd/Refresh/Stop/addr/Menu`：仅 `gui shot-blkgen` 探针可达，不进默认桌面，砍项顺位最后。
- **右键菜单 / 模态文件·会话对话框**：当前 GUI 无实现（input.c 无右键上下文菜单；inputbox 为内联输入框，无独立弹窗）。`i18n-dialog.png` 以浏览器/文件管理器错误态顶替，报告中说明 N/A。
- shell 命令名、参数、路径、`/etc`、实例编号一律保留英文。

## 3. 实施要点（02:00 后）

1. 每个用到 `trs()` 的 gui 文件加 `#include "lang.h"`（`-Iinclude`，与 shell.c 同口径）。
2. 文案经 `trs("English", "中文")` 包裹；静态串指针生命周期不变（`b_text` 存指针，常驻缓冲先 copy 再指）。
3. 状态行拼接片段逐段 `trs()`；数字、单位、路径原样拼接。
4. 菜单/选项保留 `&X` ASCII 加速键；不改 `txt_accel_char` 口径。
5. 不改字体渲染口径、不改断言逻辑；英文默认路径文案逐字不变，保证像素门禁基线不漂移。
6. 宽度换行：复用 textlayout 既有宽字计宽（中文 = 2 单位）；`help` 长文本在控制台走既有 trhelp，不重排。

## 4. 门禁（四门禁 + 拖拽附加）

- strict：`python3 kernel_v25/tools/check_link_layout.py kernel_v25/build/kernel.pe --strict` → RC=0
- e3：`python3 tests/e3_regress.py` → 2/2
- 像素：`python3 tests/gui_pixel_regress.py <iso> --group {dark,light,hc,menu,drag,crop}` → 6/6（颜色/几何断言，无 OCR；若汉化致 golden 失配，按"文案更新 golden"处理，不改断言放水）
- vprobe：`python3 tests/vprobe_regress.py` → 10/10
- 拖拽附加：`python3 tests/gui_drag_probe.py` → 2/2

## 5. 截图落 `artifacts/final5/i18n/`

| 文件 | 触发 |
|---|---|
| i18n-menu.png | `lang zh` 后 `gui shot-menu` |
| i18n-term.png | 新增内部 `gui shot-term` 常驻路径（仿 `shot-notepad`，`gui_term_open()`；纯内部 shell 动词，不动 CE ABI）。若无更优注入入口，按此落地 |
| i18n-notepad.png | `gui shot-notepad` |
| i18n-files.png | `gui shot-files` |
| i18n-images.png | `gui shot-images` |
| i18n-browser.png | `gui shot-browser`（离线样本页） |
| i18n-settings.png | `gui shot-settings` |
| i18n-canvas.png | `gui shot-canvas` |
| i18n-dialog.png | 错误态顶替（浏览器无网 / 文件管理器不支持类型） |
| i18n-help-wrap.png | 文本控制台 `help`（zh），验长文换行不溢出 |

每张自检：无 `???`、无豆腐块、无中英混排断裂、无横向溢出。

## 6. 砍项顺位（时间不够按序丢）

1. 设置面板细分项（选项文案 63/64）
2. 七组件内提示/空态（状态栏片段、错误原因串）
3. 右键菜单/对话框（本就缺实现）
4. **不可砍**：菜单 8 项、窗口标题、状态栏、错误提示。

## 6.5 在位批次冲突提示（02:00 实施前必须重读工作树）

- 起手后另一批次（任务栏实例按钮 + 浏览器多实例）正在改 `chrome.c` / `browser.c` / `gui.h` / `compose.c` / `filemgr.c` / `notepad.c`，工作树有未提交改动。本批**不回退、不覆盖**；开工前重新 `git status` + 重读目标文件，按当时实际行号/文案对接。
- 任务栏实例按钮标签前缀 `Terminal ` / `Files ` / `Browser ` / `Notepad`（`tb_make_label`）：按实例标识口径**保留英文**，与窗口标题一致。
- `browser.c` 被多实例批次大改；其错误串/状态片段在实施时以工作树现状为准重新核对。

## 7. 红线自查

CE ABI append-only；无 GPL；版本号不动；ISO/bin 不入库；无 `window/win_id/wm_`；不整屏标脏；大数组不进 `.bss`。
