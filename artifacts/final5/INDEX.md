# final5 取证索引（六项需求 · 2026-11-29 凌晨批次）

- 仓库：`/home/user/Doubao/chats/38453665424926722/CatOs`（main，HEAD=`357a00e`，源码树干净）
- 口径：每例独立起 QEMU（`-m 256 -smp 1 -cpu qemu32 -vga std -net none -display none`），串口摘要行 + `screendump` 转 PNG；截图均为 800×600 gfx24 GUI 帧。
- 本批功能冻结（04:30）：未改 `kernel_v25/src/**`、`include/**`、`tests/**`，本索引只列既有证据。

---

## ① 挪动窗口残影 / 留半截 —— 已修

**做了什么**：离屏合成影子缓冲 + 第二遍全树重合成兜底。帧内全部绘制（底色兜底、块 body、图片 blit、alpha 混合、光标精灵）重定向到与屏幕同尺寸的影子帧缓冲；帧末只把本帧损伤矩形 + 光标 old/new 两个 12×12 精灵区从影子回拷 LFB，不整屏拷贝。损伤矩形先以活动底色填实兜底，再遍历可见块逐节点重画 body，露出区由真背景/下层窗口补齐，不再只铺底色烙残影。

**量化数字（串口）**：
- `GUI:shadow mode=1 w=800 h=600 bytes=1440000 heap_before=0 heap_after=1441792`（影子全尺寸 800×600×3 = 1,440,000 B；分配失败降级 640×480×3 = 921,600 B，再失败 mode=0 回退 legacy 直写 LFB）。
- `GUI:drag damage_rects=1 damage_area=768 frame_us=3799 b_same=1 result=1`（drag 门禁 `artifacts/gui-drag-probe/serial.log`；批次 A 取证帧 `frame_us=3980`，见 `artifacts/final5/REPORT.md`）。
- 稳态空闲帧 27–47µs（`GUI:frame n=31 us=27` … `n=211 us=47`）。

**证据图（9 张）**：
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/offscr/offscr-shadow-01.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/offscr/offscr-shadow-02.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/drag/drag-slow-01.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/drag/drag-slow-02.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/drag/drag-slow-03.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/drag/drag-fling-01.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/drag/drag-fling-02.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/drag/drag-fling-03.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/drag/drag-edge-out-back.png`

**状态**：✅ 已修，drag 探针 2/2 PASS；降级 640×480 路径本轮未在 QEMU 实测（边界裁剪已在 `clip()`/blit 做处理）。

## ② 鼠标移动整块刷新 —— 已修

**做了什么**：同上离屏合成路径，光标经影子缓冲合成后自行回拷旧/新 12×12 精灵矩形；鼠标移动只标脏光标 old/new 两个精灵区，不再整屏刷新。

**量化数字**：稳态空闲帧 27–47µs（`serial-cursor.log` / `serial-cursor-2.log`：`n=31 us=28` … `n=211 us=29`，另帧 `n=61 us=48` / `n=91 us=55`）；两帧光标位置无拖尾。

**证据图（2 张）**：
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/cursor/cursor-move-a.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/cursor/cursor-move-b.png`

**状态**：✅ 已修。

## ③ 终端 / 记事本滚动 —— 已做

**做了什么**：终端加 `view_top`/`follow` 状态、PgUp/PgDn + 滚轮钩子、脱跟随/贴底跟随；记事本滚动上下界钳制 + 滚轮钩子；状态行补位置指示，串口打 `SCROLL:` 摘要行。

**量化数字（串口）**：
- 终端 `total=61`：`SCROLL:term up top=32 total=61 follow=0`（向上回看已脱跟随）；`SCROLL:term bottom top=38 total=61 follow=1`（贴底跟随）。
- 记事本 `total=60`：`SCROLL:note up top=37 total=60`。

