# D2 — CatOS TLS 1.2 客户端（HTTPS）报告

> 状态：**端到端通过（PASS）**。guest 内 `curl https://10.0.2.2:8443/` 取回固定串
> `CATOS_TLS_OK_278`（16 字节），screendump 证据见本目录 `tls_probe.png`。
>
> 安全口径（必须先读）：本 v1 客户端**只做加密，不做证书链 / 主机名 / RSA 签名校验**。
> 服务端自签证书被读取后丢弃，ServerKeyExchange 签名块读而不验。因此当前实现
> **可被中间人（MITM）冒充**：加密只能防被动窃听，不能防主动冒充。X.509 解析与
> RSA-2048 验签列入下一阶段。

## 1. 分支 / 基线 / 边界

- 工作树：`/home/user/work/catos-d2`
- 分支：`feat/d2-tls`
- 基线提交：`73c750f`（CatOS v2.77）
- 远端：**未 push**；未增删 git worktree；未触碰 `/home/user/work/catos-os`（main）、
  `/home/user/work/catos-d10`（D10 并行）。
- 未修改版本号 / banner、`docs/全局待办总表.md`、`docs/v2.77-变更说明.md`、README。

## 2. 总体结论

| 阶段 | 模块 | 状态 |
|------|------|------|
| 1 | AES-128-CBC + TLS 1.2 PRF（P_SHA256） | **完成**：NIST/SP800-38A 向量 + 随机 CBC 往返通过；PRF 对照公开算例逐字节一致 |
| 2 | secp256r1 ECDHE（vendor micro-ecc） | **完成**：主机侧与 `cryptography` P-256 参考逐字节一致 |
| 3 | X.509 DER 解析 + RSA PKCS#1 v1.5(SHA-256) 验签 | **未做（有意裁剪）**：v1 不验签，见第 7 节 |
| 4 | TLS 记录层（CBC MAC-then-encrypt、序列号 MAC、去填充、显式 IV） | **完成** |
| 5 | 握手状态机（CH→SH→Cert→SKE→SHD→CKE→CCS→Finished 双向） | **完成** |
| 6 | netstack HTTPS 分支接入（scheme/端口识别 + 调 TLS 取数） | **完成** |
| 7 | 验收脚本（自起 TLS 服务端 + QEMU + 自动判定 + screendump） | **完成**：`tests/tls_probe.py` |

协商套件：`TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256`（`0xC027`）；曲线 secp256r1。

## 3. 实现构成（文件清单）

内核侧：

- `kernel_v25/src/tls.c` — 记录帧收发、记录保护（protect/app_send/app_recv）、握手状态机、
  主密钥 / 密钥块派生、最小 HTTP 响应解析；主入口 `tls_https_get()`。
- `kernel_v25/src/tls_aes.c` — AES-128-CBC 薄封装 + `tls_prf`（P_SHA256）。
- `kernel_v25/include/tls_aes.h`、`kernel_v25/include/tls.h` — 接口声明。
- `kernel_v25/src/third_party/tiny-aes/aes.c` — vendor tiny-AES-c（CBC-only，ECB/CTR 关闭）。
- `kernel_v25/src/third_party/uecc/` — vendor micro-ecc（仅启用 secp256r1，`uECC_WORD_SIZE=4`）。
- `kernel_v25/src/netstack.c` — 传输钩子 `net_tcp_connect/net_tcp_send/net_tcp_recv`；
  `http_parse_url` 增加 `is_https` 出参并识别 `https://`；`http_fetch_once` 的 https 分支
  调用 `tls_https_get`；`tcp_do_send` 发送后推进 `tcp.seq`。
- `kernel_v25/src/stubs.c` — freestanding `memcpy/memset/memcmp/memmove` 全局实现。
- `kernel_v25/build.sh` — CFLAGS 加 `-DuECC_WORD_SIZE=4`；源清单与两条 LD 链接行编入
  `tls.o tls_aes.o aes.o uECC.o`。

测试 / 验收：

- `tests/tls_probe.py` — 验收脚本：起自签 TLS1.2 服务端（钉 `ECDHE-RSA-AES128-SHA256`），
  QEMU（pcnet/slirp，monitor 4501），自动 `netcfg dhcp` 后执行
  `curl https://10.0.2.2:8443/`，判定屏幕含 `CATOS_TLS_OK_278`，screendump 落本目录。

## 4. 主机侧已验证的基元

- **AES-128-CBC**：过 FIPS SP800-38A ECB/CBC 双向向量；100 组随机 CBC 往返通过。
  - FIPS key `00010203…0f`，pt `001122…eeff` → ct `69c4e0d86a7b0430d8cdb78070b4c55a`。
