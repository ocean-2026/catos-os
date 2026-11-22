# D10 大包流式下载 / 安装 验收报告

- 分支：`feat/d10-stream`（worktree `/home/user/work/catos-d10`）
- 基线：`73c750f`（CatOS v2.77）
- 验收时间：2026-11-22（Asia/Shanghai）
- 探针：`tests/d10_stream_probe.py`（e1000 + slirp，host 端 HTTP，QEMU screendump + 屏幕文字判定）

## 结论

**7/7 PASS。** 超过旧 16 KiB `PKG_BUF_MAX` 上限的签名大包，经 Content-Length 与
chunked 两条路径下载、验签、落盘后，均可在 guest 内安装并运行，打印验收标记后退出 0。

| # | 用例 | 结果 | 证据截图（绝对路径） |
|---|------|------|----------------------|
| 1 | netcfg dhcp 获取租约 | PASS | — |
| 2 | `pkg repo` 持久化基础 URL | PASS | — |
| 3 | `pkg update` 下载远程索引 | PASS | — |
| 4 | 大包下载（Content-Length，>16 KiB） | PASS，`got 46456 bytes` | `/home/user/work/catos-d10/artifacts/d10-stream/shot1_fetch_big.png` |
| 5 | 已安装大包运行 | PASS，`D10 BIGPKG OK` / `46 KiB payload resident` / `exit(0)` | `/home/user/work/catos-d10/artifacts/d10-stream/shot2_run_big.png` |
| 6 | 大包下载（Transfer-Encoding: chunked，>16 KiB） | PASS，`got 46456 bytes` | `/home/user/work/catos-d10/artifacts/d10-stream/shot3_fetch_chunk.png` |
| 7 | chunked 安装包运行 | PASS，`D10 BIGPKG OK` / `46 KiB payload resident` / `exit(0)` | `/home/user/work/catos-d10/artifacts/d10-stream/shot4_run_chunk.png` |

### 实测尺寸

- 验收程序 `d10big.ce`：46252 字节（CE01 头 `image_size=46232`，`api=0xb494`，bss=0）。
- 签名后 `.catpkg`：46456 字节（头 204 B + 载荷 46252 B）；两路下载实测 `got 46456 bytes`，均远大于旧 16 KiB（16384 B）上限。

## 复现步骤

```sh
cd /home/user/work/catos-d10
sh build.sh kernel
sh build.sh iso
# 探针在磁盘缺失时自动从 kernel_v25/catos_disk.img 种子化；guest 内自行 format
python3 tests/d10_stream_probe.py
# 可选端口覆盖（避开并行线占用）：D10_MONITOR=4511 D10_HTTP=8130
# 可选串口落盘：D10_SERIAL=/tmp/d10_serial.log
```

## 根因与修复（本轮）

### 现象

继承层下载 / 验签 / 落盘已打通，两条“运行新装程序”用例失败：任务以 `exit(0)`
结束但屏幕无任何输出；串口转储显示 `validate_uaddr()` 解引用 VMA 链表时，节点
内容被读成镜像 blob 的 ASCII 字节（`nstart=0x442d534f` … 对应 `"OS-D"` / `"10-p"` /
`"ad-0"`）。最小无 blob CE 可安装运行；24 KiB、46 KiB 内嵌 blob 种子均在运行时崩溃。

### 根因

帧分配器按物理地址 first-fit 出帧，内核把返回的物理帧地址直接当作恒等映射的
内核指针解引用（VMA 节点、页目录 / 页表、任务临时帧均如此）。任务 CR3 下，
虚拟地址区间 `0x2000000..0x2800000`（PDE 8/9）被替换为任务私有页表，**不再走
kdir 恒等映射**。

- 小镜像时，内核对象帧的物理地址落在 `0x2000000` 以下，恒等映射不受影响；
- 镜像变大后，首帧分配把 VMA 节点等内核对象帧推到 `0x2000000` 以上、与用户虚存区
  同一片物理地址区间。内核在 ring3 任务 CR3 下解引用该指针时，被任务私有页表别名
  到了任务镜像页，于是 VMA 节点被“读成”镜像字节，`validate_uaddr()` 遍历链表即崩溃。

帧分配追踪确认：目标帧只分配过一次，不存在同一物理帧二次分配；是别名翻译问题，
而非位图重复记账。

