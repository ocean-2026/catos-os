# 内核静态缓冲归属表（Stage 3C-1，feat/hosted）

本轮把网络 / SSH 路径里函数级 `static` scratch 缓冲收进所属的连接 / 会话对象；
任务侧按 tid 索引的快照缓冲核对为已 per-task。**不新增 `.bss` 大数组**：缓冲只是
从函数级静态搬进所属结构体内，总量净减 64 字节（对齐重排），strict RC=0，
`_bss_end=0x02117a00`（3A 收口时为 `0x02117a40`）。

> 模型前提：当前内核单核、非抢占，网络栈同一时刻只有一个 TCP 客户端连接
> （`tcp`）、一个被接受的服务端连接（`tcps`）、一个 UDP 上下文、一个 SSH 会话。
> 因此"归属对象"是这些单实例结构；真正的多 socket / 多会话并发分配（kvheap
> 按连接发、关闭即释放）列入 3B，待并发连接模型落地。

## 1. netstack.c —— UDP 上下文 `udp`

原函数级 / 全局静态缓冲并入文件级 UDP 上下文结构 `udp`（单 UDP 客户端 / 解析器
实例，一次一个数据报复用）。

| 原静态缓冲 | 新归属对象 | 分配 / 释放点 | 大小 |
|---|---|---|---|
| `udp_rx_data[2048]` | `udp.rx_data` | 结构静态持有；`net_udp_recv` / DHCP / DNS 取走后置 `rx_len=0` | 2048 B |
| `udp_rx_len` | `udp.rx_len` | 同上 | 4 B |
| `udp_rx_sport` | `udp.rx_sport` | 同上 | 2 B |
| `udp_rx_dport` | `udp.rx_dport` | 同上 | 2 B |
| `udp_rx_src[4]` | `udp.rx_src` | 同上 | 4 B |
| `udp_local_port` | `udp.local_port` | 静态初值 `0xC000`，DNS 发送自增 | 2 B |
| `pseudo[12+8+2048]`（UDP 校验和伪头 + 数据报，原 `udp_handle` 内 static） | `udp.pseudo` | 上下文持有；每次校验和复用 | 2068 B |

### 保持协议全局（非连接归属，本轮不动）

| 缓冲 / 状态 | 归属 | 理由 |
|---|---|---|
| `arp_cache_mac[6] / arp_cache_ip[4] / arp_cache_valid / arp_cache_tick` | 协议全局 ARP 缓存 | L2 地址解析，不属于任一 socket |
| `dns_cache_name[64] / dns_cache_ip[4] / dns_cache_valid` | 协议全局解析缓存 | 跨连接复用的名称→地址缓存 |
| `ping_reply_data[8] / ping_reply_ok / ping_want_id / ping_want_seq` | 协议全局 ICMP 回显 | 单回显请求的一次性状态 |
| DHCP `dhcp_tried / dhcp_ok / dhcp_user_cfg` | 协议全局租约状态 | 本机配置，非连接 |
| `tcp`（客户端连接，含 `rcv[4096]`、`rcv_buf[65536]`） | 已在连接结构体内 | 既有 per-connection 对象，本轮未改 |
| `tcps`（服务端连接，含 `rcv_buf[16384]`） | 已在连接结构体内 | 既有 per-connection 对象，本轮未改 |

## 2. ssh.c —— 会话对象 `S`

原散落在各函数内的 static 收发 / KEX / 执行 / shell scratch 全部并入会话结构
`S` 的字段；函数内以指针别名引用（`uint8_t *out = S.s_out;` 等），协议与密码学
逻辑未改。`S` 是单会话槽，一次一条连接复用；并发会话是 3B 项。

