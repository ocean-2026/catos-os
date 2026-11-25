# blkhtml — CatOS「HTML → 块树」转换工具组

纯 Python 3 标准库，**零第三方依赖**（不 pip install，不用 Pillow/lxml/bs4；PNG 由
`zlib` + `struct` 手写）。

唯一字段契约：[`schema.json`](schema.json)（`catos.blk/1.0`）；内核事实与三条
预览口径见 [`FROZEN.md`](FROZEN.md)。四个工具只认这一份字段定义，不在脚本里
另立字段名或样式表。

## 目录

```
tools/blkhtml/
├── schema.json      # 冻结契约（字段名/取值/规则 ID 不许改、不许加）
├── FROZEN.md        # 已核实的内核事实、预览口径、红线
├── blkcommon.py     # 内部共享库（枚举/调色板/字模/度量/贴边判定），非 CLI
├── html2blk.py      # 受控 HTML 子集 → .blk.json
├── blk2bin.py       # .blk.json → recs.bin + strings.bin（+ --emit-c）
├── preview.py       # .blk.json → 单文件 HTML / 纯净 PNG（app|calm|design）
├── validate.py      # C1/G1/G2/B1/B2/R1/S1/S2/D1/T1/L1/O1/P1 逐条断言
├── tests/
│   ├── run_tests.py # 测试入口（等价 unittest discover）
│   ├── test_blkhtml.py
│   └── fixtures/sample.html
└── README.md
```

## 快速开始

```bash
# 1. HTML → 块树 JSON
python3 html2blk.py page.html --out page.blk.json --title "My Page" --theme dark

# 2. 校验（任一 FAIL 退出 1）
python3 validate.py page.blk.json

# 3. 二进制记录 + 串池（--stats 看配额；--emit-c 出内核内置页 C 片段）
python3 blk2bin.py page.blk.json --out-prefix page --stats --emit-c page.c

# 4. 预览：单文件 HTML 与 PNG（默认 app 口径）
python3 preview.py page.blk.json --html page.html --png page.png --mode app
```

## 四个脚本

### html2blk.py — HTML → .blk.json

解析 `schema :: html_subset` 受控子集：`div/span/button/input/img/hr` + 内联
`style` + 同文件 `<style>` 里的受限 class 规则。

- 8pt 量化后计算绝对 `rect`；坐标/尺寸均为 8 的倍数。
- 实现 `flush_rule`：贴边侧对应两角 radius 归零、贴边侧 stroke 省略、贴边侧
  shadow 抑制；父子共享边只保留 1px 分隔线（role=line）。
- 对 `css_properties.unsupported` 命中的属性逐条输出
  `UNSUPPORTED <selector> <prop>`，**默认非 0 退出**；`--allow-lossy` 才降级为
  warning 且退出 0。
- 声明层入口（HTML 内唯一的样式外通道，schema :: `html_subset.attributes.data-blk`）：
  `data-blk-layer`、`data-blk-anchor="R,T"`、`data-blk-overflow`、`data-blk-drag`、
  `data-blk-drag-bounds`、`data-blk-resize="R,B"`、`data-blk-resize-edge`、
  `data-blk-bleed=true`、`data-blk-role`、`data-blk-z`、`data-blk-text-overflow`。
  resize / bleed / 锚点 / 层 / 溢出 / 拖拽一律由 HTML 单入口表达，不再事后回填 JSON。
- 每个节点写出 `ce` 九字段镜像（id/parent/kind/layer/req_w/req_h/text_off/
  pix_off/pix_w/pix_h/res）。

参数：`--out`、`--allow-lossy`、`--json`、`--title`、`--theme dark|light|hc`、
`--frame-w`、`--frame-h`。

### blk2bin.py — .blk.json → 二进制

- `recs.bin`：`40*n` 字节，`pad0=0`；`text_off` 为串池内偏移或 `-1`。
- `strings.bin`：按首次出现顺序拼接，同文本去重，NUL 结尾。
- 偏移写入后**回读校验**（不是估算）：每个 `text_off` 指向 NUL 结尾且内容与
  原文一致，否则报错。
- `--emit-c`：产出内核内置页可用的 C 片段（`static blk_spec` / `blk_desc`
  数组 + `blkgen_commit()`）；超集字段按 `blk_spec` 逐字段赋值：anchor 位、
  pct、px、min/max、m_*、overflow、drag/resize、scale。颜色角色名写入
  `role_fg/role_bg/role_accent` 枚举（`ROLE_FG/ROLE_BG/ROLE_PANEL/ROLE_BAR/
  ROLE_ACCENT/ROLE_BORDER/ROLE_LINE`），颜色值为 `INHERIT_COLOR`；只有
  `#RRGGBB` 字面量才固化进 `.fg/.bg/.accent`。`bleed` 由运行时 `block`
  结构持有，不写进 `blk_spec`。
- `--stats`：记录数、池字节、与配额 256 / 4096 / 32768 的比较。

参数：`--out-prefix`、`--emit-c OUT.c`、`--stats`、`--patch-json`（把回填的
`text_off` 写回 JSON 的 `ce` 镜像）。

### preview.py — 预览渲染

`.blk.json` → 单文件 `--html` 与纯净 `--png`。文字逐像素解析
`font_ascii.inc` 绘制（不用系统字体近似）；度量与 `decl_text_width()` 一致：
ASCII 8px、双字节 16px，`scale` 1/2 各乘 2；超宽按 ellipsis 截断。颜色只用
`schema :: token_palette`。

三口径（`--mode`，严格按 FROZEN.md §3）：

