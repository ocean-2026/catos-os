# v2.82 P2-D7 · be_vbe 32bpp pan / MTRR-WC 逐像素探针 — 交付说明

分支：`v282/p1-1-pe-reloc`。范围：仅 `kernel_v25/src/be_vbe.c`、新增
`tests/vbe_pan_wc_probe.py`、本文件。**只验证，不改绘制行为**：自检函数挂在既有
`vbe_diag()` 末尾，未改动任何 paint 原语。

## 1. 实现

### 内核侧 `vbe_selftest()`（be_vbe.c，诊断专用）

1. **WC/UC 写后回读一致性**：在离屏最后一行写四个已知 32bpp 像素
   （红/绿/蓝/白），`sfence` + `wbinvd` 排空 WC 缓冲后回读，按 XRGB 低 24 位逐像素
   比对；不区分 WC/UC（两条路径都验证写→回读一致），结论行标注当前 MTRR 类型。
2. **pan 扫描线映射**（仅 `pan_rows != 0` 时执行）：在逻辑行 0 打标记像素，
   `vbe_scroll(1, bg)` 滚一行，回读物理行 `pan_off` 同位像素，验证标记跟随视口移动；
   包裹越界/回绕检查，不可测时记 n/a 不算失败。
3. 自检结束 `vbe_clear()` 复位视口并清屏，再在可见第 2 行（y=16..31, x=0..15）
   画结论色块供探针采样：
   - 绿 `0x00FF00`：回读 OK 且 pan 实测 OK；
   - 黄 `0xFFFF00`：回读 OK，pan 在当前后端 n/a；
   - 红 `0xFF0000`：任一像素不一致。

### 探针 `tests/vbe_pan_wc_probe.py`

- 启动 ISO → `gfx vbe` 切到 32bpp be_vbe → `vdiag` 触发自检；
- `screendump` 取 PPM，采样色块中心 `(8, 24)`，按颜色判定 PASS/FAIL；
- 截图落 `artifacts/v282/p1-2-d7/vbe-pan-wc.png`。

## 2. 验证命令与真实输出

```
$ sh build.sh iso
BUILD_RC=0
$ python3 tools/check_link_layout.py build/kernel.pe --strict
GUARD_RC=0

$ python3 tests/vbe_pan_wc_probe.py
swatch pixel at (8,24): (255, 255, 0) (800x600)
screenshot: artifacts/v282/p1-2-d7/vbe-pan-wc.png
PASS vbe-pe     yellow swatch: readback OK, pan n/a
steps=1 ok=1 fail=0
RC=0
```

## 3. 降级说明（pan 未在仿真中实测）

- 本机 QEMU `-vga std` 走 Bochs dispi 路径（svga_probe 只认 VMware 厂商 15AD）。
  实测 `pan_rows = 0`（`scroll=re-render`）：dispi 拒绝超出 600px 的虚拟高度，
  `vbe_enter_bochs()` 里 `DISPI_VIRT_HEIGHT` 回读不符即把 `pan_rows` 清零。
  可用 BAR 虽够大，但 std VGA 显示模式不支持超出一屏的虚拟纵向分辨率，因此
  **pan 扫描线断言在本仿真环境无法触发**，探针记黄块 n/a，不计失败。
- pan 路径的内核断言代码已就位（标记 → 滚一行 → 物理行回读比对），待有支持
  虚拟高度的显示后端（或真机/VMware 路径）时可直接转绿块。
- WC 状态：回读断言在当前 MTRR 类型下通过；探针未解析 `mtrr=WC/UC` 文本行
  （vbe 后端文本不回显 0xB8000，探针按色块判定），WC/UC 归类以 `vdiag` 串口/
  屏幕文本为准。

## 4. 回归

D7 改动后重跑：

```
$ python3 tests/e3_regress.py kernel_v25/catos.iso     # 2/2 PASS（见 p1-1/e3-d7.log）
$ python3 tests/vprobe_regress.py kernel_v25/catos.iso # 10/10 PASS（见 p1-1/vprobe-d7.log）
```

## 5. 新发现的坑

1. shell 显示模式命令是 `gfx vbe`（`vbe` 单独不是命令，dispatch 只认 `gfx`）。
2. workspace fs 上的磁盘镜像拿不到 QEMU OFD 写锁，必须先 copy 到 `/tmp` stage 再挂。
3. vbe/gfx 后端的 `con_write` 不回显 0xB8000，文本结论无法用 `xp` 读屏；
   内核自检结论需用固定位置色块 + PPM 像素采样来传递。