**证据图（3 张）**：
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/scroll/term-scroll-mid.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/scroll/term-scroll-bottom-follow.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/scroll/note-scroll-60l-mid.png`

**状态**：✅ 已做（滚轮 + PgUp/PgDn；thumb 拖动、Ctrl+Home/End 为砍项顺位，未做）。

## ④ GUI 汉化 —— 第一刀完成，第二刀待办

**做了什么**：沿用既有 `trs(en, zh)` 内联机制，菜单/标题/状态栏/任务栏标签/错误提示接入中文；菜单加速键保留 ASCII 字母，形式 `中文(&X)`（如 `记事本(&N)`），Alt+字母直达口径未改；命令名、路径、实例编号保持英文；本批用字全部命中 `font_hz.inc`（6961 码点，缺字 0），未扩字库。

**量化数字**：`trs()` 接入 **55** 处（chrome 8 / 终端 5 / 记事本 6 / 文件管理器 7 / 图片查看器 4 / 浏览器 23 / 设置 2 / 画布 1）。

**证据图（6 张）**：
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/i18n/i18n-menu.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/i18n/i18n-window-titles.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/i18n/i18n-statusbar.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/i18n/i18n-taskbar.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/i18n/i18n-error.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/i18n/i18n-notepad-status.png`

**状态**：🟡 第一刀完成；**第二刀（七组件内部长文案、设置面板细分项、演示样本窗、blkgen 探针页）未做，列为待办**（见「未采 / 待办说明」）。

## ⑤ 浏览器地址栏 —— 已修好

**做了什么**：根因为 `browser.c:370 br_normalize()` 在**未清零的栈缓冲**上用 `xlen(out)` 反复扫描定位写偏移，扫进栈垃圾后字符写到错误偏移，新旧两串 URL 叠印；改为显式偏移顺序写（先置 `out[0]=0`，按记录的 `o` 偏移顺序写）。修后键入例串口 `BR:edit addr=example.com/x len=13 active=1`（干净串，无追加叠印）；历史 `BR:back url=sample://local hist=2` / `BR:fwd url=sample://page2 hist=2`；错误路径 `BR:error active=1 url=http://127.0.0.1:9/nope hist=2`。

**量化数字**：样本页 `GUI:br bytes=300 nodes=12 blocks=12`；第二实例 `bytes=156 nodes=6 blocks=6`。

**证据图（5 张）**：
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/browser/browser-url-edit.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/browser/browser-back-forward.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/browser/browser-error-page.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/browser/browser-two-instances.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/browser/control-desktop-browser.png`（对照：不走探针的真实桌面主流程，排除探针上下文因素）

**状态**：✅ 已修好（地址栏叠印根因闭环）。

## ⑥ 任务栏运行实例按钮 —— 已修好

**做了什么**：根因为 `compose.c:491 reorder_z()`——桌面静态子树首次 raise 后父槽 `tb_slot` 被抬到 z=3，而 `tb_rebuild()` 动态挂入的按钮仍带规格默认 z=0，排序落在父容器之前；父容器不透明底色后画，把按钮底色/描边/标签整段盖掉。修法：按钮改回 `BLK_BUTTON` 单块承载底色/描边/聚焦反色/居中文本，`role_bg=ROLE_PANEL`，新建按钮 z 跟随父槽 `bb->z = slot->z`。

**量化数字（串口）**：
- 双实例：`TB:entry kind=term idx=0 label=Terminal 1 active=0` / `TB:entry kind=files idx=0 label=Files 1 active=1`。
- 高亮：`FOCUS:kind=term idx=1`；`TB:entry kind=term idx=0 active=0` / `TB:entry kind=term idx=1 active=1`。
- 关闭移除：仅剩 `TB:entry kind=files idx=0 label=Files 1 active=0`。

**证据图（3 张）**：
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/taskbar/taskbar-two-instances.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/taskbar/taskbar-active-highlight.png`
- `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/final5/taskbar/taskbar-closed-removed.png`