### 修复

`kernel_v25/src/paging.c::paging_init()` 在帧池初始化时，把与用户虚存区
`[USER_IMAGE_BASE, USER_EXT_END)` 对应的物理区间 `0x2000000..0x2800000`
标记为占用、不交给帧分配器。内核对象（VMA 节点、目录 / 页表等）因此只会落在该切片
之外；任务自身的镜像 / 栈 / 堆帧仍由帧池分配，通过页表项映射到用户虚地址，
不要求物理地址 == 虚拟地址。

修复后串口转储：VMA 节点帧位于 `0x2803000`（保留区之上），节点内容正确
（`start=0x2000000, end=0x2007000`）。

## 容量与协议定界

### 实际上限

- **加载器镜像上限**：`CE_MAX_IMAGE = 256 KiB`、`CE_MAX_BSS = 256 KiB`（`include/ce.h`），
  头字段越界即拒绝（`-5`）。本轮 46 KiB 镜像在此范围内。
- **磁盘**：安装落盘受 FAT 空闲空间限制（`/bin` 目录项与簇）。
- **内存**：任务镜像 / bss / 栈 / 堆帧来自统一帧池；帧池总量取决于 multiboot 上报的
  可用 RAM。本轮 guest `-m 256`，未触内存上限。
- **网络**：单连接在 slirp 下约 80 KiB 后可能停止喂包；链路会以 `-13`（needs resume）
  上报，当前调用方**不自动续传**。

### 已支持的 HTTP 定界方式

- `Content-Length`：按声明长度收取，收满即结束；
- `Transfer-Encoding: chunked`：增量 chunk 解码（chunk 长度行 + 数据 + 尾块）；
- 连接关闭定界：无长度声明时，FIN 排空即结束（`remain < 0`）；
- 3xx 跳转：最多跟随 4 跳（`Location`），HTTPS/TLS 仍不支持（`-7`）。

### 本轮未做 / 未解限制

- **断点续传**：协议层预留 `Range: bytes=<have>-` 入口与 `-13` 提示，但 shell / 下载
  调用方没有自动重连续传的状态机。
- **超时 / 失败自动重试**：仅有空闲轮询上限与 RST 上报，不做退避重试。
- **curl 到屏幕**：文本 `curl` 输出仍受控制台行 / 缓冲长度约束；本探针只验证包体
  下载与运行，未做大流量屏幕回显。
- 帧池仍是“内核虚拟 == 物理”的扁平假设；PCI 孔洞上方的第二段 RAM 未枚举（既有
  已知边界，见 `docs/ROADMAP`）。

## 回归核对

`tests/pkg_remote_v270_probe.py`（小包远程安装）：**6/7 PASS**，与基线一致。
唯一 FAIL 为“篡改包被校验器拒绝”用例：该包在 CE 镜像头门即被拒（`corrupt CE image
header -- refused`），属既有行为差异，与本次流式层 / 帧保留改动无关。

## 交付物清单

- 探针：`tests/d10_stream_probe.py`
- 验收程序源码：`kernel_v25/extapp/d10big.c`（46 KiB 内嵌只读 blob）
- 证据目录：`/home/user/work/catos-d10/artifacts/d10-stream/`
  - 四张运行 / 下载截图（见上表）
  - `repo/big.catpkg`、`repo/chunk.catpkg`（host 端现签样本，gitignored 产物）
- 本报告：`artifacts/d10-stream/REPORT.md`

## 提交列表（本地，未 push）

| Commit | 说明 |
|--------|------|
| `424abd1` | wip(d10): 流式 TCP/HTTP 下载 + 流式包安装层（继承层：流式 SHA-256、`net_http_download_stream`、shell 边下边落盘 / 验签 / 安装、探针骨架、46 KiB 夹具） |
| `cc42b03` | fix(d10): 帧分配器保留用户虚存区对应物理切片（本轮根因修复） |
| `4e152ad` | test(d10): 46 KiB 验收夹具按 sizeof 校验 + 探针可复现性（磁盘种子化、端口 / 串口 env 覆盖） |

基线：`73c750f`（v2.77）。未改动 `/home/user/work/catos-os`、`/home/user/work/catos-d2`；
未 push；未改版本号 / banner / 文档总表 / README。
