# M5 · GUI 汉化第一刀 验收报告

基线 HEAD：`831e55b`。本刀范围：菜单 8 项 / 窗口标题 / 状态栏 / 任务栏标签 / 错误提示。
沿用既有 `trs(en, zh)` 内联机制，未新造 i18n 结构、未引库、未改表结构；命令名、路径、实例编号保持英文。

## 门禁（clean 全量构建后）

| 门禁 | 命令 | 结果 |
|---|---|---|
| 链接布局 | `tools/check_link_layout.py build/kernel.pe --strict` | RC=0 |
| e3 回归 | `tests/e3_regress.py` | 2/2 PASS |
| 像素回归 | `tests/gui_pixel_regress.py` | 6/6 PASS（英文默认路径 golden 未漂移，未改断言） |
| vprobe | `tests/vprobe_regress.py` | 10/10 PASS |
| 拖拽探针 | `tests/gui_drag_probe.py` | 2/2 PASS |

## 接入条目

- `trs()` 调用点 **55** 处；去重后可见文案条目 **55** 条（菜单 7 + 启动示例 1 + 任务栏前缀 4 + 开始/空桌面 2；终端 5；记事本 6；文件管理器 7；图片查看器 4；浏览器 23；设置 2；画布 1）。
- 菜单加速键保留 ASCII 字母，形式 `中文(&X)`：`记事本(&N)` `画布演示(&C)` `文件(&F)` `图片(&I)` `浏览器(&B)` `设置(&S)` `终端(&T)` `启动示例(&D)`；`txt_accel_char` 口径未改，Alt+字母直达不受影响。
- 标题与任务栏标签同口径：`终端 N` / `文件 N` / `浏览器 N` / `记事本` / `设置` / `图片` / `画布演示`，任务栏按钮标签与窗口标题一致。
- 字库：本批用字全部命中 `font_hz.inc`（6961 码点，缺字 0），未扩字库。

## 截图（`artifacts/final5/i18n/`，已逐张 Read 复核）

| 文件 | 内容 |
|---|---|
| `i18n-menu.png` | 菜单 8 项中文，加速键字母下划线可见 |
| `i18n-window-titles.png` | 终端 `终端 1`、文件管理器 `文件 1` 标题中文，关闭按钮 `关闭` |
| `i18n-statusbar.png` | 终端 `终端 1/1 ↑1/1 /`、FM `读取失败`；记事本状态 `/notes.txt 行1/1 列1 已保存`（独立 boot 取证），无溢出裁字 |
| `i18n-taskbar.png` | 任务栏 `终端 1` `文件 1`，与窗口标题一致 |
| `i18n-error.png` | 浏览器错误页 `导航失败` / `重载`，状态行 `状态= 字节= 毫秒 节点= 块=`，任务栏 `浏览器 1` |
| `i18n-notepad-status.png` | 补充取证：记事本标题 `记事本`、状态行 `/notes.txt 行1/1 列1 已保存`，任务栏 `记事本`，无溢出 |

复核结论：无 `???`、无豆腐块、无横向溢出裁字。

## 未做项（登记待办，按砍项顺序）

1. 设置面板细分项：分类 `Appearance`、选项 `App custom` / `Calm & soft`。
2. 演示样本窗标题 `CatOS Sample`、正文 `demo content area`。
3. `blkgen.c` 探针页按钮（Back/Fwd/Refresh/Stop/addr/Menu）。
4. 浏览器样本页 HTML 正文、地址栏 URL（按设计保留英文）。
5. 七组件其余内部长文案 / 帮助文本。
6. 右键菜单、模态文件/会话对话框：当前 GUI 无实现，N/A，未造。

## 红线自查

版本号 2.82 未动；CE ABI append-only；无 GPL；ISO/bin 未入库；无 `window/win_id/wm_`；未整屏标脏；未新增 `.bss` 大数组；未碰 `/home/user/work/catos-iso-build`。
