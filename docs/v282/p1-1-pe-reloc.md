# v2.82 P1-1 · D9 PE/PE32+ 基址重定位 — 交付说明

分支：`v282/p1-1-pe-reloc`，基线 `fbab3a7`。
范围：仅 `kernel_v25/src/task.c`、新增 `tests/mkpe_reloc.py`、`tests/pe_reloc_probe.py`、本文件。

## 1. 缺陷与背景

`spawn_load_pe()`（task.c）原本只按 Optional Header 的首选 `ImageBase` 映射段，
**完全忽略 Base Relocation 目录（DataDirectory[5]）**。镜像被装载到与链接期首选基址
不同的实际基址时，HIGHLOW/DIR64 绝对引用仍指向首选基址，首次解引用即崩。这是
问题清单 #130「PE32 loader 未处理 .reloc」与 #二十八「bc 类 PE 绝对地址未修正」。

## 2. 改动内容（task.c）

1. **基址选择**：首选 `ImageBase` 落在用户区间 `[USER_IMAGE_BASE, USER_EXT_END - size_image)`
   内时，保持原位（与既有行为一致，既有 PIC 样例不受影响）；否则滑动到固定 legacy
   镜像槽 `USER_IMAGE_BASE`（`0x2000000`，堆 `USER_HEAP_BASE` 以下），并要求整镜像
   不越堆。重定位差值 `delta = load_base − pref_base`（int32）。
2. **解析重定位目录**：`NumberOfRvaAndSizes > 5` 时从 Optional Header 偏移 96+5*8
   读出 DataDirectory[5]（RVA, size）；文件越界返回 -5，读盘失败返回 -4。
3. **新增 `pe_apply_relocs()`**：遍历已拷入映射镜像的 `.reloc` 块
   （`IMAGE_BASE_RELOCATION {VirtualAddress, SizeOfBlock, entries[]}`）：
   - 类型 `0 ABSOLUTE`：跳过（填充项）；
   - 类型 `3 HIGHLOW`：32 位槽位 += delta；
   - 类型 `10 DIR64`：64 位槽位 += 符号扩展后的 delta（PE32 镜像高字应为 0）；
   - 其余类型：返回 -5，不静默跳过（与 ELF 引擎 reject-don't-skip 一致）。
4. **健壮性（畸形 PE 一律负错误码，不 ring0 崩）**：目录为空/零长度跳过；目录 RVA 越出
   `size_image`、块大小 <8 / 非 2 对齐、块越过目录尾、块页 RVA 越界、条目数超
   `PE_MAX_RELOC_ENTRIES(4096)`、目标 RVA 越出镜像、目标页物理地址解析失败，均返回 -5。
5. 段映射/拷贝/入口点全部改用 `load_base + RVA`；`opt_magic != 0x10b` 的 PE32+（0x20b）
   仍按现状拒绝，未放开。

## 3. 复现说明（补丁前）

- 任务提到的 `bc.catpkg` 经解析：内部可执行体是 **CE01**（非 PE），位于
  `kernel_v25/extapp/pkg/bc.catpkg`，前缀 `CPKG02` + `CE01`（偏移 0xcc）；
  本次树内没有可复现该历史崩溃的 PE 工件。因此实际复现/验收用的可执行体是
  `tests/mkpe_reloc.py` 现生成的两个 PE32 i386 样例（首选基址 `0x400000`，
  在用户区之外，强制滑动）：
  - `PELOC.EXE`：`.data` 槽存放指向消息串的首选基址绝对指针，一条 HIGHLOW(3) 重定位；
    重定位正确时输出 `PE-RELOC PROBE OK`。
  - `PELOCBAD.EXE`：目录声明 12 字节，块声称 136 字节（条目越界），必须被拒。
