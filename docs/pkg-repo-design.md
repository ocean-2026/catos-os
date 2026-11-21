# CatOS 包管理器与仓库链路设计（P4）

## 1. 现状（P4 已实现的本地包路线）

CatOS 客户机在 QEMU 下以 `-net none` 启动，没有可用的网络栈链路，因此 P4
落地的是**本地包仓库**：包容器随内核镜像嵌入，开机播种到 FAT32 卷的
`/pkg/` 目录，`pkg` 命令在本地完成安装/卸载/查询。

```
内核镜像 (catos.iso)
  └─ gen_builtin.py 嵌入
       ├─ /bin/<builtin>.ce        内置应用（apps.list）
       └─ /pkg/<pkg>.catpkg        打包应用（pkg.list，P4 新增）
            └─ PKGS.TXT 索引       name|version|desc|file
```

### 1.1 `.catpkg` 容器格式（宿主侧 `tools/mkpkg.py` 生成）

> v2.68 更新：格式从 `CPKG01` 升级为带 RSA-512 签名的 `CPKG02`。
> 下面以当前代码为准（`tools/mkpkg.py` + `shell.c:pkg_verify`）。

定长 204 字节头 + 原始 CE01 镜像负载，小端序 ASCII 字段，签名 64 字节大端：

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0    | 8    | magic | `CPKG02\0\0` |
| 8    | 16   | name  | 包名（索引键） |
| 24   | 16   | version | 版本字符串 |
| 40   | 16   | cmd   | 安装后的命令名 → `/bin/<cmd>.ce` |
| 56   | 80   | desc  | 一行描述 |
| 136  | 4    | payload_len | 负载字节数（uint32 LE） |
| 140  | 64   | signature | RSA-512 签名（大端）：`pow(0x01<<56 \| FNV1a64(header\|payload), d, n)` |
| 204  | …    | payload | 原始 `.ce` 镜像（含 CE01 头） |

私钥 `tools/pkg_devkey.txt` 只在宿主侧，内核仅嵌公钥模数 `PKG_PUB_N`；
`pkg install` 用 `pow(sig, 65537, n)` 验签，失败即拒绝安装。旧 `CPKG01`
无签名容器不再被接受。容器格式与 CE01 解耦：未来更换负载格式（ELF/压缩）
只需扩展头部，安装器按 magic 分派。

### 1.2 客户机命令

| 命令 | 行为 |
|------|------|
| `pkg list` | 列出索引中全部包及状态（installed / available） |
| `pkg info <name>` | 显示版本、描述、负载文件、安装状态 |
| `pkg install <name>` | 校验容器 → 解包到 `/bin/<cmd>.ce`，注册为可执行命令 |
| `pkg remove <name>` | 删除 `/bin/<cmd>.ce`（保留 `/pkg/` 仓库副本） |

索引文件 `/pkg/PKGS.TXT` 为简单行式：`name|version|desc|file`。

## 2. 未来：从 Linux 发行版仓库交叉构建 CE 包

> 本节为目标链路设计，P4 不要求全部实现。当前宿主侧只有 `mkpkg.py`
> 打包器与本地索引；联网拉包依赖内核已在位的 e1000 驱动与
> `net_send`/`net_recv` 接口（见 `docs/TODO-2026-11-12-VMware反馈轮.md`
> §四），作为后续扩展点保留。

### 2.1 仓库布局（构建宿主侧）

```
catos-repo/
  dists/
    <catos-release>/
      binary-i386/
        Packages.tar.zst        # 元数据索引（名称/版本/依赖/描述/校验和）
        *.catpkg                # 包容器（格式见 §1.1）
      Release                   # 仓库级签名（时间戳、Packages 校验和）
  pool/
    <upstream-src>.tar.xz
```

- `binary-i386/Packages`：每行一个包，字段对齐 `pkg_meta_t`：
  `name|version|description|cmd|dependencies|sha256`。
- `Release`：仓库清单的 detached 签名（GPG/Ed25519），含 Packages
  文件的 SHA-256 与时间戳，防篡改与回滚。

### 2.2 交叉工具链

| 环节 | 选型 | 说明 |
|------|------|------|
| 编译器 | 现有 freestanding i386 工具链（`gcc -m32 -ffreestanding`） | 与 `extapp/build_ce.sh` 一致；产出 PE flat 镜像 |
| 链接器 | `ld -m i386pe` + `ce.ld` | 入口 `ce_main` 固定在镜像偏移 0 |
| 打包 | `tools/mkpkg.py` | 包元数据写入定长头，负载为 CE01 镜像 |
| 校验 | SHA-256（P4 占位）→ 签名（后续） | 安装前校验负载完整性 |

上游源码（C 库、工具、游戏）在交叉 sysroot 中编译，剥离调试符号后
封装为 `.catpkg`。CE ABI 是追加式的（`ce_abi.h` 槽位只增不删），旧
二进制在新内核上保持兼容。

### 2.3 包构建流水线（目标态）

1. **取源码**：从上游 tarball / git 检出到 `pool/<src>/`。
2. **交叉配置**：`./configure --host=i386-catos --prefix=/usr`（或等价
   裸 Makefile），链接 CatOS freestanding 运行时桩。