| 原函数级静态缓冲 | `S` 字段 | 阶段 | 大小 |
|---|---|---|---|
| `ssh_send_packet` `out[SSH_OUT_MAX+64]` | `S.s_out` | 出站报文打包 / 加密 | 16448 B |
| `ssh_recv_packet` `body[SSH_IN_MAX]` | `S.s_body` | 入站报文解密 | 16384 B |
| `ssh_recv_packet` `macmsg[4+SSH_IN_MAX]` | `S.s_macmsg` | 入站 MAC 校验串 | 16388 B |
| `ssh_derive` `buf[64+40+1+32]` | `S.s_kdf` | KDF 哈希输入 | 137 B |
| `ssh_chan_data` `msg[8200]` | `S.s_msg` | channel-data 打包 | 8200 B |
| `ssh_do_exec` `line[512]` | `S.s_exec_line` | 命令行拼装 | 512 B |
| `ssh_do_exec` `out[8192]` | `S.s_exec_out` | 命令输出捕获 | 8192 B |
| `ssh_do_shell` `payload[SSH_IN_MAX]` | `S.s_shell_payload` | shell 入站报文 | 16384 B |
| `ssh_do_shell` `out[8192]` | `S.s_shell_out` | shell 命令输出 | 8192 B |
| `ssh_serve_session` `payload[SSH_IN_MAX]` | `S.s_loop_payload` | KEX 主循环入站报文 | 16384 B |
| `ssh_serve_session` `hb[8192]` | `S.s_hb` | 交换哈希 H 拼装 | 8192 B |
| `ssh_serve_session` `reply[512]` | `S.s_reply` | KEX_ECDH_REPLY 打包 | 512 B |

分配 / 释放点：`S` 为单会话静态槽；`ssh_serve_session()` 入口复位 seq / 长度字段，
各阶段覆写对应 scratch；会话结束随槽位复用。**deferred（3B）**：并发会话落地时，
整块 scratch 改由 kvheap 在会话建立时分配、关闭时释放，`S` 持指针而非内嵌数组。

## 3. task.c —— 按 tid 索引的任务快照缓冲（已 per-task，本轮核对未迁）

下列数组按 `tid` 分槽，阻塞型 syscall 切换到别的任务时不会串掉快照；归属已是
per-task 槽位，**不是共享单缓冲**。本轮未把它们嵌进 `task_t`（搬迁会抬高每任务
结构体与 .bss），列为 3B 清理项。

| 缓冲 | 索引 | 单槽大小 | 用途 |
|---|---|---|---|
| `ls_buf[MAX_TASKS][LS_MAX]` | tid | `ls_ent_t` × 64 | 目录列表快照 |
| `e3_wr_buf` | tid | 257 B | `SYS_WRITE` 快照 |
| `e3_rl_buf` | tid | 256 B | `SYS_READLINE` 快照 |
| `e3_fr_buf` | tid | 16384 B | `SYS_FILE_READ` 快照 |
| `e3_fw_buf` | tid | 4096 B | `SYS_FILE_WRITE` 快照 |
| `e3_pw_buf` | tid | 4096 B | `SYS_PIPE_WRITE` 快照 |
| `e3_pr_buf` | tid | 4096 B | `SYS_PIPE_READ` 快照 |
| `e3_ur_buf` | tid | 2048 B | `SYS_NET_UDP_RECV` 载荷 |
| `e3_ur_ip` | tid | 24 B | `SYS_NET_UDP_RECV` 地址串 |

> 注意：线程共享进程但持独立 tid，因此工作线程在这套按 tid 索引的缓冲上同样拿到
> 自己的槽；真正要迁进 `task_t` 结构体、按进程 / 线程对象归属，随 3B 堆 per-process
> 一起做。

## 4. 冒烟与回归（3C-1 构建）

- `threads.ce` 多线程：`THREAD PASS`，`sum=10`、fault join ret=1，无线程间串扰。
- 三门禁：vprobe 10/10、E3 2/2、F11 9/9，均 RC=0。
- **卡点（未在本机实测）**：本机 QEMU 无 PCnet/e1000（串口 `no PCnet`），网络
  联机路径（`curl https://example.com`）与 SSH 会话握手无法在此环境拉起；本轮
  netstack/ssh 改动为归属搬迁（指针别名 / 结构体归类），协议与密码学路径逻辑未
  改，联机冒烟待有 slirp 网卡的环境补跑。

## 5. deferred 清单（3B / 3C）

- 多 socket / 多会话并发模型：UDP / SSH scratch 改 kvheap 按实例分配、关闭释放；
  `tcp` / `tcps` 连接对象扩容为连接表。
- `ls_buf` / `e3_*_buf` 从静态 per-tid 数组迁入 `task_t` 结构体。
- 共享堆分配器 per-process；`dl_mods` 句柄表归属与加锁。
- 内核态可抢占（3C）：抢占点 + 关抢占区计数，解除 netstack/ssh/dl 的非抢占假设。
