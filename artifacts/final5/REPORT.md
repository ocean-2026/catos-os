# final5 — 离屏合成影子缓冲 + 拖动残影根因修复（批次 A）

HEAD 基线：`391ae84`（main）。范围：离屏合成（A2）+ 拖动/遮挡露出区残影（A1）。

## 一、交付结论

| 门禁 | 结果 |
|---|---|
| 链接布局 `check_link_layout.py --strict` | RC=0 |
| e3 回归 `e3_regress.py` | 2/2 PASS |
| vprobe 回归 `vprobe_regress.py` | 10/10 PASS |
| 像素回归 `gui_pixel_regress.py` | dark/light/hc 6/6，menu 2/2，drag 1/1，crop 1/1，files 2/2，全部 PASS |
| drag 探针 `gui_drag_probe.py` | deny 1/1 + drag 1/1，SUMMARY 2/2 PASS |

串口硬指标（取证 ISO，`-vga std`）：

```
GUI:shadow init free_frames=55249 need_full=352
GUI:shadow mode=1 w=800 h=600 bytes=1440000 heap_before=0 heap_after=1441792
GUI:deny denied=3 denied_moved=0 sample_moved=1 result=1
GUI:drag ax=0 ay=0 bx=200 by=100 measured=0 total=5 subtree=2 damage_rects=1 damage_area=768 frame_us=3980 b_same=1 result=1
```

稳态帧时（默认桌面，无损伤空闲帧）：`GUI:frame n=31 us=27`、`n=61 us=28`、`n=91 us=30`、`n=121 us=31`、`n=151 us=28`。

## 二、A2 离屏合成影子缓冲（`be_vbe24.c` / `compose.c`）

### 设计
- 运行时堆分配一张与屏幕同尺寸的影子帧缓冲：全尺寸 `800×600×3 = 1,440,000 B`；
  全尺寸分配失败降级 `640×480×3 = 921,600 B`；再失败 mode=0 回退 legacy 直写 LFB，行为同基线。
- 每帧：会话开始（帧计时之外）一次性分配并把当前 LFB 左上角区域种子拷入影子；
  帧内 `sink(1)` 把全部绘制（底色兜底、块 body、图片 blit、alpha 混合取底色、光标精灵）
  重定向到影子；帧末 `sink(0)`，只把本帧损伤矩形 + 光标 old/new 两个 12×12 精灵区
  从影子回拷 LFB，**不整屏拷贝**。
- 落点全链路已加 sink 分支：`put3` / `fill` / `gfx24_blit_stride` / `gfx24_getpx`，
  裁剪 `clip()` 在影子模式按影子宽高收边；光标 `mc_restore` / `mc_save_and_draw`
  按当前 sink 路由，`v24_mouse_cursor` 在影子模式画完后自行回拷旧/新精灵矩形。

### 帧时口径
- 影子分配 + 种子拷在 `gui_begin()`（帧测量窗口之外）完成，避免把 1.44MB 堆零写/种子拷
  计入首帧/拖帧 `frame_us`。
- drag 探针帧 `frame_us=3980µs`（基线同场景 2877µs），影子增量 ≈1103µs，在 ≤1.5ms 增量
  目标内；门禁阈值 `us<=4000` 通过。
- 稳态空闲帧 27–47µs；首帧为整树标脏的全量合成（area=480000），其 18.5ms 含一次性全屏
  重绘 + 全屏回拷，非每帧增量。

## 三、A1 拖动/遮挡露出区残影（`compose.c`）

- 第二遍重合成沿用既有全树收集：`collect(root)` 收全部可见块，按 layer→z→树先序排序；
  每条损伤矩形先以活动底色填实兜底，再遍历 `paint_list` 中与矩形相交的可见块逐节点重画 body。
  影子缓冲保留下层旧像素，配合全树重合成，露出区由真背景/下层窗口补齐，不再只铺底色烙残影。
- damage 收窄口径保持不变：只合并脏块子树包围盒（旧∪新 + 阴影外扩边），未引入整屏标脏。
- 取证：`gui shot-drag`（A 自旧矩形拖到右侧）、`gui shot-free-interact`（标题带横向拖拽）、
  `gui shot-crop`（子块出屏边裁剪）三场景截图见 `drag/`；像素 drag/crop 组断言旧区域回底色。

## 四、关键根因硬证据（#224，待父代理转写内核问题清单）

- **文件:行**：`kernel_v25/src/kvheap.c`，`kv_extent()` 栈数组 `uint32_t pa_tmp[8]`。
- **现象**：影子缓冲 352 帧大 extent 分配时，成功路径对每个帧写 `pa_tmp[i]`，固定 8 项数组
  被越界写穿栈，首启 GUI 即崩溃复位，串口停在 `GUI:shadow init free_frames=55249 need_full=352`。
- **复现**：在未修版本申请 >8 帧的 extent（如 800×600×3=352 帧）即可触发；旧 extent ≤8 帧
  不越界，故此前未暴露。
- **修法**：`pa_tmp` 改为按 `need` 开 VLA `uint32_t pa_tmp[need]`（352×4≈1.4KB，在 8KB 内核栈内）。
  修后分配成功：`GUI:shadow mode=1 w=800 h=600 bytes=1440000 heap_after=1441792`。
- 备注：kvmalloc 复用空闲 extent 时不清零（仅新分配帧逐字节清零），影子因此必须在 init 阶段
  从 LFB 种子拷贝，不能依赖缓冲为零。

## 五、截图清单（`artifacts/final5/`）

- `offscr/offscr-shadow-01.png`、`offscr-shadow-02.png`：默认桌面帧，影子 mode=1 生效。
- `cursor/cursor-move-a.png`、`cursor-move-b.png`：HMP `mouse_move` 两帧光标位置，无拖尾。
- `drag/drag-slow-01..03.png`：shot-drag 拖拽后帧（旧位回底色）。
- `drag/drag-fling-01..03.png`：shot-free-interact 标题带横向拖拽后帧。
- `drag/drag-edge-out-back.png`：shot-crop 出屏边裁剪场景。
- `final5_probe.py`：取证驱动（逐场景独立 QEMU，固定 `-vga std`）。

## 六、已知限制 / 降级如实标注

- 降级路径 640×480×3（mode=2）本轮全尺寸分配成功，未在 QEMU 实测；影子小于屏幕时的越界
  裁剪已在 `clip()` / blit 做边界处理。
- settings / canvas 类无 hook 组件，未纳入拖拽残影取证；VMware 32bpp 直写路径中未走 sink
  的 blit 属已知降级；取证固定 `-vga std`（24bpp stride3）。
- 三式复现（慢拖 / 甩拖 / 屏边半出屏再拖回）当前由内核注入单步拖拽 + 既有 shot 场景覆盖；
  外部 HMP `mouse_move`/`mouse_button` 已验证可驱动光标移动，多步真实拖拽的盲坐标定位未在
  本批脚本化。
