# IME 自动复现（命令跑完候选框又弹出来）修复报告

日期：2026-11-23
分支：`fix/ime-autotoggle`（基于 `main` 3e6957e）

## 症状

用中文输入法打了几个字、关掉之后，此后**每执行一条有输出的命令**，底部又会冒出输入法候选框
（分隔线 `----…` 加 `1/2 1. * 2. …` 候选行），看起来像"输入法自己弹出来"。

## 根因（不是误触发键盘，是状态没清）

`con_status_bar2()` 在借用底部两行时置 `bar_active = 1` 并保存原屏幕；`con_status_bar_off()`
只把两行还原，**不清 `bar_active`**；`con_status_bar_forget()` 才会连状态一起清。

`ime.c` 关闭输入法的四条路径（F2 关闭 / Esc / `ime_refresh()` 未激活分支 / `ime_off()`）
全部只调了 `con_status_bar_off()`，于是 `bar_active` 一直是 1、候选文本还留在 `bar_s2` 里。
只要屏幕滚一行（任何打印输出的命令都会滚），`scroll_one()` 就 `con_status_bar_reapply()`
把这份**上一次的候选行**重新钉到底部 —— 这就是"命令一跑完输入法就弹出来"。

顺带一个相邻缺陷：`SHIFT_MARK_COMBO()` 只在"可打印字符"分支被调用，Enter / Backspace /
Tab / 方向键这些提前 `return` 的键不标记组合，于是 `Shift+Enter`、`Shift+Backspace`
按住 Shift 时松开会被当成"孤立 Shift tap"，直接开了输入法（v2.68 起单次 Shift tap 即生效）。

## 修复

`kernel_v25/src/ime.c`
- F2 关闭、Esc、`ime_refresh()`（未激活分支）、`ime_off()` 一律改为 `con_status_bar_forget()`：
  既还原被借用的两行，也丢掉 `bar_active` 与候选文本。

`kernel_v25/src/keyboard.c`
- `kb_readline()` 进入新 prompt 时：`if (!ime_active()) con_status_bar_forget();` —— 兜底，
  任何历史遗留的悬浮候选框都不会跨命令存活。
- 两个解码器（`kb_getkey` / `kb_getkey_timed`）在 Enter、Backspace、`sc_to_key` 命中、
  E0 扩展键这四条早退路径上补 `SHIFT_MARK_COMBO()`：Shift 与任何键同时按下都算组合，不再
  被误判为 tap。

## 验收

新增探针 `tests/ime_autotoggle_probe.py`（文本模式，串口/文本平面读取，端口 4595）：

| 用例 | 修复前 | 修复后 |
|------|--------|--------|
| C 控制组：F2 开 → 候选框出现；F2 关 → 消失 | PASS | PASS |
| A：F2 开 → 打 `hao` → F2 关 → 跑 `help`（滚动） | **FAIL**（底部重现 `----` + `1/2 1. * …`） | PASS |
| B：F2 开 → 打 `ni` → Esc 关 → 跑 `help` | **FAIL**（同上） | PASS |

日志：`artifacts/ime-autotoggle/before-fix.log`（rc=1）、`after-fix.log`（ALL PASS，rc=0）。

既有 `tests/ime_regress.py`：修复前 15/19、修复后 16/19，差异只在 vbe24 切换的时序抖动；
两条稳定 FAIL（`- = [ ]` 翻页用例、`Enter: raw pinyin committed`）在**基线镜像上同样失败**，
与本修复无关。其中 Enter 提交裸拼音会打出 `divide by zero` 内核崩溃转储（EIP 0x29），
已作为 B13 记入待办，未在本轮修。

## 复现命令

```bash
cd kernel_v25 && sh build.sh iso        # 产物 catos.iso 11,868,160 B，kernel.bin 1,142,264 B
cd .. && python3 tests/ime_autotoggle_probe.py kernel_v25/catos.iso     # ALL PASS
python3 tests/ime_regress.py kernel_v25/catos.iso                      # 16/19（3 条既存 FAIL）
```

探针需要 `kernel_v25/catos_disk.img` 存在（`qemu_driver.boot()` 会把它拷到临时目录再用，
原盘只读）；不在本工作树时临时软链到主树的盘即可。
