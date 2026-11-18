# CatOS 回归端口分配与基线豁免登记

本文件记录两件事：①每个回归套件**固定**的 monitor 端口；②与代码无关、长期存在的
**基线豁免条目**（终验判绿时按本表扣除）。

## 1. 固定端口分配（禁止动态抢端口）

套件的 monitor 端口必须固定。并发 clone / 并发子代理在本机同时跑时，一旦某个套件
"抢"到别人在用的端口，症状不是端口冲突报错，而是伪装成代码故障：

- `HMP empty reply`、`screendump produced no file`（size 类）
- `FAIL: prompt never appeared`（qemu_driver 类）
- `ConnectionReset`（长尾套件）
- 个别 ACL/权限步骤莫名 FAIL（组读取等）

| 套件 | 端口 | 备注 |
|---|---|---|
| qemu | 4707 | |
| shell_guess | 4450 | 不接 `--port` 也能跑，但显式传入保持一致 |
| fsrec | 4711 | |
| lfn | 4712 | |
| recur | 4713 | |
| jobctl | 4468 | |
| group | 4469 | |
| daemon | 4467 | |
| caps | 脚本内固定 | 不接受 `--port` |
| user | 脚本内固定 | 不接受 `--port` |
| disk | 4714 | |
| env | 脚本内固定 | 不接受 `--port` |
| heap | 4716 | 不需要 ISO |
| size | **4492（写死）** | 端口写死在脚本内，改端口需临时改文件并还原 |
| ce | 无 QEMU | 宿主侧解释器套件 |
| videomode | 4718 | v2.56 新增 |
| fs_badname | 4719 | v2.59 新增 |
| mouse | 4720 | v2.57 新增 |
| panic | 4546 | v2.59 新增（测试内会自选空闲口时以 4546 为默认） |
| pager | 4721 | v2.59 新增（落地后启用） |
| ime | 4722 | v2.58 新增（落地后启用） |

纪律：`tests/run_full_regress.sh` 用 `flock tests/regress_logs/.regress.lock` 做互斥，
两个 clone 同时跑 sweep 时后来者直接退出（exit 3），不并行争端口。各 clone 内部做
局部验证时同样只用本表端口，不要临时改端口（历史上临时改端口后忘记还原，导致
`git status` 里出现"测试端口被改"的噪声 diff）。

## 2. 基线豁免条目（不属于本轮代码回归）

| 条目 | 现象 | 判定依据 | 何时修 |
|---|---|---|---|
| `gfx24_regress` 用例 `"/bin/gfx24.ce installed"` | 11/12，唯一 FAIL 是这条 ls 断言 | `ls` 输出 39 条溢出 25 行文本缓冲，测试读窗口截断；像素/绘制步骤全 PASS；在基线 ISO 上同样 FAIL | 分页器落地后，`ls` 分页 + 该用例改为分页读取，届时一并转绿 |
| `svga24_regress` 2 条背景色断言 | 期望旧主题 `(16,32,64)`，实跑 `(12,12,12)` | v2.37 起主题色已改，std 的 gfx24 套件同点位 PASS | 要么更新旧断言，要么把主题色列为可配置；属断言过期，不是渲染回归 |
| `ce_regress` 11 FAIL | `ce_sim` 未建模 `0x6a/0x0a/0x08`、`ff /6` 等 opcode | 宿主侧解释器套件，本轮未碰 `ce_sim` / `.ce` 源 | 需要一次专项 ce_sim opcode 补全 |
| `size_regress` large 帧捕获偶发卡住 | 卡在 large 步的 `screendump` | 与端口争用/既有 screendump 时序相关；隔离端口串行跑可稳定 7/7 | 属环境性问题，串行 + 固定端口后不再出现 |

## 3. 已知内核限制（与测试相关）

见 `docs/problem.md` #50（dispi/LFB 图形模式下 `0xB8000` 文本平面不可被 monitor `xp`
读回）——旧文本读屏套件只能靠驱动 `force_text()` 盲发 `text`+Y 切回文本后端，或在磁盘
预置 `/etc/videomode=text`。
