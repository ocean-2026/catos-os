# final3 — 按钮图片资源 + 内核极简 SVG 光栅化子集 取证索引

- 构建来源 commit：`4a1f5fc`（`feat(gui): 按钮图片资源 + SVG 光栅化子集`），基线 `3831ee7`。
- 探针：`tests/final3_icons_probe.py`（三/四场景独立开机播种磁盘）；像素断言：`tests/final3_pixel_assert.py`。
- 磁盘播种：宿主侧写 `/icons/ok.svg`、`/icons/folder.svg`；`missing.svg` 与 `/nope/x.svg` 故意不存在。

## 截图清单

| 图片 | 场景 | 关键断言 / 串口值 | 构建来源 commit | 文件路径 |
|------|------|-------------------|-----------------|----------|
| btn-svg-ok.png | `/icons/ok.svg`（蓝底 rect + 白色闭合 path 勾），16×16 | 文件 SVG 原色 `mono=0`；图标墨迹列 15、bbox x=5..19；文字 bbox x=117..131；图文列间距 98（不重叠） | 4a1f5fc | `artifacts/desktop-icons/final3/btn-svg-ok.png` |
| btn-svg-folder.png | `/icons/folder.svg`（两个黄色 rect），16×16 | `mono=0`；图标 bbox x=5..19；文字 bbox x=101..147；间距 82 | 4a1f5fc | `artifacts/desktop-icons/final3/btn-svg-folder.png` |
| btn-svg-fallback.png | `/icons/missing.svg`（不存在 → 回落内置 file 图标），16×16 | `mono=1`（内置单色）；图标 bbox x=8..15；文字 bbox x=97..151；间距 82；按钮文字保留、无卡死 | 4a1f5fc | `artifacts/desktop-icons/final3/btn-svg-fallback.png` |
| btn-icon24.png | `/icons/ok.svg`，`icon=24` 大图标 | `mono=0`；图标 bbox x=5..26（22 列）；文字 bbox x=105..151；间距 79 | 4a1f5fc | `artifacts/desktop-icons/final3/btn-icon24.png` |
| btn-notext.png | `/icons/folder.svg`，`label==""` 仅图标 | 图标水平居中：bbox x=107..121，中心 114 = 按钮中心 114 | 4a1f5fc | `artifacts/desktop-icons/final3/btn-notext.png` |
| btn-notext-bad.png | 坏路径 `/nope/x.svg` → 回落内置 cancel（X），文字 NoFile 保留 | `mono=1`；图标 bbox x=8..15；文字 bbox x=101..147；间距 86；串口仅 1 条 fallback（见下） | 4a1f5fc | `artifacts/desktop-icons/final3/btn-notext-bad.png` |
| btn-hover-icon.png | `shot-icons-hover`：首按钮注入 hover，验证 hover 背景下图标仍可见 | 首按钮图标区墨迹 288 像素，非空；按钮 hover 不改底色（反色仅 focused/pressed），反色路径由既有 e1 三态探针覆盖 | 4a1f5fc | `artifacts/desktop-icons/final3/btn-hover-icon.png` |
| btn-hover-icon-crop.png | 上帧首按钮裁切 | 同上，局部放大 | 4a1f5fc | `artifacts/desktop-icons/final3/btn-hover-icon-crop.png` |
| icons-full.png | 全量六按钮整帧（ok / folder / missing 回落 / Big 24 / 仅图标 / NoFile） | 六按钮均出框、无残影；审计逐按钮 ink=1、在父框内 | 4a1f5fc | `artifacts/desktop-icons/final3/icons-full.png` |
| desk-hover-gap.png | 既有桌面 hover 留白场景（前序轮次保留，未重采） | 沿用 round2/前序帧；不属本轮 SVG 改动 | 前序 | `artifacts/desktop-icons/final3/desk-hover-gap.png` |
| desk-selected.png | 桌面选中态 | 复制自 `round5/desk-selected.png`（来源轮次 R5，非本轮重采） | 前序 round5 | `artifacts/desktop-icons/final3/desk-selected.png` |

## 门禁数字（本轮实机 / 链接）