3. **编译**：复用 `extapp/build_ce.sh` 的参数族（`-m32 -ffreestanding
   -fno-pic -fno-stack-protector …`），产出对象文件。
4. **打包**：`mkpkg.py` 写入元数据头 + CE01 负载；生成
   `Packages` 索引条目。
5. **入库**：`.catpkg` 放入 `pool/`，元数据追加到 `Packages`；
   `Release` 对 `Packages` 做签名。
6. **客户机安装**：`pkg install <name>` 拉取 → 校验签名/校验和 →
   解包到 `/bin/`。

### 2.4 联网拉包的扩展点（内核已在位）

- 网卡驱动：e1000（`src/e1000.c`），`net_send`/`net_recv` 接口已在
  CE ABI 表中。
- 传输协议：当前无 HTTP 栈；后续可先实现极简 HTTP/1.0 客户端，
  从仓库镜像拉取 `.catpkg` 与 `Packages` 索引。
- 安全：P4 容器头无签名；目标态在 `Release` 中记录 `Packages` 的
  SHA-256，安装器校验负载哈希；签名验证作为后续增强。

### 2.5 依赖与升级（后续）

- 依赖解析：`Packages` 条目的 `dependencies` 字段；安装时拓扑排序，
  先装依赖再装包。
- 升级：`pkg upgrade <name>` 对比仓库版本与本地版本，备份旧包到
  `/var/backup/`，失败可回滚。
- 回滚：保留上一版本 `.catpkg`，`pkg rollback <name>` 恢复。

## 3. P4 落地范围对照

| 能力 | P4 状态 |
|------|---------|
| 本地 `/pkg/` 仓库 + `PKGS.TXT` 索引 | 已实现 |
| `.catpkg` 容器格式 + `mkpkg.py` | 已实现 |
| `pkg list/info/install/remove` | 已实现 |
| 示例包（bc / stats / rps） | 已实现并实机验证 |
| 签名校验（CPKG02 / RSA-512） | v2.68 已实现（`pkg_verify`），验签不符拒装 |
| **可拆卸桌面扩展（desktop.catpkg）** | **v2.69 P1 已实现，10/10 实机验证**（见下） |
| **HTTP 远程仓库拉包（`pkg repo/update/fetch`）** | **v2.70 已实现，7/7 实机验证**（见下） |
| 交叉编译上游源码入仓 | 设计预留，未实现 |

## 4. 范例：desktop.catpkg —— 图形桌面作为可装卸扩展（v2.69）

这是包机制最有力的证明：`desktop` 是**整个图形桌面环境**，走普通
`.catpkg` 通道装卸，内核代码零改动：

```
/pkg/desktop.catpkg --pkg install--> 验签(RSA-512) --> /bin/desktop.ce
   --裸名 desktop--> 运行纯用户态桌面(窗口/焦点/任务栏全在 CE 侧)
   --pkg remove desktop--> 删除 /bin/desktop.ce --> 回到纯 shell
```

- `desktop.ce` 只用普通 CE ABI：新增的 mouse_x/y/buttons（槽 66..68）
  + 已有 gotoxy/write/pollkey；窗口语义不在内核。
- 安装前后内核二进制字节一致，证明桌面语义确实不在内核里。
- 生命周期实机验证：`tests/desktop_pkg_v269_probe.py` 10/10 PASS。
- 设计细节见 `docs/CDE-模块化设计.md` §8。

## 5. 远程仓库（v2.70）：pkg repo / update / fetch

本地仓库之外，`pkg` 现在可以从 HTTP 远程仓库拉包，全程复用同一套 RSA-512
验签——远程来源的包和本地包走完全相同的 `pkg_verify()`，签名不符一律拒装。

```
pkg repo http://10.0.2.2:8125/repo   # 设置/查看远程仓库 base URL（存 hive）
pkg update                           # GET <repo>/PKGS.TXT -> /pkg/PKGS.TXT
pkg fetch NAME                       # 按索引文件名拉 <repo>/<file>，验签后装到 /bin
pkg fetch http://IP[:port]/file.catpkg  # 显式 URL 直拉
```

- **二进制安全传输**：现有 `net_http_get()` 是文本 API（`char*`+NUL 结尾、
  rcv_buf 4096、按 Content-Length 截断），不能承载含 0 字节的 `.catpkg`。
  v2.70 抽公共核 `http_fetch()`，新增 `net_http_download(url, uint8_t*, max)`
  返回精确字节数、不做 NUL 结尾；`tcp.rcv_buf` 扩到 20480 容纳整包。
- **验签零分叉**：本地 install 与远程 fetch 共用抽出的 `pkg_install_payload()`，
  包含 magic/length 校验、`pkg_verify()`、写盘 + ACL。篡改 payload 一字节即拒装。
- **安全边界**：仓库 URL 仅接受 `http://`；私钥不进内核，内核只嵌公钥 N。
  传输本身不加密（公开软件包无需机密性，apt/npm 同理），完整性与来源由签名保证。
- 实机验证：`tests/pkg_remote_v270_probe.py` 7/7 PASS——含宿主 HTTP 服务器可达性
  marker、`pkg update` 拉索引、`pkg fetch` 装远程 desktop 并能运行、篡改包被拒。
- 宿主协议原型：`deliverables/pkg-remote-prototype/pkg_remote_proto.py`（真实包字节级验签）。