- **基线（fbab3a7 原样代码）实测**：`run /bin/PELOC.EXE` → 内核在首选基址校验处
  返回 -5，shell 打印
  `run: cannot load '/bin/PELOC.EXE' (err -5)`；未进入段映射/重定位路径，guest 存活
  回到提示符。即基线表现为「首选基址越界直接拒绝」，历史崩溃发生在更早一版把镜像
  强行装载到 `0x2000000` 而不修正绝对引用的装载器上；本补丁同时覆盖了这两条路径
  （滑动 + 差值修正）。

## 4. 验证命令与真实输出

工作目录 `work/v282-p11`。

### 4.1 构建
```
$ sh build.sh iso
OK -> build/kernel.bin
OK -> catos.iso
BUILD_RC=0
```

### 4.2 链接布局护栏（strict）
```
$ python3 tools/check_link_layout.py build/kernel.pe --strict
GUARD_RC=0
```
（仅有既有 arena-window 静态表告警，无新增 fatal。）

### 4.3 PE 重定位探针（两轮磁盘：format → 注入 → 跑例）
```
$ python3 tests/pe_reloc_probe.py
[reloc screen tail] run /bin/PELOC.EXE ... n PE conversion skipped; trying direct load.
  PE-RELOC PROBE OK [task 1 'PELOC' exit(0)]
PASS reloc        marker: PE-RELOC PROBE OK
[badreloc screen tail] run /bin/PELOCBAD.EXE ... n PE conversion skipped; trying direct load.
  run: cannot load '/bin/PELOCBAD.EXE' (err -5)
PASS badreloc     no fault, prompt back
steps=2 ok=2 fail=0
RC=0
```

### 4.4 门禁回归
```
$ python3 tests/e3_regress.py kernel_v25/catos.iso
PASS badptr       verdict PASS
PASS conc         verdict PASS
steps=2 ok=2 fail=0
RC=0

$ python3 tests/vprobe_regress.py kernel_v25/catos.iso
PASS format / bin-has-vprobe / fr / cat / hw / str / strcpy / wr / fw / pw
steps=10 ok=10 fail=0
RC=0
```

## 5. 实机截图（artifacts/v282/p1-1/）

- `p1-1-reloc.png`：`PE-RELOC PROBE OK`，任务 `exit(0)`。
- `p1-1-badreloc.png`：畸形重定位被拒 `err -5`，提示符正常返回，无 fault。
- `probe.log` / `e3.log` / `vprobe.log`：完整串口日志（*.log 被 gitignore，不入库）。

## 6. 未完成 / 降级项

- DIR64(10) 路径在代码中已实现并做边界校验，但探针样例只覆盖 HIGHLOW(3)；
  PE32 镜像上 DIR64 极少出现，未单独造 DIR64 样例上机。
- 历史 bfloat16 PE 工件不在本树内（bc.catpkg 为 CE01），未对其做回放；如后续拿到
  原始 PE，可直接放进同一探针流程复验。

## 7. 新发现的坑（供 history.md 汇总）

1. `tests/e3_regress.py` / `vprobe_regress.py` 的 `find_qemu()` 候选路径不含
   `/home/user/bin`，必须 `export PATH=/home/user/bin:$PATH` 后再跑，否则报
   `qemu-system-i386 not found`（RC=2）；自写探针若直接用 `find_qemu()` 需补该 fallback。
2. `ppm_to_png()` 内部已 `os.remove(ppm)`，探针不要再二次删除，否则
   `FileNotFoundError` 中断后续用例。
3. `/tmp` 会被其它代理的 stage 清理波及；探针日志/中间产物要落在 worktree 内。
4. QEMU HMP `screendump` 是异步的：监控回包先于文件落盘，需轮询等待 PPM。
5. 基线 PE 装载器对 `ImageBase < USER_IMAGE_BASE` 直接 -5；旧版「固定装到
   `0x2000000` 不修正引用」的崩溃形态在当前基线上已不可直接复现，复现时按
   实际错误码记录，不要套旧结论。