| 模式 | 画什么 | 对应内核状态 |
|---|---|---|
| `app`（默认） | 直角 + 1px 描边 + 无阴影 | `/etc/style=app`（`calm_shape_eligible` 全 false） |
| `calm` | 尺寸阶梯 8/12/16/24 超椭圆 n≈5 圆角 + `paint_halo`（WORK 接触层 1px@y+1；OVERLAY/TOAST 双层 y+3/y+4，黑 18%/10% 按底色混） | `/etc/style=calm` |
| `design` | 逐角 `radius` + `shadow.sides`，并执行 flush_rule | 目标态，**内核暂不具备** |

> QEMU 截图对照只允许同口径比：`app`↔style=app，`calm`↔style=calm；
> `design` 只做「设计 vs 内核」差异清单，不冒充同布局。

### validate.py — 规则断言

逐条打印 `PASS/FAIL <规则ID> <对象> <细节>`；任一 FAIL 退出 1。

| 规则 | 含义 |
|---|---|
| C1 | id 唯一且 >=1（根 id=0），parent 存在，无环，块数 ≤256，深度 ≤16 |
| G1 | rect 的 x/y/w/h 均为 8 的倍数 |
| G2 | padding/gap/margin 取值属于 allowed_spacing |
| B1 | `overflow=clip` 时子块完全落在父内容矩形内 |
| B2 | 子块不得越出父块外框（visible 亦然） |
| R1 | 贴边侧对应两角 radius 必须为 0 |
| S1 | 贴边侧不得声明描边 |
| S2 | 贴边侧不得启用阴影 |
| D1 | 贴边共享边只允许 1px；父子不得同侧都画 ≥2px |
| T1 | 文本按字模度量能放进内容矩形（或声明 ellipsis） |
| L1 | layer 合法；`--ce` 口径下 layer 只允许 work、kind 必须 ∈ COLUMN..CANVAS |
| O1 | `overflow=scroll` 时 scroll 偏移 ≤ 实际溢出范围 |
| P1 | 二进制：`text_off/pix_off` 落在池内或 -1，recs 长度 = 40*n，pad0=0 |

参数：`--json`（机读结果）、`--ce`（CE 投影口径）、`--bin --recs R --strings S`
（对 `recs.bin/strings.bin` 做 P1 复核）。

## 字段对应（schema ↔ 内核）

- **CE 槽 72 可还原**（`ce_blk_rec_t`，9 字段镜像）：id、parent、kind、
  layer（仅 work）、req_w/req_h、text、pix/res。
- **CE 通道不携带**（只进 `--emit-c` 的内核内置页 `blk_spec`）：rect 绝对几何、
  padding/gap/align、anchor/px/pct/min/max/margin、overflow/scroll、
  drag/resize、bleed、z、fill/fg/accent/stroke、radius/radius_mode、shadow、
  font.scale、visible/focusable/inert。

> 全保真路径是 `blk2bin --emit-c` 产出的内置页片段，**不是** CE 应用；槽 72
> 提交后几何与样式全部丢失。

## 布局对齐口径

`html2blk` 的 row/column 矩形求解照抄 `kernel_v25/src/gui/layout.c`
（`layout_row` / `layout_column`）：

- 主轴：`ALN_START` 起排；`ALN_CENTER` 整体居中；`ALN_END` 贴尾；
  `ALN_SPACE_BETWEEN` 在子块间平分剩余空间（`slot = extra/(n-1)`）。
- 交叉轴：`XALN_START` 落内容框起边；`XALN_CENTER` 取 `(bh-h)/2`；
  `XALN_END` 贴尾；`XALN_STRETCH` 拉满交叉轴。
- 文本叶子默认交叉轴落内容框顶（与内核实测 settings id3 文本 y=8 一致）。

## 已知偏差 / ABI 缺口

- **描边侧别**：`blk_spec` 只有 `border_w`、没有侧别掩码；内核 `body_border`
  对 `border_w>0` 一律画四边。校验器与 design 预览里的「贴边侧省略描边」只在
  design 预览成立；内核内置页会画四边（2px 共享缝）。这是 ABI 缺口，本轮不为
  它改内核。
- **交叉轴居中与 8pt 网格**：内核布局不归整子块坐标；工具 schema G1 要求
  x/y/w/h 为 8 的倍数。若居中/端对齐算出非网格坐标（如 48 高父框内 24 高
  子块 y=12），G1 会判 FAIL；样例 HTML 应选能落到网格上的档位，不在工具内
  放宽断言。
- **文本叶子交叉轴（center/end 档）未逐块真机复核**：默认 `XALN_START` 落
  内容框顶已对照内核实测；center/end 档下文本叶子与内核是否逐块相等尚未在
  真机上逐块验证。

## UNSUPPORTED 处理流程

1. 先看诊断行 `UNSUPPORTED <selector> <prop>` 指向哪个属性/标签。
2. **优先改 HTML**：去掉该声明，或换成 `css_properties.supported` 白名单内的
   等价写法（例如 `display:grid` → `display:flex; flex-direction:row`）。
3. 只有当该属性是临时原型损耗、可接受退化时，才用 `--allow-lossy` 降级为
   warning（退出 0）；不要为了跑通而放宽 schema 规则。
4. 做不到的字段在交付报告里列为缺口，不静默丢弃。

## 测试

```bash
python3 tests/run_tests.py        # 或 python3 -m unittest discover -s tests
```

覆盖：40 字节记录布局与 pad0、text_off 偏移与 -1 语义、串池去重、贴边四角
归零、UNSUPPORTED 非 0 退出、PNG 可被 zlib 解出且尺寸正确、design 与 app
像素差异存在、fixture 全链路；以及本轮回炉的 5 个回归用例：FREE 子树递归坐标、
`gap:4px` 解析、stroke 角色名渲染、scroll 越界 WARN、void 元素后接兄弟的 parent 归属。
