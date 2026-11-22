# D2 — CatOS TLS 1.2 客户端（HTTPS）进度报告

> 状态：**未完成（WIP 半成品）**。本报告如实区分"已主机向量验证"与"卡住/未开始"，
> 不存在把未实现环节标记为已实现的情况。端到端 `curl https://…` 本次**未跑通**，
> 也没有可作为验收证据的 screendump 截图。

## 1. 分支 / 基线 / 边界

- 工作树：`/home/user/work/catos-d2`
- 分支：`feat/d2-tls`
- 基线提交：`73c750f`（CatOS v2.77）
- 远端：**未 push**；未增删 git worktree；未触碰 `/home/user/work/catos-os`、
  `/home/user/work/catos-d10`。
- 未修改版本号 / banner、`docs/全局待办总表.md`、`docs/v2.77-变更说明.md`、README。
- `kernel_v25/build.sh` 的源文件清单**未改动**；本次新增的 `.c` 尚未编入内核镜像
  （处于休眠状态，不影响基线内核构建）。

## 2. 总体结论

| 阶段 | 模块 | 状态 |
|------|------|------|
| 1 | AES-128-CBC 加解密 + TLS 1.2 PRF (P_SHA256) | **部分完成**：加密 + PRF 主机向量通过；CBC 解密往返失败（见 4.1） |
| 2 | P-256 (secp256r1) ECDHE | **阻塞**：字段乘法 / 求逆通过；点乘大标量错误、约减末尾死锁（见 4.2） |
| 3 | X.509 DER 解析 + RSA PKCS#1 v1.5(SHA-256) 验签 | **未开始** |
| 4 | TLS 记录层（分片/重组、CBC MAC-then-encrypt、序列号 MAC、去填充） | **未开始** |
| 5 | 握手状态机（ClientHello→ServerHello→Certificate→SKE 验签→ServerHelloDone→ClientKeyExchange→CCS→Finished 双向） | **未开始** |
| 6 | netstack HTTPS 分支接入（URL scheme/端口识别 + 调 TLS 取数） | **未开始**；明文 HTTP 收包循环与响应体提取**未做任何重构** |
| 7 | 验收脚本（自起 s_server + QEMU + 自动判定 + screendump） | **未开始**；无 e2e 证据 |

目标套件：`TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256`（0xC027）；
回退套件 `TLS_RSA_WITH_AES_128_CBC_SHA`（0x002F）尚未规划实现。

## 3. 已完成并在主机侧验证的部分

主机工具链：`gcc`（64 位主机，测试代码内 limb 仍为 32 位），验证用小程序为临时文件，
未纳入仓库。编译模式示例：

```
gcc -O0 -Ikernel_v25/include -Ikernel_v25/src /tmp/test.c \
    kernel_v25/src/tls_aes.c kernel_v25/src/crypto.c -o /tmp/x
```

### 3.1 AES-128-CBC 加密（NIST 向量通过）

文件：`kernel_v25/include/tls_aes.h`、`kernel_v25/src/tls_aes.c`

- Key `000102030405060708090a0b0c0d0e0f`
- PT  `00112233445566778899aabbccddeeff`
- 实测 CT `69c4e0d86a7b0430d8cdb78070b4c55a`
- NIST 期望 `69c4e0d86a7b0430d8cdb78070b4c55a` —— **一致**。

### 3.2 TLS 1.2 PRF（P_SHA256，与 Python 参考一致）

- secret=`secret`，label=`label`，seed=`seed`，输出 32 字节。
- Python（`hmac`/`hashlib` 自实现 P_hash）：
  `c9ffd20be61c615c34c4888a57c4203e9e2d3b6658e30c6aa812f9f4e5faca99`
- C `tls_prf` 输出：
  `c9ffd20be61c615c34c4888a57c4203e9e2d3b6658e30c6aa812f9f4e5faca99` —— **一致**。

### 3.3 P-256 字段层（部分通过）

文件：`kernel_v25/include/p256.h`、`kernel_v25/src/p256.c`

- 基点采用 `cryptography` 库 `ec.derive_private_key(1, SECP256R1())` 导出并复核在曲线上的权威常数
  （早期手抄的 Gx/Gy 中段错误，已用库值替换）。
- 字段乘法：`Gx² mod p` 与 Python 整数运算一致；随机归约输入的乘法在**未触发约减死锁的用例**上对照一致。
- Fermat 求逆 `a^(p-2) mod p`：`a · a⁻¹ ≡ 1 (mod p)` 与 Python 一致。
- 标量乘小标量对照 `cryptography`：
  - `priv=1` → `1·G` 的 X 坐标与基点 Gx 一致；
  - `priv=2` → `2Gx=7cf27b18…69978` 一致；
  - `priv=3` → `3G` 的 X/Y 均与参考一致；
  - `priv=4,5` 一致。

