# 合并后联合回归（E3 + F11，2026-11-23）

- 基线：`cf0ee35`（TLS v2.79 修复）
- 合并后 main：`day-e3`（`36739ea`）与 `day-f11`（`3ab1c17`）各以 `--no-ff` 合并
- 集成构建工作树：`/home/user/work/catos-integ`（基于合并后的 main，独立于主树，避免与 TLS 线构建互相覆盖）
- 产物：`kernel.bin` 1,165,084 B 级、`catos.iso` 11,868,160 B（`sh build.sh kernel && sh build.sh iso`，两次退出码 0）

本报告只记录**合并后**的联合验证；两条线各自的详细验收见
`artifacts/e3-syscall/REPORT.md` 与 `artifacts/f11-passwd/REPORT.md`。

## 1. 联合回归结果

| 套件 | 命令 | 结果 |
|---|---|---|
| vprobe（Issue #78 既有回归，8 用例 + 2 前置） | `python3 tests/vprobe_regress.py` | **10/10 PASS**（`steps=10 ok=10 fail=0`），日志 `vprobe.log` |
| E3 并发串用 + 恶意指针 | `python3 tests/e3_regress.py` | **2/2 PASS**（`PASS badptr`、`PASS conc`），日志 `e3_regress.log` |
| F11 场景一（格式/权限/掩码） | `python3 artifacts/f11-passwd/s1_format_perms.py` | **9/9 PASS**，日志 `f11_s1.log` |

F11 场景一实测要点（`f11_s1.log`）：

- 同口令 `AlicePass1` 的 alice / bob 记录均为 `$C$`，两条摘要与盐都不同：
  `$C$399cd7e12d4c245c$afe7a233…3760b`、`$C$2fcd5fc801b14681$3ce52556…fd0493`
- 非 root `edit -c /etc/passwd` → `cat: permission denied`
- 非 root `lsuser` 口令列为 `*`；root 可见 `$C$` 方案标记
- 重启后原口令仍可登录（分支验收场景二已覆盖）

### 1.1 首次跑 F11 场景一出现 3 条假 FAIL 的原因（已修复测试脚本）

`screen_text()` 返回的是含滚动历史的整屏文本，且 80 列屏宽的 `$C$` 记录会折行：

- `nonroot-lsuser-mask`：断言在整屏里搜 `$C$`，命中了**上一条 root `lsuser`** 的残留回显；
       实际 alice 段是 `0101 0101 * 00000000 alice`。
- `root-sees-C-records` / `salts-differ-same-pw`：正则要求单行 64 hex，折行后匹配不到，
       且由此推出 `digests=[]`。

已把断言改为「从最后一个提示符起截段」+「去空白后再匹配完整 `$C$` 记录」，
复跑得到 9/9 PASS；屏幕原文与截图见
`artifacts/f11-passwd/s1_*.txt` 与 `artifacts/f11-passwd/shots/s1_*.png`。
这是**测试脚本解析问题，不是内核行为失败**，如实记录以防误读。

## 2. 构建产物

合并后 main 的 `kernel_v25/build/kernel.bin` 与 `kernel_v25/catos.iso`
已用集成工作树的同名产物覆盖，二者均来自**合并后源码**，与提交一致。

## 3. 未做 / 限制（如实）

1. **D9（PE 基址重定位）本轮未做**：`deliverables/bc-bfloat16-diag.md` 在当前树中不存在；
   `spawn_load_pe()` 目前对非 `USER_IMAGE_BASE` 的 `ImageBase` 直接返回 `-5`（不是「装载后崩溃」），
   要补齐 `.reloc` 需要连带确认首选基址策略与 `bc.catpkg`（CE01）的真实复现路径，
   属另一次更大改造，按「宁少而完整」本轮不做。
2. **未发版本号 / 未复制 ISO 到仓库根**：`v2.79` 正由另一条线（TLS）收尾，
   避免同号冲突；合并后的可运行镜像在 `kernel_v25/catos.iso`，版本发布由主线统一打。
3. E3 的「基线 `cf0ee35` 跑同一并发测试必须 FAIL」未补做（未另建基线构建）；
   只验证了修复后 PASS（详见 E3 报告 §6）。
4. shell 级 `|` 管道的按键注入抖动（`|` = shift-backslash）未脚本化；
   管道内核读写路径已由 CE 双任务往返覆盖。

## 4. 复现命令

```sh
# 合并后构建（任一干净工作树，需预置 build/catos_work.iso）
sh build.sh kernel && sh build.sh iso

# 联合回归
python3 tests/vprobe_regress.py
python3 tests/e3_regress.py
python3 artifacts/f11-passwd/s1_format_perms.py
```

QEMU：`qemu-system-i386 -m 256 -no-reboot -cdrom kernel_v25/catos.iso -boot order=d -display none
-monitor tcp:127.0.0.1:<port>,server,nowait -vga std -drive file=<测试盘副本>,format=raw,if=ide`。
每个场景使用基准盘的私有副本，未修改仓库内 `catos_disk.img`。
