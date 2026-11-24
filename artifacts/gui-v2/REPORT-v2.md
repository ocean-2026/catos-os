# 块化 GUI v2 交付报告（feat/gui-v2）

> 基线：`feat/gui-blocks` `d6fbecd`；已并入 hotfix 分支 `fix/gui-console-leak`
> tip `596a78e`（控制台泄漏 + 二次进入闪退）。本报告只覆盖 v2 增量；v1 既有
> 层枚举、令牌、幂等投影、33fps 三 tick 节流、回调只打脏位口径不变。

## 1. 范围与交付物

v2 在 v1 声明式块树之上落地四块能力：

1. **`BLK_FREE` 任意矩形排版**：锚点（L/R/T/B/HCENTER/VCENTER）、百分比
   （单位 1/1000 父框）、min/max 尺寸夹取、四向外边距；FREE 子块不参与父容器
   主轴流动，与流式容器可互嵌。
2. **溢出三态**：`OVF_CLIP`（默认裁剪）/ `OVF_VISIBLE`（越界仍绘仍命中）/
   `OVF_SCROLL`（裁剪 + 平移栈，只改绘制落点不改几何）。
3. **拖拽 / 缩放属性化**：`drag_mode`（NONE/TITLE/FULL）、`drag_bounds`
   （FREE/PARENT/SCREEN）、`resize_dir` 八位方向掩码 + min/max/step；命中优先级
   = resize 边缘带（父链就近）> 子块自身拖拽 > 祖先拖拽带 > 子块命中。
4. **画布子矩形脏区 + 页池扩容**：`CV_MAX_PAGES` 24→64；PFN 表与帧缓冲均走
   帧分配器按需申请、销毁归还（不入 `.bss` 静态大数组）；`cv_dirty` 子矩形并集
   标脏，合成层只 blit 脏子矩形。

## 2. 源码改动（相对 d6fbecd）

| 文件 | 改动 |
|---|---|
| `include/gui/block.h` | 枚举末尾追加 `BLK_FREE=11`；新增 `OVF_*` / `ANC_*` / `DRAG_*` / `BOUND_*` / `RESIZE_*`；`blk_spec` 与运行时 `block` 镜像新字段（overflow/anchor/pct/px/min/max/margin/drag/resize/scroll/pos）。 |
| `include/gui/gui.h`、`gui_int.h` | v2 交互/脏区接口声明。 |
| `src/gui/desc.c` | `b_free()` creator；`apply_spec` 搬运新字段并识别几何变化。 |
| `src/gui/layout.c` | `BLK_FREE` 入 measure 聚合；`layout_free_child` 解约束（父框扣外边距 → 锚点/百分比 → min/max 夹取 → 落矩形）；`moved` 后保留绝对几何，跨帧不被布局重置（FREE 子块与叶子容器回退分支均加守卫）。 |
| `src/gui/paint.c` | 平移栈（SCROLL）；裁剪逃逸条件 = 子层 > 父层 / 子块 VISIBLE / 父块 VISIBLE；`paint_node_clipped` 祖先链同步跳过；画布走子矩形脏区 blit（显式步进）。 |
| `src/gui/input.c` | 命中优先级与 `hit_allowed` 与视觉同真；MOVE 只改声明几何/pos，RESIZE 按方向位 + min/max/step；`drag_bounds` 夹父框/屏幕；`gui_interaction_at()`；`gui_input_reset` 清 hover。 |
| `src/gui/compose.c` | VISIBLE 子块脏区并入父块脏区防残影；`gui_begin` 收摊顺序沿用 hotfix。 |
| `src/gui/canvas.c` | 页池 24→64，帧/PFN 表走 allocator；`cv_dirty` 子矩形并集、绘后清位；`cv_dirty_rect/clear` 供合成层取用。 |
| `src/gui/chrome.c` | 探针 `gui free` / `gui cvdirty` / `gui interact`；五个样例态 `shot-free-dash/sidebar/float/overflow/interact`。 |

## 3. 串口探针结果（终版 ISO）

批量回归 26 个子命令全部 PASS（端口 4510–4519，scratch 用 v2 私有副本）。
v2 新增探针原始记录：

### `gui free`（锚点/百分比/min-max 解算绝对矩形）

```
  A              x1   latest=(20,20,120,80)
  B              x1   latest=(600,0,200,80)
  C              x1   latest=(350,270,100,60)
  D              x1   latest=(400,300,240,180)
  E              x1   latest=(0,540,120,60)
  result         x1   latest=1
```

### `gui interact`（8 方向 / min-max / 父框夹回 / 跨帧保持 / 节点不膨胀）

```
  grew_r=1  grew_l=1  grew_b=1  grew_t=1
  min_ok=1  max_ok=1  clamped=1  persist=1
  measured=4  total=4
  result=1
```

### `gui cvdirty`（子矩形标脏，区域外像素差=0）

```
  first=1  inside_miss=0  outside_bad=0  area=0  rects=0
  us=448  frame_us=448
  result=1
```

