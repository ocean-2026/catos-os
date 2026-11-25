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
└── tests/           # run_tests.py 入口 + test_blkhtml.py + fixtures/
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
`--font-src`、`--verbose`。

### validate.py — 规则断言 / 规范审计

参数：`--json`（机读结果）、`--ce`（CE 投影口径）、
`--bin --recs R --strings S`（对 `recs.bin/strings.bin` 做 P1 复核）。

- 规则 ID：`C1 G1 G2 B1 B2 R1 S1 S2 D1 T1 L1 O1 P1`；诊断行首带 `[<ID>]`
  前缀，`--json` 结果每条含 `rule` 字段，便于 CI 过滤。
- `--spec-audit [--md <path>]`：审计规范 md 中的内核符号引用（`A-9-01` 文件
  存在 / `A-9-02` 符号在该文件 / `A-9-03` 未跨文件误引 / `A-9-04` 文档仍引用
  `blk_spec` 字段）；任一 FAIL 退出 1，末尾打印 `SPEC_AUDIT fail=N total=M`。
  行号提示漂移只打 WARN，不判错。

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
归零、UNSUPPORTED 非 0 退出、PNG 解码与尺寸、design/app 像素差异、fixture 全链路，以及回归用例、`--spec-audit` 审计与诊断 ID 前缀单测。

## 语义与判定理由

语义解释、三条预览口径（app/calm/design）的渲染差异、CE 通道可表达 / 不可表达
字段、贴边规则定义、令牌表、布局对齐口径、ABI 缺口与已知偏差，一律见
`../../docs/HTML-块树-转换规范.md`（规则 ID M-4.3-01 等）；本 README 不重复。