**状态**：✅ 已修好。

---

## Commit 链

| commit | 一句话 |
|---|---|
| `85ea80f` | feat(gui)：离屏合成影子缓冲 + 拖动残影根因修复；附带 `kvheap.c kv_extent()` 栈数组 `pa_tmp[8]` 改 VLA，修 352 帧大 extent 越界写穿 8KB 内核栈。 |
| `19252fe` | docs：记录 `build/` 跟踪脚本不可整目录删除与 `_ce_api` 首轮自愈易错点。 |
| `dbcb71c` | feat(gui)：终端/记事本滚轮滚动 + 任务栏实例按钮 + 浏览器多实例池（首版，部分渲染问题留待后续 fix）。 |
| `42a4b0d` | fix(gui)：修复任务栏按钮零宽 / 浏览器正文不上屏 / 地址栏缓冲损坏 / 多实例 active 误报（M3 一轮排查，绘制序与栈缓冲两根因在此轮之后闭合）。 |
| `4e9bd2c` | fix(gui)：任务栏实例按钮上屏（根因 = reorder_z 父子 z 序覆盖）+ 滚动探针补 `follow=` 打点。 |
| `e7262ec` | docs：记录「机制对但缺打点」证据易被判未取证的易错点。 |
| `831e55b` | fix(gui)：浏览器 URL 提交后叠印（根因 = `br_normalize` 在未清零栈缓冲上扫栈垃圾定偏移）。 |
| `fb7959f` | feat(gui)：GUI 汉化第一刀（菜单 / 标题 / 状态栏 / 任务栏 / 错误提示，`trs()` 55 处）。 |
| `357a00e` | docs(gui)：补记事本状态栏汉化取证截图（M5 第一刀）。 |

## 门禁汇总（clean 全量构建，最终 ISO）

| 门禁 | 口径 | 结果 |
|---|---|---|
| 链接布局 strict | `tools/check_link_layout.py build/kernel.pe --strict` | RC=0（仅既存 arena window warn，非本批引入） |
| e3 回归 | `tests/e3_regress.py` | 2/2 PASS |
| 像素回归 | `tests/gui_pixel_regress.py`（dark/light/hc 等） | 6/6 PASS |
| vprobe 回归 | `tests/vprobe_regress.py` | 10/10 PASS |
| drag 探针 | `tests/gui_drag_probe.py` | 2/2 PASS |

## 未采 / 待办说明（如实标注）

1. **汉化第二刀未做**：七组件内部长文案 / 帮助文本；设置面板细分项 `Appearance`、`App custom` / `Calm & soft`；演示样本窗标题 `CatOS Sample` 与正文；`blkgen.c` 探针页按钮（Back/Fwd/Refresh/Stop/addr/Menu）；浏览器样本页 HTML 正文、地址栏 URL（按设计保留英文）。砍因 = 04:30 功能冻结，优先级低，已登记进内核问题清单 #225。
2. **右键菜单与模态对话框 N/A**：当前 GUI 无右键菜单、无模态文件/会话对话框实现，未造对应截图；`i18n-dialog.png` 计划中的对话框帧未采到，本轮以错误态帧 `i18n-error.png` 顶替佐证中文错误提示上屏。
3. **记事本中段滚动仅靠串口佐证**：记事本 60 行测试内容全为 `x`（同字符），中段帧画面上无法目检位置变化；`note-scroll-60l-mid.png` 配合串口 `SCROLL:note top=37 total=60` 佐证滚动位置，无像素级中段断言。
4. **浏览器 back 停留态帧未截到**：探针 back 之后又执行 fwd，终帧为 fwd 之后态；back 回到上一页仅由串口 `BR:back url=sample://local hist=2` 佐证。
5. **影子降级路径未实测**：640×480×3（mode=2）本轮全尺寸分配成功，降级裁剪仅静态走查，未在 QEMU 实测。
