# SSH-2 服务端宿主原型（内核移植前的可行性验证）

按用户"新工具先出原型"规则，在移植到 CatOS 内核前，先宿主验证：
**标准 OpenSSH 客户端能否连进一个从零实现的 SSH-2 服务端。**

## 结论：可行，已验证

```
$ python deliverables/ssh-prototype/verify_ssh.py
PASS system ssh connects + exec returns marker
PASS all SSH protocol stages completed
```

`sshd_min.py` 用最小算法集实现完整 SSH-2：
- KEX: `curve25519-sha256`（X25519）
- 主机密钥: `ssh-ed25519`
- 加密: `aes128-ctr`，MAC: `hmac-sha2-256`
- 认证: `none`（可扩展 password）
- 连接层: session channel + exec/shell + data/eof/close

## 文件

- `crypto_shim.py` —— 通过 ctypes 调宿主 libcrypto 的 X25519/Ed25519/AES-CTR；
  SHA-256/HMAC 用 Python hashlib。自测 4/4 通过。
- `sshd_min.py` —— 干净的最小 SSH-2 服务端（最终可工作版本）。
- `verify_ssh.py` —— 自动化验收：起服务 → 系统 ssh 连 → 断言 marker 返回。
- `sshd_proto.py` —— 早期调试版（保留，含逐字节诊断痕迹；不要用于移植参考）。

## 移植内核必须知道的关键点（都是踩坑换来的）

1. **exchange hash H**（RFC 4253 §8 + RFC 8731）：
   `H = SHA256(s(V_C) || s(V_S) || s(I_C) || s(I_S) || s(K_S) || s(q_c) || s(q_s) || mpint(K))`
   - `V_C/V_S` **去掉尾部 CRLF**（RFC 4253 §4.2）
   - `I_C/I_S` 是**完整 KEXINIT payload，含开头 msg 字节 20**
   - `q_c/q_s` 是 SSH string；**只有 K 用 mpint**（去前导零、最高位 set 补 0x00）
   - 上面每一项（含 V）都要带 4 字节长度前缀——**这是最初签名一直失败的根因**
2. **签名**：Ed25519 **直接对 H 签**（不是 `string("ssh-ed25519")+string(H)`），
   reply 里签名 blob = `s("ssh-ed25519") || s(sig)`。
3. **密钥派生**（RFC 4253 §7.2）：
   `key = SHA256(mpint(K) || H || "A".."F" || session_id)`，`session_id = H`。
   服务端解密方向 A/C/E、加密方向 B/D/F。**已与 paramiko 逐字节核对一致。**
4. **二进制包加密（最大的坑）**：`aes128-ctr`（SDCTR, RFC 4344）加密的是
   **整个包（含 4 字节 packet_length）**，不是只加密 body！而且用的是
   **跨包持续的 CTR keystream**（一个长生命周期 cipher 对象），不是每包重置 IV。
   - paramiko 源码 `packet.py:452` `out = block_engine.update(packet)` 是权威
   - "length 明文/每包重置 IV" 是错误理解，会导致第一个包对、后续全错
   - `sshd_min.py:CTRStream` 是正确实现（128-bit 大端计数器持续递增）
5. **MAC**（非 EtM）：`HMAC(mac_key, seq_nr || 明文packet)`，附在加密包之后。
6. **报文顺序**：版本串 → KEXINIT → KEX_ECDH_INIT/REPLY → NEWKEYS →
   **SERVICE_REQUEST/ACCEPT（易漏！）** → USERAUTH_REQUEST/SUCCESS →
   CHANNEL_OPEN/CONFIRM → CHANNEL_REQUEST(exec/shell) → DATA/EOF/CLOSE。
7. **padding**：`(4 + 1 + payload_len + pad) % 8 == 0` 且 `pad >= 4`——
   **必须算上 4 字节 length 字段**，否则客户端报 "padding error"。
8. **channel request body**：首字段是 `uint32 recipient_channel`，偏移 4 之后才是
   `string request_type`（早期读偏移 0 导致 rtype 空）。
9. **bytes vs str**：解析出的 rtype 是 `bytes`，比较要用 `b"exec"` 不是 `"exec"`。

## OpenSSH 10.3 客户端注意

默认首选后量子 KEX（mlkem768x25519-sha256），纯经典服务端需要在客户端指定：
`-o KexAlgorithms=curve25519-sha256 -o HostKeyAlgorithms=ssh-ed25519
 -o Ciphers=aes128-ctr -o MACs=hmac-sha2-256`
（内核版若只实现经典算法，真实用户连接时需同样指定，或后续补 PQ KEX。）

## 内核移植工作量评估（供决策）

原型验证的是"协议可行"，内核落地还需：
- TCP **被动监听/accept**（现有 netstack 只有主动连接的客户端 TCP）
- SHA-256、HMAC-SHA256、AES-128-CTR、X25519、Ed25519 的 C 实现
  （内核现仅有 RSA-512 bigint 用于 pkg 验签，无 SHA/AES/曲线）
- 把 shell 的输入输出接到 SSH channel（exec/shell）
- 密钥/主机密钥持久化（可复用 hive）
- 完整的连接状态机、重连、超时、多会话

这是一次完整的协议栈工程，建议分期，每期过 QEMU 实机回归。
