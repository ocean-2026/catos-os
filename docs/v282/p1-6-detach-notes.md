# P1-6（A2.4）· 可拆卸性回归 — 验收记录

分支：`v282/p1-4-5-6-cde-lang-detach`，基线 `fbab3a7`。

## 1. 任务书与现实偏差（先说结论）

任务书步骤：`pkg install desktop` → `gui` 用一轮 → `pkg remove desktop` → 回纯 shell，`kernel.bin` 前后 sha256 一致、prompt 干净无残留。

**实机实测后的真实情况**（64 MiB 已入库镜像 `kernel_v25/catos_disk.img`，以下均为 guest 真实返回，落盘文本见 `artifacts/v282/p1-6/pkg_reality.txt`）：

- `pkg list`：列出 `bc` / `stats` / `rps`，**另有一条 `desktop [1.0] available - detachable graphical desktop environment`**。
- `pkg info desktop`：`name: desktop version: 1.0 file: desktop.catpkg status: available`。
- `ls /pkg`：`bc.cat  desktop.cat  pkgs.txt  rps.cat  stats.cat`（5 项）。
- 但当前构建源 `kernel_v25/build/pkg.list` **只打包 bc/stats/rps**；注释明确写明 `extapp/desktop.c` 仅作学习参考、不再打包进镜像，块化桌面由 `kernel_v25` 内核态 `gui/` 子系统承担。
- 即：shipped 镜像里的 desktop 条目是**旧版残留**（陈旧 `pkgs.txt` + 遗留 `desktop.cat` blob），不是当前构建仓库的一部分。全新格式化卷上 `pkg list` 只有 bc/stats/rps，**没有 desktop**（见 2.6）。

因此回归拆成两层：① 镜像残留 desktop 条目的 install/remove 包层生命周期；② 当前真正在用的内核态 `gui` 桌面进出残留观测；③ 用 `bc` 做可正常运行的等价可拆卸回归。

## 2. 验证命令与真实输出

探针：`tests/detach_probe.py`（可复跑，打印 PASS/FAIL 与 sha256）。

### 2.1 完整性基线

```
kernel.bin sha256 before: 88ab295c7a944fa40b17711107cea97cf847607a193caafebf6c16f994aac3b7
...
kernel.bin sha256 after:  88ab295c7a944fa40b17711107cea97cf847607a193caafebf6c16f994aac3b7
PASS kernel.bin sha256 unchanged
```

整轮 guest 会话（含安装/卸载/进桌面）前后，主机侧 `kernel_v25/build/kernel.bin` sha256 **完全一致**；guest 只写临时盘副本，不写内核镜像。

### 2.2 残留 desktop 包的 install/remove 生命周期

```
PASS pkg install desktop returns     pkg: installed desktop -> /bin/desktop.ce
PASS desktop marked after install    desktop [1.0] installed - detachable graphical desktop environment
PASS legacy desktop session no fault
PASS ps after legacy desktop: only shell task   tasks=1
PASS pkg remove desktop returns      pkg: removed desktop
PASS desktop back to available after remove      desktop [1.0] available - detachable graphical desktop environment
```

遗留 `desktop.ce`（`kernel_v25/extapp/desktop.c`，v2.69 时代参考应用）启动后用 Esc（`0x1B`，`desktop.c:121` 退出键）退回 shell；退回后 `ps` 只有 `TID 0 shell` 一个任务，无残留任务。

### 2.3 内核态 gui 桌面进出残留观测

```
PASS gui exits to shell, no fault
PASS gui leaves only shell task      tasks=1
```

- 进桌面截图：`artifacts/v282/p1-6/01-gui-entered.png`（块化桌面：CatOS Sample 窗口、Start 任务栏、时钟、鼠标指针）。
- 退回 shell 截图：`02-after-gui-shell.png`；`ps_after_gui.txt`：`TID 0 0 0 RUN shell --- 1 task(s)`。
- 观测结论：内核态 `gui` 进出后无残留任务，prompt 干净；桌面静态状态（perf 环形缓冲等）按既有设计在退出后保留可读，不产生新任务。

### 2.4 bc 等价可拆卸回归（当前仓库真实可装包）

```
PASS pkg install bc                  pkg: installed bc -> /bin/bc.ce
PASS bc runs: 12 + 30 = 42           12 + 30 = 42 [task 1 'bc' exit(0)]
PASS pkg remove bc                   pkg: removed bc
PASS bc unknown after remove         catos: unknown command: bc (did you mean: wc?)
```

截图：`03-after-bc-remove.png`。安装→运行一轮→卸载→命令重新不可用，闭环成立。

### 2.5 探针汇总

```
15/15 PASS        RC=0
```

### 2.6 全新格式化卷

```
PASS fresh volume: no desktop entry
PASS fresh volume: bc/stats/rps present
```

`fresh_pkglist.txt`：只有 bc / stats / rps，无 desktop。

### 2.7 门禁

```
$ python3 tests/e3_regress.py kernel_v25/catos.iso
PASS badptr / PASS conc        steps=2 ok=2 fail=0

$ python3 tests/vprobe_regress.py kernel_v25/catos.iso
steps=10 ok=10 fail=0
```

## 3. 未做 / 降级项及客观理由

- **任务书的 `pkg install desktop` 原样流程无法按当前仓库执行**：当前 `pkg.list` 不产 desktop 包；shipped 镜像里的 desktop 条目是旧残留。探针在残留条目上跑通了包层 install/remove，但遗留 `desktop.ce` 不是当前支持的桌面形态，未对其 UI 做功能验收。
- **包名/文件名不一致**：索引写 `desktop.catpkg`，盘上实际 blob 为 `desktop.cat`；安装路径未报“payload missing”，说明 `fs_resolve_file` 对后缀/桩名较宽容。该残留条目清理属于另一个议题（镜像播种数据），本轮未改。
- **遗留 CE 的鼠标 ABI 路径**：`desktop.c:110` 检测 slots 66-68 鼠标 ABI，缺失时直接返回 1；当前内核态桌面走 `gui/` 自己的鼠标子系统，两者不是一套接口。
- `ls /bin/<file>` 不能用于判断文件是否存在：当前 `ls` 把文件参数当目录读，报 “cannot read directory”；包是否已装以 `pkg list` 的 installed/available 状态为准。

## 4. 新发现的限制 / 坑（供问题清单汇总）

- shipped 64 MiB 镜像的 `/pkg/pkgs.txt` 与当前 `build/pkg.list` 不同步，残留了已退役的 desktop 条目；全新格式化卷才反映当前仓库内容。后续做镜像播种时应以 `pkg.list` 为唯一源重建索引。
- 遗留 CE 应用退出后，最后一个按键可能与下一条命令行粘连（本次观测到 `q` + `clear` 拼成 `qclear`，被 unknown-command 兜住，未造成残留任务）；探针在继续下一条命令前应先按一次回车排空。