| 门禁 | 命令 | 结果 |
|------|------|------|
| 布局护栏 strict | `python3 tools/check_link_layout.py build/kernel.pe --strict` | **RC=0 通过** |
| vprobe 回归 | `python3 tests/vprobe_regress.py` | **steps=10 ok=10 fail=0** |
| E3 系统调用回归 | `python3 tests/e3_regress.py` | **steps=2 ok=2 fail=0**（badptr / conc） |
| GUI 像素回归 | `python3 tests/gui_pixel_regress.py` | **6/6 checks passed，RESULT=PASS** |
| 图标自检（内核 audit） | `gui icon-audit` | **slots=5 miss=5 fallback=2 result=1**（6/6 按钮 ink=1 且在父框内） |

## 坏路径 fallback 串口原文（坏路径 boot，仅一条）

```
GUI:img fallback path=/nope/x.svg reason=noent icon=cancel
```

- 坏路径 boot 中 `fallback` 行数 = **1**；缓存命中不重复打点。
- 审计会话内回落计数 `fallback=2`：`missing.svg`（file）+ `/nope/x.svg`（cancel）。

## 像素断言结论（`final3_pixel_assert.py`，RESULT=PASS）

- 图标区域非空：六按钮图标墨迹列均 > 0。
- 文字墨迹 bbox 与图标 bbox 不重叠：左图标按钮图文列间距 79–98 px；仅图标按钮居中于按钮中轴。
- hover 背景下图标仍可见：hover 帧首按钮图标 288 墨迹像素。
- 图标不越出父块：audit `inside` 判定六按钮全部在屏内（40,40 / 220×32 列），blit 另有父框夹紧。

## 功能自查四条

1. **解析失败不崩**：宿主 ASAN/UBSAN 桩测喂截断 XML、未知元素（gradient/filter/text）、超 64KB 源、超 64×64 输出，均安全回落内置/纯文字，无越界、无告警；实机坏路径 boot 正常到提示符、不卡死。
2. **缓存不越界**：固定 8 槽堆分配，灌入 12 个不同路径槽数封顶 8（LRU 淘汰最旧）；同路径同尺寸二次解析命中缓存，`miss` 不增。
3. **图标不越出父块**：audit 逐按钮 `inside` 判定通过；`button_icon_blit` 对 x/y 做父框夹紧。
4. **坏路径不卡死**：`/nope/x.svg` 与 `missing.svg` 均回落，按钮文字保留；串口每条真正回落仅一行。

## 本轮砍掉 / 未接线项

- **构建期预转位图轨**：双轨中的「构建期预转位图」留接口，本轮只落地内核内置光栅化。
- **`icon_pos` 变体**：字段预留（默认左图标），本轮未接右图标 / 上图标等变体。
- **CE 按钮记录槽位 / 声明语法 `img=` `icon=`**：本轮仅内核内耗字段、由 C 侧 `blk_spec` 填充；CE 记录槽预留未接线（见规格 §9.4）。
- **`polyline` / `ellipse`**：解析分支保留，验收图元未覆盖；主线保住 `rect / circle / path(M/L/C/Q/Z)` + 左图标右文字。
- **明确不支持**：gradient / clipPath / transform / filter / 文本 / pattern / 圆角（rect rx 忽略）；`stroke` 仅按 1px 等效折线。

## 未采 / 未闭环（禁止造假，未用空帧冒充）

- `clip-terminal.png`：本轮未新开终端批次；终端帧见 `final2/term-final.png`，独立 clip 截图未采。
- `loose-offscreen.png`：`gui loose` 探针结束即 `gui_end()` 不保留帧；部分出屏坐标已由前序 `round5/serial-loose.log` 串口取证，截图未采。

## 声明语法接入 img=/icon=（CE rec2 通道，本轮）

- `btn-decl-svg.png`：记录声明 `img=/icons/ok.svg`、`icon_sz=16`，文件 SVG 蓝勾 + `DeclSVG`，与内核内构建视觉一致。
- `btn-decl-fallback.png`：声明 `img=/nope/x.svg`，回落内置 cancel(X) + `DeclFB`；串口恰一条 `GUI:img fallback path=/nope/x.svg reason=noent icon=cancel`。
- `btn-decl-badicon.png`：声明 `icon_sz=99`（非法档位）被忽略回默认 16 档，`img=/icons/ok.svg` 正常出勾 + `BadIcon`，不崩。
- `decl-img-full.png`：三按钮整帧。串口 `GUI:decl-img btn=0/1/2 img=... sz=...`：sz=16 / 16 / 0（非法 99 清 0）。
- 接线入口：`validate2` 软校验（非法项跳过不整批拒绝）→ `decl_apply2` 落地 `block.img/icon_sz`；探针命令 `gui shot-decl-img`，脚本 `tests/final3_decl_probe.py`。