## 4. 卡点与未完成项（逐条）

### 4.1 AES-128-CBC 解密往返失败（阻塞记录层）

- 现象：同一 key/PT，加密得到正确 CT；`tls_aes128_cbc_dec`（以及块级
  `aes_dec_block`）解出的不是原文，块级往返 `memcmp != 0`。
- 已排除：
  - 加密方向与 NIST 向量一致（3.1）；
  - 逆 S-box 是双射（对所有 `i`，`inv_sbox[sbox[i]] == i`，`bad=0`）；
  - `mix_columns` → `inv_mix_columns` 单独往返一致，`shift_rows` →
    `inv_shift_rows` 单独往返一致（变换本身互逆）；
  - CBC 链路的 IV/prev 处理按标准写法。
- 收敛后的嫌疑：逆密码（equivalent inverse cipher）的轮次编排或轮钥使用顺序；
  单个变换均自逆，组合后仍解不出原文，尚未隔离到具体轮次。
- 下一步：取 OpenSSL/cryptography 的单块 ECB 中间状态，逐轮 dump 逆密码状态，
  对照定位；修复后补解密 NIST 向量与往返测试。

### 4.2 P-256 标量乘大标量错误 + 快速约减末尾死锁（阻塞 ECDHE）

- 现象：
  - `priv=6,7,16` 以及随机大标量的公钥 / ECDH 共享密钥与 `cryptography` 参考不符；
    小标量 1–5 通过（见 3.3）。
  - 主机模糊测试中，`vli_modMult` 在部分归约输入上进入**死循环**（超时 5s 无输出）。
- 已定位的根因之一：`mmod_fast` 末尾归一化的 `carry<0` 修复循环
  （`do { carry += vli_add(res,res,PP); } while(carry<0)`）中，`vli_add` 只返回
  0/1 的进位，无法把负的整数计数器带回非负，导致该分支**永远退出不了**。
  观测值：`preduce carry=-1 cmp=-1`，随后挂起。
- 已排除：乘法器 `vli_mult` 原始 16-limb 乘积正确；快速约减的 s1–s4 / d1–d4
  limb 索引已逐条对照 micro-ecc `curve-specific.inc`（WORD_SIZE=4）转录一致；
  求逆独立验证通过。
- 待办：
  1. 先把末尾归一化改为不依赖符号计数器的稳妥写法：残差按 `[0,p)` 比较后减法归约，
     并单独处理减法借位（借位则回加 p）；消除死锁。
  2. 归一化修复后，重跑归约输入随机乘法模糊测试（Python 对照），确保无死锁、结果一致。
  3. 再复核仿射点加 / 倍点：当前倍点斜率分子已按 P-256 `a=-3` 写为 `3x²−3`；
     仍需排查大标量下累积偏差的来源（疑似某次字段减法/加法未正确处理进位借位）。
  4. 通过判定：`p256_pubgen(1)==G`、`2G`、`3G`、随机大标量公钥，以及双方 ECDH
     共享密钥 X 坐标与 `cryptography` 的 `exchange(ECDH(), …)` 完全一致。
- 备注：当前点层用的是**仿射坐标 double-and-add**（便于审计），未使用 micro-ecc
  的 Jacobian ladder；字段运算可复用，点层可继续留在仿射坐标直至向量全绿。

### 4.3 尚未动工的模块及接手入口

- **X.509 / RSA 验签**：需最小 DER 解析，从证书 SPKI 取出 RSA `n/e`；用仓库
  `shell.c` 已有的 17-limb 小端 bignum（`bn_t`/`bn_mulmod`/平方-乘模幂）做
  2048 位验签；校验 RSASSA-PKCS#1 v1.5 的 DigestInfo（SHA-256 OID + 摘要）。
  仓库内此前**没有**现成 RSA modexp 可直接复用，只有 bignum 模式可参照。
- **记录层**：16 KiB 明文分片与重组；CBC 记录 MAC-then-encrypt（解密→校验 HMAC，
  序列号 + 类型 + 版本 + 长度 + 内容参与 HMAC-SHA256）；MAC 比较需常数时间；
  PKCS#7 去填充。MAC 基元 `hmac_sha256` 已在 `crypto.c` 提供。
- **握手状态机**：ClientHello（可带 SNI）→ ServerHello → Certificate →
  ServerKeyExchange（ECDHE 参数 + RSA 签名，需验签）→ ServerHelloDone →
  ClientKeyExchange → ChangeCipherSpec → Finished（双向校验）；主密钥/会话密钥
  由 PRF 派生。
