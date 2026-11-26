# blkhtml — CatOS「HTML → 块树」转换工具组

> 本 README 只讲怎么用；所有语义、可表达边界与判定理由以
> `../../docs/HTML-块树-转换规范.md` 为准，二者冲突时以规范为准。

纯 Python 3 标准库，**零第三方依赖**（不 pip install；PNG 由 `zlib` + `struct`
手写）。字段唯一契约：[`schema.json`](schema.json)；已核实内核事实见 [`FROZEN.md`](FROZEN.md)。

## 目录

```
tools/blkhtml/
├── schema.json      # 冻结契约（字段名/取值/规则 ID 不许改、不许加）
├── FROZEN.md        # 已核实内核事实、预览口径、红线
├── blkcommon.py     # 内部共享库（枚举/调色板/字模/度量/贴边判定），非 CLI
├── html2blk.py      # 受控 HTML 子集 → .blk.json
├── blk2bin.py       # .blk.json → recs.bin + strings.bin（+ --emit-c）
├── preview.py       # .blk.json → 单文件 HTML / 纯净 PNG
├── validate.py      # 规则断言 + --spec-audit 规范一致性审计
├── samples/         # 基线 5 样例 + samples/cap2/ 离线预览样例
└── tests/           # run_tests.py 入口 + test_blkhtml/preview/cap2_samples + fixtures/
```

## 快速开始

```bash
python3 html2blk.py page.html --out page.blk.json --title "My Page" --theme dark
python3 validate.py page.blk.json                                   # 任一 FAIL 退出 1
python3 blk2bin.py page.blk.json --out-prefix page --stats --emit-c page.c
python3 preview.py page.blk.json --html page.html --png page.png --mode app
```

## 四个脚本

### html2blk.py — HTML → .blk.json

参数：`--out`、`--allow-lossy`、`--json`、`--title`、`--theme dark|light|hc`、
`--frame-w`、`--frame-h`。命中 `css_properties.unsupported` 的属性逐条输出
`UNSUPPORTED <selector> <prop>`，**默认非 0 退出**；`--allow-lossy` 才降级为
warning 且退出 0。

### blk2bin.py — .blk.json → 二进制

参数：`--out-prefix`、`--emit-c OUT.c`、`--stats`、`--patch-json`。`recs.bin`
为 `40*n` 字节、`pad0=0`；`strings.bin` 按首次出现顺序拼接、同文本去重、NUL
结尾；偏移写入后回读校验，指向 NUL 结尾且内容与原文一致，否则报错。

### preview.py — 预览渲染

参数：`--html`、`--png`、`--mode app|calm|design`、`--theme dark|light|hc`、
`--font-src`、`--verbose`、`--frames DIR`（按 16ms tick 逐帧导出 PNG 序列）、
`--play`（打印每 tick 缓动进度表，不落图）。

离线渲染为确定性纯函数，所有结果可复核：单图走 `--png`，动画走 `--frames`
序列；不做不可复现的浏览器特效。已实现口径：ordered16 Bayer 16 档抖动（子树
alpha 乘法继承）、elevation 0..5 右下光源 9-slice 阴影（HC 换 1px 边框）、
逐角 / pill / auto 半径、arc / squircle 4×4 面积子采样、多行 wrap + ellipsis、
四档字号（8/16/24/32，bold 横胀、tabular 等宽）、7 条缓动 × 5 档时长、
popover 锚定→8px 钳制→四向翻转、内置 8 单色图标 + 四角色 tint、scrollbar 比例。

### validate.py — 规则断言 / 规范审计

参数：`--json`（机读结果）、`--ce`（CE 投影口径）、
`--bin --recs R --strings S`（对 `recs.bin/strings.bin` 做 P1 复核）。

- 规则 ID：`C1 C2 G1 G2 B1 B2 R1 S1 S2 D1 T1 L1 O1 P1`；cap2 映射诊断
  `M-CAP2-01..23`，cap3 形态诊断 `E_RADIUS_N_BAD` / `E_RADIUS_SHAPE_BAD` /
  `WARN_RADIUS_N_RANGE`，形态规则 `R-7.5-01..03`；诊断行首带 `[<ID>]`
  前缀，`--json` 结果每条含 `rule` 字段，便于 CI 过滤。
- `--spec-audit [--md <path>]`：审计规范 md 中的内核符号引用（`A-9-01` 文件
  存在 / `A-9-02` 符号在该文件 / `A-9-03` 未跨文件误引 / `A-9-04` 文档仍引用
  `blk_spec` 字段）；cap2/cap3 一致性 `A-9-05` 属性名三处不漂移 / `A-9-06`
  字段名两处不漂移 / `A-9-07` 枚举映射一致 / `A-9-08` 默认值一致 /
  `A-9-09` 诊断规则 ID 就位 / `A-9-10` 新 kind 枚举一致；任一 FAIL 退出 1，
  末尾打印 `SPEC_AUDIT fail=N total=M`。行号提示漂移只打 WARN，不判错。

