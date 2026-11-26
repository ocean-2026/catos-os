# P1-2 · H10 shell 层 `A | B` 管道无 SINK 行 — 作业记录（v2.82）

分支：`v282/p1-2-3-shell-text`，基线 `fbab3a7`。

## 1. 现象（基线复现）

基线镜像（未改源码）文本模式下：

- `hello.ce | sink_ce.ce`：生产者 `hello.ce` 的正文直接打到了控制台，
  右端消费者 `sink_ce.ce` **根本没有被启动**，屏幕上没有 `SINK got N lines` 行。
- 同一行第二次执行行为相同；`run /bin/hello.ce | /bin/sink_ce.ce`（`run` 路径）
  同样接不上管道。

基线抓屏转录（修复前，`tests/p12_repro_probe.py` 首轮输出，已删除）：

```
===== 3) hello.ce | sink_ce.ce =====
root@catos:/# hello.ce | sink_ce.ce
  Hello from CE!
  This is a native CatOS application.
  Time: 2026-11-26 14:29:56
  alloc test: abcdefghij
  [task 1 'hello' exit(0)]
root@catos:/#
```

（对比：无管道 `hello.ce` 的正文行就是上面这 4 行；管道场景里这些正文本应
进管道，不应出现在控制台。）

## 2. 根因

`shell_exec()` 在分发前先调用 `shell_line_noninteractive(arg, ...)`，内部
`parse_redir()` 把 `arg` 在 `|` 处截断（`*q = 0`）并记下 `pipe_to`。之后：

1. **外部命令分支**（`src/shell.c` 原 ~7599 行）重新声明局部
   `out_redir/pipe_to/append` 并对**已被截断的 `arg`** 再次 `parse_redir()`，
   本地 `pipe_to` 必然为 NULL → 走 `run_from_path()` 裸跑左端，管道从未接线。
2. **`cmd_run()`**（`run A | B` 路径）同样对截断后的 `arg` 二次解析，
   右命令丢失。
3. 即便接上管道，右端命令名也没有走 cwd/PATH 解析（左端有，右端没有），
   裸写 `sink_ce.ce` 直接 `task_spawn_ce` 报 `cannot load ... (err -2)`。

## 3. 改了什么（仅 `kernel_v25/src/shell.c` 管道解析/接线路径）

- `shell_exec()` 外层 7286 行已解析出的 `out_redir / append / pipe_to` 直接
  下传给外部命令分支与 `cmd_run()`，不再对截断串二次解析。
  - `cmd_run` 签名改为 `(char *arg, int uid, const char *pre_redir,
    int pre_append, const char *pre_pipe)`；未传入时保留本地 `parse_redir`
    兜底。
  - 外部命令分支删掉局部 `parse_redir` 重解析，复用外层解析结果。
- 新增 `resolve_pipe_target()`：右端命令按「绝对路径 → 当前目录 → `$PATH`」
  解析，无扩展名补 `.ce`，并过 `sec_exec_check` 执行权限闸口；与
  `run_from_path()` 的查找口径一致。
- 两条管道接线路径（外部分支、`cmd_run`）在 spawn 前先解析右端；解析失败
  报 `catos: unknown command:` / `run: cannot load '...'`，不启动生产者。

未触碰：`cmd_theme`(734 行)、`theme` 命令表(1718/6818/7375 行)；
`task.c` 管道 FIFO / EOF 语义未改。

## 4. 验证命令与真实输出

构建 / 护栏：

```
cd kernel_v25 && sh build.sh iso
  RC=0，build/kernel.bin 1,533,196 B，catos.iso 12,259,328 B
python3 tools/check_link_layout.py build/kernel.pe --strict
  RC=0（仅既有 arena-window 告警，无新增 error）
```

P1-2 回归（`tests/p12_pipe_regress.py`，文本模式实机，端口 4472）：

```
PASS plain hello prints body      body lines present
PASS pipeline reports SINK line   sink=['SINK got 4 lines']
PASS producer stdout not leaked to console producer text went to the pipe
PASS consumer counted 4 lines     SINK got 4 lines
PASS second pipeline reports SINK line sink=['SINK got 4 lines']
PASS piped ls matches plain ls listing plain=13 lines piped=13 lines
PASS builtin-left pipe spawns no consumer noise no SINK line on builtin left side

steps=7 ok=7 fail=0
P12_RC=0
```

门禁：

```
python3 tests/e3_regress.py kernel_v25/catos.iso
  PASS badptr / PASS conc — steps=2 ok=2 fail=0
python3 tests/vprobe_regress.py kernel_v25/catos.iso
  steps=10 ok=10 fail=0，VPROBE_RC=0
```

实机抓屏（`artifacts/v282/p1-2/`）：

- `01-hello-plain.png` — 无管道 `hello.ce` 基线控制台输出。
- `02-hello-pipe-sink.png` — `hello.ce | sink_ce.ce`：生产者正文不泄漏，
  `SINK got 4 lines`，双任务干净退出。
- `03-hello-pipe-sink-again.png` — 第二次管道复用管道槽，结果一致。
- `04-ls-plain.png` / `05-ls-pipe-builtin-left.png` — 内置命令 `ls /bin`
  与 `ls /bin | sink_ce.ce` 清单逐行一致（13 行），无消费者噪声。

## 5. 未完成 / 降级项

- **内置命令作为管道左端仍不流式进管道**：`ls`/`wc`/`help` 等 ring0 内建
  直接写控制台，不走任务管道；`A(内建) | B` 时右端消费者不会被拉起，
  左端输出与无管道版本一致（回归第 6/7 条已锁定该口径）。要支持内建 → CE
  管道，需要给 ring0 内建 stdout 增加一条管道捕获路径，属后续线，本轮不动。
- 管道生产者（后台任务）退出时的 `[task N 'name' exit(code)]` 横幅仍直写
  控制台（既有行为，后台任务一律如此），本轮未改退出横幅路径。

## 6. 新发现的内核限制 / 坑（供问题清单汇总）

- **shell 重定向/管道标记只能解析一次**：`shell_line_noninteractive` 在前
  置解析时会原地截断 `arg`，下游任何路径都不能再对同一 `arg` 二次
  `parse_redir`，否则右端命令静默丢失。新增 shell 接线路径时应复用外层
  解析出的指针。
- 管道右端命令 historically 不走 PATH/cwd 解析，只认 `run` 给出的绝对路径；
  裸命令名接管道会 `err -2`。本次已在两条接线路径补齐解析。