- **netstack 接入**：严格遵守边界——只在 `http_parse_url`/scheme 识别处区分
  `https://` 与端口，在 `http_fetch_once()` 的 https 分支调用 TLS 侧取数函数；
  TLS 内部完成 HTTP 响应解析。明文 HTTP 收包循环与 `http_hdr`/`http_dechunk`
  行为不得改动（另一工作树正在改同一函数的流式版本，避免语义冲突）。
- **验收脚本**：参照 `tests/pkg_remote_v270_probe.py` 的启动 / 交互骨架，
  新脚本接受 ISO 路径参数，自起 `openssl s_server` 与 QEMU（监控端口 4501、
  TLS 端口 8443），guest 内 `netcfg` 后执行 `curl https://10.0.2.2:8443/`，
  用 HMP `screendump` 存 PPM 转 PNG 判定；清理只按 PID，不杀他人 qemu。

## 5. 证书校验策略（诚实声明）

- 本次实现**既没有做证书链校验，也没有做主机名校验**；当前规划里对服务端证书的
  用途仅是取出 RSA 公钥用于校验 ServerKeyExchange 签名的完整性。
- 因此即使后续打通，本客户端在未补链校验前仍属 **"仅加密、未验签"**：
  存在中间人（MITM）风险——攻击者可替换证书/公钥而客户端不告警，加密只能防被动
  窃听，不能防主动冒充。
- 信任锚最终形态待定（绑定自签证书 / 公钥 / 指纹之一），需在合并前明确写入代码
  与本报告；当前**不存在任何已固化的信任锚**。

## 6. 端到端验收（目标命令，本次未跑通）

环境：云主机 `vm`（linux），guest 经 slirp，`10.0.2.2` 映射主机 loopback。

1. 主机自签证书并起服务：

```
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/d2.key -out /tmp/d2.crt \
    -subj "/CN=10.0.2.2" -days 2
openssl s_server -accept 8443 -cert /tmp/d2.crt -key /tmp/d2.key \
    -tls1_2 -cipher ECDHE-RSA-AES128-SHA256 \
    -WWW -cert_chain /tmp/d2.crt   # 固定响应内容串：CATOS_TLS_OK_278
```

2. QEMU（自建命令行；`tests/qemu_driver.py` 的 `boot()` 硬编码 `-net none` 不能直接用）：

```
qemu-system-i386 -m 256 -no-reboot -display none \
  -monitor tcp:127.0.0.1:4501,server,nowait \
  -netdev user,id=n0 -device pcnet,netdev=n0 \
  -drive file=<disk>,format=raw,if=ide,index=0 \
  -drive file=<iso>,media=cdrom,if=ide,index=1 -boot order=d
```

3. guest 内：

```
netcfg 10.0.2.15 10.0.2.2 10.0.2.3     # 静态不通可退 netcfg dhcp（报告需写明实际方式）
curl https://10.0.2.2:8443/
```

4. 通过判据：屏幕打印固定串 `CATOS_TLS_OK_278`；HMP `screendump` 至少两屏
   （进 shell 后、curl 打出固定内容那屏），PPM 转 PNG 存本目录。

> 本次未产生上述截图；`artifacts/d2-tls/` 下暂无 PNG 证据。

## 7. 已知限制（目标态与现状）

- 现状：无可运行的 TLS 客户端；内核镜像未编入本次新文件。
- 目标态拟支持：TLS 1.2，套件 0xC027（ECDHE-RSA-AES128-CBC-SHA256）；
  曲线 secp256r1。暂不支持：TLS 1.3、前向保密以外的票证/会话恢复、ALPN、
  证书链与主机名校验、非 CBC 的 AEAD（GCM）套件。
- 内存约束：`-m 256`，freestanding 无 malloc；大缓冲须用静态数组。

## 8. 复现本次主机验证的临时命令（不落仓库）

```
# AES 加密 NIST
gcc -O0 -Ikernel_v25/include -Ikernel_v25/src /tmp/test_aes2.c \
    kernel_v25/src/tls_aes.c kernel_v25/src/crypto.c -o /tmp/test_aes2 && /tmp/test_aes2

# PRF
gcc -O0 -Ikernel_v25/include /tmp/test_prf.c \
    kernel_v25/src/tls_aes.c kernel_v25/src/crypto.c -o /tmp/test_prf && /tmp/test_prf

# P-256 小标量 / 字段层（测试程序 include p256.c 后调用内部函数）
gcc -O0 -Ikernel_v25/include -Ikernel_v25/src /tmp/p256_ecdh.c -o /tmp/p256_ecdh && /tmp/p256_ecdh
```
