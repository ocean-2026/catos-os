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