回归关键项：`gui cv` result=1、`gui cvchan` result=1、`gui hit` z 高者优先 +
父块角 3px 内返回 RESIZE、`idem` 哈希 `0x88751F07` match=1（rebuild vs 增量一致）、
`leak` leaked=0。

## 4. 样例截图（`-vga std`，绝对路径）

| 样例 | 状态 / 命令 | 截图 |
|---|---|---|
| 仪表盘叠卡（三张错层 z 序） | `shot-free-dash` | `/home/user/work/artifacts/gui-v2/shots/free-dash.png`（`.ppm` 同目录） |
| 右侧固定侧栏 + 中央自适应 | `shot-free-sidebar` | `/home/user/work/artifacts/gui-v2/shots/free-sidebar.png` |
| 悬浮球 + 百分比定位徽标 | `shot-free-float` | `/home/user/work/artifacts/gui-v2/shots/free-float.png` |
| 溢出三态像素断言 | `shot-free-overflow` | `/home/user/work/artifacts/gui-v2/shots/free-overflow.png` |
| 左手柄缩放像素断言 | `shot-free-interact` | `/home/user/work/artifacts/gui-v2/shots/free-interact.png` |

## 5. 像素断言基线（`tests/gui_pixel_assert.py`）

- **`free` 组 3/3 PASS**（overflow 样例）：重叠区 (80,330) 只显示 z 高者 BAR；
  旧矩形 (120,120,80,60) 恢复父面板底色（panel 像素 4800 ≥100）；新矩形
  (300,220,80,60) 显示子块 BAR（4524 ≥100）。
- **`interact` 组 3/3 PASS**（左手柄缩放）：左边界拉伸到 180（BAR 像素 4802
  ≥100）；右边界内部 (388,140) 仍为 BAR；右边界外 (396,150) 非 BAR（右边界
  保持 390 不动）。

## 6. 不退化与性能

| 指标 | 值 | 判据 |
|---|---|---|
| `gui drag` 帧耗时 | 1551 µs（≈1.55 ms） | ≤4 ms PASS |
| drag damage | 9600 px ≈ 2.0% 屏（1 个 damage 矩形） | ≤25% 屏 PASS |
| 布局节点（单块拖拽） | measured=2 / subtree=2 / total=5 | 节点不膨胀 |
| 幂等哈希 | `0x88751F07` match=1，round 19 一致 | rebuild vs 增量一致 |
| 33fps 节流 | `rate` double_commit=0 | 不退回重复提交 |
| 链接布局 | `check_link_layout.py --strict` RC=0；`_bss_end=0x020ce7e4`（界 `0x02800000`） | 页池走 allocator，无新增 `.bss` 帧数组 |

## 7. 三门禁（终版 ISO，独立端口 4516/4517/4518，私有 scratch）

| 门禁 | 结果 | 耗时 | 日志 |
|---|---|---|---|
| vprobe（跨页系统调用） | **10/10 PASS** | 157.7 s | `probes/logs/gate-vprobe-v2final.log` |
| E3（任务系统调用 staging buffer） | **2/2 PASS**（badptr + conc） | 80.8 s | `probes/logs/gate-e3-v2final.log` |
| F11（`$C$` 摘要/权限/掩码） | **9/9 PASS** | 162.0 s | `probes/logs/gate-f11-v2final.log` |

> F11 首轮曾出现两例瞬时 FS 挂载竞争（`/etc/passwd` 读不到、digests=[]），
> 清空 `/tmp/catos_f11_v2/` 暂存后独立复跑 9/9；v2 未触碰 acl/users/passwd/TLS
> 路径，判定为环境竞争而非回归。

## 8. 交付指纹

- ISO：`kernel_v25/catos.iso`
- 字节：**12,011,520**
- sha256：`30fb6486489e50dbf69b3fa5feb8b6f267b52509767b723ad4ed25afd365da20`
- `_bss_end`：`0x020ce7e4`

## 9. 边界与砍项

- 不做：alpha / 圆角 / 阴影 / 半透明；位图不缩放；SCROLL 仅平移内容不做惯性
  滚动条；CE 声明式接入、`blk_commit` 画布记录带 `res` 归后续通道。
- 命名禁令：GUI 源码/文档不出现 `window`/`win_id`；CE ABI 0–68 槽位不动，新槽
  从 69 起；未引入 GPL/AGPL；未改交付盘 `catos_disk.img`；未改版本号。
- 合并口径（主线收尾）：枚举取并、v2 语义为准；hotfix 的 `con_gui_owned` 闸门、
  `gui_begin` 收摊顺序、`con_set_gui_mode(1)` 只在真进桌面时设，均不得丢。

## 10. 遗留 / 未由 v2 处理

- 合并进 `feat/gui-blocks`、版本号 v2.80、双 ISO 产物由主线负责；v2 不碰。
- 浏览器 `<img>` 解码、HTTPS 握手、11a 探针后端统一等沿用 v1 遗留项，不在 v2 范围。
