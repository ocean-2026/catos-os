# ADoFAI 关卡文件

本目录包含 CatOS adofai.ce 的内置节奏游戏关卡（`.tad` 格式）。

## 格式说明

`.tad` 是纯文本配置文件，adofai.ce 的 `parse_tado()` 解析器支持：

```
title: <游戏标题>
bpm:   <初始BPM>
tile  <角度> [twirl] [speed N] [bpm N]
```

- **角度**：`0`（上）/ `90`（右）/ `180`（下）/ `270`（左）/ `999`（保持当前方向 = midspin）
- **twirl**：翻转扫掠方向
- **speed N**：速度倍率百分比（如 `speed 150` = 1.5×）
- **bpm N**：绝对 BPM（从该 tile 起生效）

## 内置关卡

| 文件 | tiles | BPM | 特点 |
|---|---:|---:|---|
| `EASY.TAD` | 72 | 90 | 直线为主，入门级 |
| `NORMAL.TAD` | 132 | 120 | 中等复杂度，有 twirl 和 speed 变化 |
| `HARD.TAD` | 204 | 160 | 高频 twist + speed ramping |
| `MARATHON.TAD` | 200 | 180 | 超长挑战，混合所有技巧 |
| `DEMO1.TAD` | ~16 | 100 | 教程关卡（原内置） |
| `DEMO2.TAD` | ~24 | 140 | Twirl & Speed 演示（原内置） |

## 自定义关卡

1. 用文本编辑器创建 `.tad` 文件
2. 通过 adofai 菜单 `[5] LOAD CUSTOM LEVEL` 加载（需要先把文件放到 `/home/user/` 或 `/etc/`）
3. 或通过 `inject_any.py` 注入到磁盘镜像

## 难度建议

- **新手**：先玩 EASY → NORMAL
- **进阶**：HARD 适合练习 timing window
- **大师**：MARATHON 考验 endurance + precision