- **P-256 ECDH**：主机往返与 Python `cryptography` `exchange(ECDH(), …)` 逐字节一致。
  - 已知向量：priv `1234…7788`，对端公钥 `0474ee…ccce` →
    shared `8c33a62658d3b76d9abcfbebc815a1f4b35d0a4ad9f04818755bde6c1946b996`。
- **PRF（P_SHA256）**：对照公开算例（ulfheim Illustrated TLS 1.2）复算 master secret
  与 client write key，逐字节一致：
  - premaster `df4a291b…7624`，CR `0001…1f`，SR `7071…8f`
  - master `916abf9da55973e13614ae0a3f5d3f37b023ba129aee02cc9134338127cd7049781c8e19fc1eb2a7387ac06ae237344c`
  - client write key `f656d037b173ef3e11169f27231a84b6`

## 5. 调试中定位并修复的两个协议层根因

端到端首跑时服务端在我方加密 Finished 后回 `bad_record_mac(0x14)`。隔离后定位到两处
协议实现错误（与密码学基元无关）：

1. **PRF 的 A 链起点漏带 label**。RFC 5246 §5 中 `P_hash(secret, seed)` 的 `seed` 已是
   `label ‖ randoms`，`A(0)` 必须取完整 `label ‖ seed`；初版 `A(0)` 只用了随机种子，
   导致派生的主密钥 / 会话密钥错误。修复后 `tls_prf` 的 `A(0) = label ‖ seed`，
   主机算例复算 master 与 client key 全部一致。
2. **服务端 Finished 的 transcript 哈希漏纳客户端 Finished**。按 RFC 5246 §7.4.9，
   服务端 `server finished` 的 verify_data 输入哈希覆盖到**我方客户端 Finished 为止**的全部
   握手消息；初版复用了发送客户端 Finished 之前的哈希。修复后为服务端校验单独计算
   含客户端 Finished 的 transcript 摘要。

修复后记录层解密 / MAC 校验通过，服务端 Finished verify_data 校验通过
（串口检查点：`tls: srv fin ok`）。

## 6. 端到端验收

环境：云主机 `vm`（linux），guest 经 QEMU slirp，`10.0.2.2` 映射主机 loopback；
网络配置方式：**DHCP**（`netcfg dhcp`，租约成功，未退静态）。

复现命令：

```
cd /home/user/work/catos-d2
sh build.sh kernel && sh build.sh iso
python3 tests/tls_probe.py
```

- 验收脚本监听：monitor `4501`，TLS `8443`。
- 判定：guest 屏幕出现 `CATOS_TLS_OK_278`；脚本退出码 0，打印
  `PASS: guest fetched CATOS_TLS_OK_278 over TLS 1.2`。
- 证据截图：`artifacts/d2-tls/tls_probe.png`（PPM 源：`artifacts/d2-tls/tls_probe.ppm`）。
- 连续两次运行均 PASS。

## 7. 已知限制（v1 现状）

**安全（最重要）**

- **仅加密、未验签**：不解析 X.509 证书链，不做主机名校验，不校验 ServerKeyExchange 的
  RSA 签名；无任何固化信任锚。存在 MITM 冒充风险。下一阶段需补：最小 DER 解析取 RSA
  `n/e`，2048 位 RSASSA-PKCS#1 v1.5（SHA-256）验签，以及信任锚策略。
  裁剪理由：仓库现有 `bn_t` 仅 512 位（shell 包签名用），2048 位验签需另起大数，
  不值得挡在端到端打通之前。

**协议 / 功能边界**

- 仅 TLS 1.2；仅套件 `0xC027`（ECDHE-RSA-AES128-CBC-SHA256）；仅曲线 secp256r1。
- 不支持：TLS 1.3、会话恢复 / 票证、重协商、ALPN、SNI、GCM/AEAD 套件、其它 CBC 套件。
- 单连接、单线程阻塞式；freestanding 无 malloc，全部使用静态缓冲。
- HTTP 响应解析为最小实现：仅 Content-Length（实际取可用长度）；**chunked 传输未实现**。
- 不做证书吊销检查、不做 OCSP。

**代码备注**

- `tls.c` / `netstack.c` 内保留了少量串口诊断 `ser_write`/`ser_hex` 面包屑（仅串口通道，
  不影响协议与界面）；收口时可按需移除。

## 8. 提交情况

- 全部改动**仅在本地**，未 push 远端。
- 既有本地提交（基线 `73c750f` 之上，由新到旧）：`12b21b3`（AES via tiny-AES-c + PRF）、
  `c64c11f`（vendor micro-ecc secp256r1）、`bf44ff1`（AES-128-CBC + PRF 基元）等；
  本轮记录层 / 握手 / netstack 接入 / 验收脚本的改动尚未分阶段提交。