## cap2 / cap3 能力扩展（v0.3）

命名纪律：**HTML 属性名以《HTML 原型开发规范 v3》为权威**，内核字段名以冻结
表为准（`FROZEN.md` §6），两者映射唯一；转换器 / 校验器 / `schema.json` / 规范
md 四处逐字一致，由 `A-9-05..10` 复核。

- cap2 属性：`data-alpha` `data-alpha-mode` `data-elevation` `data-radius`
  （+ `data-radius-tl/tr/br/bl`）`data-border-sides` `data-wrap`
  `data-ellipsis` `data-font` `data-bold` `data-tabular` `data-line-h`
  `data-anim` `data-ease` `data-anim-props` `data-anchor` `data-prefer`
  `data-max-h` `data-icon` `data-tint` `data-scrollbar` `data-scroll`
  `data-theme="hc"`；自定义标签 `<c-popover>`（`BLK_POPOVER=15`）、
  `<c-icon>`（`BLK_ICON=16`）。
- cap3 形态：`data-radius-shape="arc|squircle|g2"` → `corner_shape`；
  `data-squircle-n="4.5|5"` → `corner_n`（单位 0.1，仅 SQUIRCLE 有效）。
  形态整块级别，与逐角半径正交；CE `ce_blk_rec2_t` 只下发 `corner_shape`。
- 越界 / 枚举不匹配 / 引用解析失败一律带规则 ID 诊断，**禁止静默降级**；
  `data-alpha` 0–1 比例与 0–255 绝对值判别见规范 §4.12.2。

## samples/cap2 离线预览样例

五个样例各含 `.html` 源、转换后的 `.blk.json`、`out/` 预览图与像素断言；
哈希锚点与断言口径见规范 §9.56。

```bash
# 逐样例：转换 → 校验 → 出图
python3 html2blk.py samples/cap2/alpha-menu.html \
    --out samples/cap2/alpha-menu.blk.json --title alpha-menu --theme dark
python3 validate.py samples/cap2/alpha-menu.blk.json
python3 preview.py samples/cap2/alpha-menu.blk.json \
    --png samples/cap2/out/alpha-menu-app.png
# 动画帧序列
python3 preview.py samples/cap2/notepad-anim.blk.json \
    --frames samples/cap2/out/notepad-anim-frames
# popover 四向对照图（up/down/left/right 各一张）
python3 samples/cap2/export_flip.py samples/cap2/out
```

断言口径：几何 / 属性直接取块树字段；像素只取块体内部区域统计角色色点亮
比例，阈值由 alpha 覆盖率与几何尺寸推导，不写死整图 RGB。样例断言脚本
`tests/test_cap2_samples.py`，预览函数专项 `tests/test_preview.py`。

## 退出码与常见诊断

- 退出 0：全部 PASS（WARN 不计失败）；退出 1：任一 FAIL（html2blk 命中
  unsupported 且未加 `--allow-lossy` 同此）。
- `UNSUPPORTED <selector> <prop>`：HTML 用了受控子集外的属性/标签；优先改
  HTML 换成白名单等价写法，确需临时退化才加 `--allow-lossy`。
- `[G1]/[G2] ...`：rect 坐标尺寸非 8 的倍数；padding/gap/margin 不在允许档位。
- `[B1]/[B2] ...`：子块越出父内容框 / 父外框。
- `[R1]/[S1]/[S2]/[D1] ...`：贴边侧圆角 / 描边 / 阴影 / 共享边不合规。
- `[T1] ...`：文本度量放不下内容矩形（或未声明 ellipsis）。
- `[L1]/[O1]/[P1] ...`：layer 非法或 CE kind 越界；scroll 越界；二进制
  记录长度 / 偏移 / pad0 不合规。

## 测试

```bash
python3 tests/run_tests.py        # 或 python3 -m unittest discover -s tests
```

覆盖：40 字节记录布局与 pad0、text_off 偏移与 `-1` 语义、串池去重、贴边四角
归零、UNSUPPORTED 非 0 退出、PNG 解码与尺寸、design/app 像素差异、fixture 全链路，
cap2/cap3 全部属性 / 标签 / 枚举映射 / 边界值 / 诊断 ID 与 C2 值域，preview 离线
渲染专项（抖动覆盖率、9-slice 阴影、逐角/pill/auto 半径、wrap 省略、四档字高、
7 缓动落点、popover 四向、icon tint、scrollbar 比例、arc vs squircle 覆盖率差），
五个 cap2 样例端到端流水线 + 哈希锚点 + 像素断言，以及回归用例、
`--spec-audit` 审计（含 A-9-05..10）与诊断 ID 前缀单测。

## 语义与判定理由

语义解释、三条预览口径（app/calm/design）的渲染差异、CE 通道可表达 / 不可表达
字段、贴边规则定义、令牌表、布局对齐口径、ABI 缺口与已知偏差，一律见
`../../docs/HTML-块树-转换规范.md`（规则 ID M-4.3-01 等）；本 README 不重复。
