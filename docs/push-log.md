# 远端推送记录（每版 commit 后实测）

命令固定形态（token 内嵌 URL，输出经 `sed 's/ghp_[A-Za-z0-9]*/ghp_***/g'` 脱敏）：

```sh
TOKEN=$(grep -oP 'oauth_token:\s*\K\S+' /home/user/.config/gh/hosts.yml)
git push https://x-access-token:${TOKEN}@github.com/ocean-2026/CatOS.git main
```

| 时间（CST） | 本地 HEAD | push 回显 | 远端校验 |
|---|---|---|---|
| 2026-11-18 21:4x | 8d42951 | `aae8640..8d42951  main -> main` | — |
| 2026-11-18 22:0x | cbf77c3 | `8d42951..cbf77c3  main -> main` | `ls-remote` = cbf77c3…（与本地 HEAD 一致） |
| 2026-11-18 ~23:5x | 614b396 | `cbf77c3..614b396  main -> main` | `ls-remote` = 614b3963… |
| 2026-11-19 ~00:1x | 1f84a69 | `614b396..1f84a69  main -> main` | — |
| 2026-11-19 ~01:2x | 6193adf | `1f84a69..6193adf  main -> main` | `ls-remote` = 6193adf5… |
| 2026-11-19 ~03:3x | 074933b | `6193adf..074933b  main -> main` | — |
| 2026-11-19 ~04:5x | 77e62ea | `074933b..77e62ea  main -> main` | `ls-remote` = 77e62ea4…（与本地 HEAD 一致） |
| 2026-11-19 ~05:2x | 834f5bf | `77e62ea..834f5bf  main -> main` | `ls-remote` = 834f5bf16d13…（与本地 HEAD 一致） |
| 2026-11-19 ~05:5x | 99af21e | `834f5bf..99af21e  main -> main`（含 merge 02c77e3 滚动返工） | `ls-remote` = 99af21ecd76d…（与本地 HEAD 一致） |
| 2026-11-19 ~06:0x | e8167f6 | `99af21e..e8167f6  main -> main`（ls 单行统一走 pager_line，修文件名重复） | `ls-remote` = e8167f6（复核通过） |
| 2026-11-19 ~07:0x | 8c123df | `e8167f6..8c123df  main -> main`（IME 补字母缓冲断言 19/19） | `ls-remote` = 8c123df3c3c7…（复核通过） |
| 2026-11-19 ~09:1x | e48f8f2 | `8c123df..e48f8f2  main -> main`（含 249e0a1 报告 §7、f437138 清误建目录、IME 光标格防御） | `ls-remote` = e48f8f2b55fa…（复核通过） |
| 2026-11-19 ~09:2x | f064bd6 | `e48f8f2..f064bd6  main -> main`（#211 根因定案） | `ls-remote` = f064bd6（复核通过） |
| 2026-11-19 ~09:3x | 5fd43d0 | `0c83e55..5fd43d0  main -> main`（报告 §8 双审结论） | `ls-remote` = 5fd43d061be6…（复核通过） |
