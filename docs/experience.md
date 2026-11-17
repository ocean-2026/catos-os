# experience.md - 开发经验记录

格式：[所属项目][经验]内容

- [ot][经验] 黑盒探测优先于猜测：execve 实现前用 argv_probe.c 实测内核 CE 调用约定（argv@0x0147c500/字符串@0x0147c540/esp/ce_main@0x02000000/api 表@0x0101c280），所有常量来自实测而非文档推导
- [ot][经验] 裸函数（naked）内无操作数 asm 用**单 %** 寄存器，扩展 asm（有操作数）用 **%%**；混用报 "bad register name"（exec_tramp 踩过）
- [ot][经验] QEMU sendkey 数字参数（0xNN）语义在 QKeyCode/PS/2 扫描码间不可靠（实测 0x2d 时 x 时 v），**键名模式（sendkey c）更稳定**；长 hold 参数（sendkey o 1000）不改善 CatOS 接收率
- [ot][经验] 跳板/桩代码用 `__attribute__((naked))` + 纯汇编 + `exec_tramp_end` 取长度差拷贝，避免内联函数地址问题；用 objdump 反汇编核对每条指令机器码再进 VM
- [ot][经验] exec 场景（覆写自身镜像）的调试节奏：先在宿主侧完成全部静态验证（CE01 头解析、跳板寄存器契约、反汇编），VM 只做最终确认，减少对脆弱键盘通道的依赖
- [ot][经验] 内核栈 8KiB 限制下，跳板/argv 区选在探测到的固定安全区（0x0147c500-0x0147c800），避开镜像（0x02000000）与内核栈（esp≈0x0147e658），先算布局再写代码
- [ot][经验] 镜像预算管理三原则：① bss 不占镜像（static 大缓冲随意用，如 4KB 分配器池/2.5KB 快照缓冲）；② text/rodata 才占镜像，入镜像前先算余量（65536 减当前体积）；③ 余量不足时优先外置 /BIN/*.csc 脚本（自动注册机制已有，calc/wc/time 已验证），或写设计文档延后（L3/⑩ 即此策略）
- [ot][经验] 键盘失效（#34）时的验证分层：① 静态（构建+bss/镜像体积核验）② 符号（objdump -t 核验新函数入镜像）③ 反汇编行为（objdump -d 核验关键逻辑如递归下降求值器）④ VM 交互（键盘好时才做）；③④ 都可覆盖时如实汇报验证方式，不长时间卡键盘
- [ot][经验] 复用小机制胜过新机制：swap 直接复用 csc_save_shards（512B 分片写）+ csc_snapb（分片读回重组），仅 +56B 就打破 save 512B/cat 8KB 双限制；改造现有原语而非另写文件 IO
- [ot][经验] 大 rodata 资产外置省镜像：像素画 banner（~460B）移到 /BIN/BANNER.TXT 由 cmd_banner 读文件渲染（复用 csc_index_lines/csc_fetch_row 逐行输出），缺文件时降级纯文本标题——净省 ~288B 镜像，且用户可编辑 banner；外置边界：启动路径依赖磁盘（缺文件有降级即可）
- [ot][经验] swap 旧分片残留绕行：CE 无 delete API 无法清旧 .A0x 文件，改为 swapin 按 RAM 页表 g_swap_n 只读主文件+前 N-1 片（起始片 .A00 对应 page1），陈旧分片被边界自然忽略——用状态计数代替文件清理
- [ot][经验] L3 子程序帧栈放 bss 不占镜像：8 帧 × (10×40B argv + 4B argc + 4B ret) ≈ 3.2KB 全在 bss（静态），实现 func/call/ret 代码仅 ~700B 入镜像；$ret 返回值直接复用变量表（csc_set("ret")）零新增展开逻辑
- [ot内核v2.5][经验] 大文件读写绕行 FS_DATA_MAX：新增独立 syscall（SYS_FILE_READ/WRITE/DELETE）走 FAT 链簇定位，不经 512B save 上限；offset 读 = offset/簇大小 跳链 + 簇内偏移，写 = mode0 截断/mode1 从链尾续接（fat_append_file）
- [ot内核v2.5][经验] CE 镜像上限提升套路：CE_MAX_IMAGE/BSS 常量提升 + 移除内核临时大缓冲（ce_bounce）+ spawn 改为"读首簇解析 header + 沿 FAT 链直写已建立页的映射"（避免整镜像进内核缓冲），256KB 镜像成本=页表页数
- [ot内核v2.5][经验] 管道 EOF 语义：read 读到空但 writer 仍开 → 阻塞；读到数据读空即返回（不因 got<max 死等）；writer 关闭（open_w=0）→ read 空时返 EOF（got=0）——"有数据即返回 + 无数据等" 两态正确实现避免死锁
- [ot内核v2.5][经验] 内核调试三件套：objdump -t 找崩溃 eip 符号 → objdump -D -b binary 看指令（pop gs=0f a9）→ 比对 cpu.s 源码理解帧布局；QEMU monitor sendkey + screendump + tesseract OCR 全自动回归
- [node解释器][经验] 微型解释器调试三层递进：token 流 → AST dump → 求值追踪；AST dump 与运行时错误点打印**节点地址+op**，地址对比可直接区分"解析建错节点"vs"运行期节点被污染/链表越界"（本轮 callee-op 2→3 之谜最终是 tok_kw 前缀误判，n_new 垃圾节点是另一族）
- [node解释器][经验] 解释器 bss 预算：大头 = token 数组(2048×16B) + arena + srcbuf；递归 fib(10) 每调用分配 scope ~144B，两遍调用 ~50KB → arena 按最坏脚本路径留量（96→160KB）；TOK_MAX 4096→2048 省 32KB；CE bss 控制在 256KB 内（当前 0x32180=205KB）
- [ot内核][经验] FAT 共享介质一致性：#48 根因=整扇区重写抹掉宿主注入表项；修法=脏表项 journal + 脏扇区 RMW（先读盘再补丁）+ 分配前核对磁盘真 0；教训：FAT 表是"多写者共享"资源（宿主工具+内核），任何整块覆写都必须基于磁盘真值而非内存缓存
- [node解释器][经验] 错误恢复健壮性：递归下降解析器每个循环（p_stmt_list/实参/声明符）都要有 token 前进不变量，否则任何 perr 路径都可能变成无限分配；加 ti0 对比 + 强制推进是通用保险
- [ot内核v2.5/工具链][经验] 单一真值源治常量漂移：构建脚本不要复制内核常量，运行时解析 `include/ce.h` 的 `#define CE_MAX_IMAGE/CE_MAX_BSS`（找不到头文件时回退内置值），再用回归测试断言「脚本读到的值 == 头文件值」+「两份副本字节相同」；以后提升上限只改一处
- [ot内核v2.5/工具链][经验] 把加载器校验前移到构建期：内核 `task_spawn_ce` 的三条早退（image / bss / api_offset）在 mkce.py 同步实现，报错直接给出违反的上限和来源头文件；QEMU 不可用的环境下，构建期能拦住的错误不要留到实机再排查
- [ot内核v2.5/工具链][经验] 重复脚本必然腐烂：同一构建链出现 `extapp/build_ce.sh` 与 `tests/build_ce.sh` 两份副本后只有前者被持续维护，后者路径全失效；修法是让后者成为薄包装（解析源文件绝对路径 + exec 转发），并在注释里写清产物落点
- [ot内核v2.5/工具链][经验] 边界值要用真实产物校准：新增 api_offset 校验时先跑一遍真实 `spawn_fail.ce`，发现 `api+4 == image_size` 恰好相等，因此判定必须用 `>` 而非 `>=`；先测真实产物再定边界，比凭代码生成逻辑推断可靠
- [ot内核v2.5/工具链][经验] 构建工具的校验允许「比内核更严」但不允许更松：内核 `task_spawn_ce` 用 `uint32_t` 算 `api+4`，`api=0xFFFFFFFC` 回绕成 0 会绕过校验并在映射区外写 API 表指针；mkce.py 用无限精度整数并显式拒绝 `api > UINT32_MAX-4`。发现内核侧缺陷时先记入问题清单、明确交给主工作树合流处理，不在隔离分支擅改他人活跃文件
- [ot内核v2.5/工具链][经验] 复核要专门验证「测试能不能失败」：新回归套件写完先做独立只读复核，重点问「如果被测机制完全失效，这条断言还会不会绿」；本次正是靠这个提问抓出回退值假阳性，比再加十条正常路径用例更有价值
- [CatOS/#90][经验] **逐字节同构复刻内核 mkfs 到宿主工具**是免改内核的应用解耦路径：`mkdisk.py` 复刻 `fat_mkfs()` 布局 + import `gen_builtin.py` 的文件清单保单一来源，内核 `fat_probe()` 只认 DBR 字段所以零改动即挂载；比 GRUB module + multiboot info 解析简单一个数量级，且绕开了"本机无 GRUB 工具链"的硬约束
- [CatOS/#91][经验] **能力检查放内核 syscall 分发处，不放 shell 层**：shell 可以被绕过（直接写 CE 程序调 syscall），只有分发处强制才构成安全边界；代价是零——现成 prologue 加一行判断。配套需要探针区分「-1 硬件缺失」vs「-13 能力拒绝」（captest.ce 打印原始返回码），否则拒绝会被误诊为没硬件
- [CatOS/#86][经验] **用户指针的统一心法是"快照进内核"而不是"校验后裸写"**：逐边界补页校验（validate 1 字节）靠人手维护、漏一处就是停机级故障；`copy_from_user()` 页表拷贝天然对跨页/未映射页健壮。字符串这种未知长度场景用 `u2k_str()` 内核快照，按真实上界校验而不是按 1 字节
- [CatOS/#91][经验] 权限语义重构时保留旧默认行为作底：新 Unix 三段位 ACL 查不到表项时回退到旧白名单（/home、/tmp 可写），老应用（perms.ce）合同不变，回归不用重写——重构兼容层的验证成本由此减半
- [CatOS/流程][经验] 每轮交付后**同步问题清单状态列**是硬约定：清单是后来者的唯一事实源，代码改了状态不改，「待修复」会误导下一轮重复排查（#75 的教训）；API.txt 与 apps.list 同理，改 ABI/应用集必须同轮同步文档
- [CatOS/v2.14][经验] **收编老项目的原则是"内核已有的从 ot 去掉，没有但有价值的内核态实现"**：ot 的 30 个与 shell 重复命令直接从命令表删除（而不是留着兼容），独有价值（脚本引擎/文件管理器/十六进制编辑/搜索/版本控制）保留；当年在用户态做 workaround 的根因（无非阻塞按键、无文件四件套）在内核态补齐后，应用侧 hack 一行不留
- [CatOS/v2.14][经验] **修用户态 hack 要修到 API 层而不是给 hack 发护照**：adofai 屏蔽 IRQ1 + 直读 0x64/0x60 的"解法"本质是 ring3 篡改中断控制器；正确路径是内核补 slot 64 `pollkey()`（`kb_getkey_timed(0)`，nargs=0 沿用单参 thunk），游戏删光 IMR/端口代码——能力进内核，应用只管调用
- [CatOS/v2.14][经验] **测试 flaky 先怀疑测试节奏再怀疑内核**：ot_regress 三处 FAIL 手动复现全部成功，根因全是测试侧（y 没跟 Enter、format 后等 2.5s 不够、读屏竞态）；诊断方法是手动脚本逐步二分"哪一步之后 faultinfo 有记录"，定位后修测试而非内核
CatOS[经验] UHCI 轮询式 HID 键盘驱动最小实现：PIIX3 UHCI（BAR4 I/O）+ 4KB 帧表全指向单 QH + 单中断 IN TD，10ms 定时器轮询 TD status，免 EHCI/OHCI 复杂度即得 USB 键盘；控制传输（SETUP/IN/OUT 深度链）仅用于枚举三步，之后设备地址固定，无需 SET_IDLE/SET_PROTOCOL。
CatOS[经验] 判定"内核某路径是否真的执行"：不要相信源码里有调用就等于运行——先核实调用点是否处于可达路径（CatOS case 0 教训），再用一次性串口 breadcrumb（static flag 打一次日志）做存在性验证，成本远低于反复推断。
CatOS[经验] 审查 ring0 shell 权限面的高效手法：按 fs_touch/fs_mkdir/fs_write_raw_mode/fs_copy_file/fs_delete/fs_rmdir 全量 grep shell.c，逐个确认该命令在写盘前是否经过 perm_write_on_parent 或 fs_perm_check；未经过的就是 syscall 层守卫罩不住的缺口（本轮由此定位 touch 漏网）。
CatOS[经验] QEMU UHCI 热插拔时序：monitor device_add usb-kbd 后，内核靠 100 tick 轮询挂起 pending_attach、由键盘等待循环里的 usb_service() 触发扫描+端口复位；枚举/复位需要 1~2s 窗口，探针延迟太短会读到 "no device found" 误判为驱动缺陷。端口状态 bit7(0x0080) 在 QEMU 实连时置位，可作端口有效位判据。
CatOS[经验] 终端现代主题整改的最低风险路径：以 16 色 CLUT 为唯一语义映射层——bg=#0C0C0C、正文=#DCDCDC、次要=#8B949E、强调绿=#7EE787、路径蓝=#60A5FA、语义红=#F85149/黄=#E3B341/青=#39C5CF/成功绿=#56D364，分别映射到 CON_BLACK/LIGHT_GREY/DARK_GREY/LIGHT_GREEN/LIGHT_BLUE/LIGHT_RED/YELLOW/LIGHT_CYAN/GREEN；shell 侧提示符/错误/成功只引用索引，不改硬编码 RGB，换肤只需改一张表。
CatOS[经验] 帮助/清单两列对齐：写一个 help_row(cmd,desc) 小助手，先把命令左对齐补齐到固定宽度（24）再写分隔与描述列（第 26 列起），比在超长静态字符串里手工数空格稳健；命令超宽时补齐循环自然跳过、描述退一格不崩溃。
CatOS[经验] 扩展恒等映射覆盖范围时，把“映射上界”和“分配器上界”分开维护：PSE 尾块可整块映射（沿用旧 16-PSE 行为），但分配器只发 < mem_top 的帧，尾块内不存在的 RAM/hole 永远不会被触达；任务目录镜像要复制全部 1024 个内核 PDE（不能只拷前 16 个），高 LFB PT PDE 随 kdir 一起镜像且保持 supervisor-only（无 US 位），ring3 仍不可触达。
CatOS[经验] 排查内存类回归先画探测链路再动手：GRUB mbi.mem_lower/mem_upper → kernel.c 透传 → hw_mem_boot/hw_num 显示 → paging_init 的 PMM 上界；逐段确认有无二次截断（本轮截断点在 hw.c 总量钳到 63MiB、paging.c top 钳到 64MiB 两处），其余消费方（shell mem 命令直读 mbi、hwinfo/top 走 hw_num）无需改。
CatOS[经验] 区分“既有环境问题”与“新引入回归”的最快办法是 A/B：git stash 源码改动 → 重建 iso → 同一条回归脚本跑基线 → 比对失败集合；本轮 size_regress 三档 band 在基线同样 FAIL，根因是检测色值常量滞后于现代主题配色，与内存改动无关。
CatOS[经验] 为新二进制格式写内核加载器时，先在宿主侧用 Python 按磁盘偏移逐字段解包并跑一遍全部边界校验，再对照内核 C 结构体；字段错位这类问题在宿主模拟器里能立刻暴露，比反复实机重编译截图定位快一个数量级。
CatOS[经验] 跨 guest 会话持久化 FAT 卷的稳妥模式：fresh 磁盘 → 拷到本地 /tmp 暂存 → guest 格式化 → 退出前把暂存盘拷回原始磁盘 → 宿主注入工具写原始磁盘 → 再拷回暂存盘起第二遍；既绕开工作区 fs 的 flock 限制，又保证两遍之间卷状态连续。
CatOS[经验] 给内核加第三条二进制轨道（CE01/ELF32 之外的 PE32）时，分派点先按魔数三分支，再把"镜像/bss/api 槽位注入"这类旧轨道专属条件用 !is_elf && !is_pe 排除；新轨道的段映射/VMA 标记在各自 loader 内部完成，避免污染既有 CE/ELF 路径的页表与 API 槽写入。
CatOS[经验] 多卷 FAT 内核的最小改造路径：几何参数按卷存上下文结构体，FAT 缓存/写日志作为活跃卷共享（避免每卷再占一份 4 MiB 静态缓存撑大 BSS），切卷时从磁盘重载该卷 FAT #1；接口上只加 activate/mount/release 三个动作，旧的 fat_* 操作签名不动，上层 fs 调用方几乎零改。
CatOS[经验] Linux 挂载语义在 VFS 层的落点：路径解析做最长前缀匹配，命中挂载点即切到对应卷再解析剩余相对路径；盘符后缀只在列目录展示时按“父路径/名字”反查挂载表追加，目录本身不改名，umount 后原名自然恢复，无需额外的改名/还原逻辑。
CatOS[经验] 无 sfdisk/mtools 时，宿主 Python 可直接构造双分区 FAT32 测试盘：镜像内核 mkdisk 的 DBR/双 FAT/根目录写入器，按分区 LBA 落盘，MBR 填两个 0x0C 表项（起始 LBA + 扇区数）；分区内预置一个标记文件即可在 guest 里验证跨分区读，cp 一个已有文件进去即可验证跨分区写。
CatOS[经验] ring0 shell 内建是彩蛋类功能的最低成本路径：无需走 CE 编译链（extapp/build_ce.sh + builtin_apps.inc 嵌入），在 shell.c 加分发分支即可；逐帧动画用 task_sleep_ticks(6)（tick=10ms，约 60ms/帧），可中断轮询用 kb_getkey_timed(1)，收到 0x03（Ctrl+C）即退出。
CatOS[经验] 日历实现：星期几用 Sakamoto 公式（t[12]={0,3,2,5,0,3,5,1,4,6,2,4}，3 月前年份减一），闰年判定 (y%4==0 && y%100!=0) || y%400==0；全年 3 列布局先把每月填进 rows[7][22] 缓冲，再按行拼接三块并补齐到固定宽度，避免边打印边对齐错位。
CatOS[经验] 命令纠错建议：微型 Levenshtein（命令词 ≤15 字符，16×16 栈表足够）+ 前缀命中降权（输入是命令前缀时距离记 1），阈值距离 ≤2 才给 "did you mean"，避免对无关命令乱提示；内置命令名单独维护一张表，新增命令同步登记。
CatOS[经验] echo 加开关参数（-n 无换行 / -c 颜色）时，先循环剥离开关再打印；颜色用 con_get_fg() 存旧前景、打印后 vga_set_fg(old) 还原，不要写死恢复色，否则会覆盖用户当前主题色。
CatOS[经验] 包管理器框架复用既有嵌入机制：gen_builtin.py 除 builtin_files[] 外再发一张 pkg_meta[]/pkg_files[] 表，开机播种 /pkg/ 与 PKGS.TXT；pkg install 解包到 /bin，pkg remove 只删 /bin 副本、保留仓库副本，安装状态由 /bin/<name>.ce 是否存在推导，无需额外状态文件。
CatOS[经验] 容器包格式定长头 + 原始负载：头字段 NUL 填充、小端长度；安装器先校验 magic 与 头长+负载长==文件长，再按头内 cmd 名落盘 /bin/<cmd>.ce；未来更换负载格式（ELF/压缩）只需扩展头部并在安装器按 magic 分派。
CatOS[经验] 并行分片 QEMU 端口隔离：本分片独占 4740-4749；回归脚本默认 4444，并发跑多个分片时必须显式 --port，避免撞 monitor socket。
CatOS[经验] 实机验证前先做镜像内容自检：strings kernel.bin | grep 新命令串，确认代码真的链接进内核；再核对 catos.iso 内嵌副本与 kernel.bin 一致，避免把"舞台镜像陈旧"误判成编译/代码问题。
CatOS[经验] 合并后全量回归分层跑：qemu 基础帧 → fsrec/caps/user/disk/env/heap/size 逐域 → ce_sim 主机侧；ce_sim 未建模 opcode（0x6a/0x08/ff /6 等）的 FAIL 不代表真机回归，同一应用在 QEMU 实机步骤 PASS 即可判定功能正常。
CatOS[经验] 测试报 FAIL 先分层定性：让受测应用直接 `run` 一遍，能跑通即目录条目/加载路径正常，问题在前端列举可见性或断言位置；再决定改测试还是改内核，避免把基建假阴性当内核缺陷修。
CatOS[经验] 多卷挂载去重应按「ATA 驱动号 + 分区 LBA」遍历全部卷槽（含根卷槽 0）判归属，而不是只扫附加挂载表的设备名；否则根分区可被二次绑定，两个 FAT 缓存覆盖同一 LBA 留下分叉风险。
CatOS[经验] 整文件读缓冲路径：空文件要在解析成功后直接返回 0（区别于读取错误），缓冲在申请长度 +1 处留尾 NUL，避免恰好读满缓冲时 tool_buf[rd]=0 越界写。
CatOS[经验] 终端审美整改的最小闭环：先截图取证建评分表（对照 Campbell/eza），再按「收集→排序→分列着色→右对齐」改列举类命令；表格类输出统一「标签深灰 / 主键强调色 / 次键灰 / 状态按语义着色」四层，列宽固定后视觉立刻整齐。
CatOS[经验] 提示行语义色分层：错误整行红，相邻的纠错建议用黄做次级提示、建议命令名用蓝高亮；问号紧贴命令词，不要留 `help ?` 这种空格。
CatOS[经验] 提示符三段配色惯例：用户@主机强调绿、路径蓝、权限标记（#/$）另用强调绿或白，不要让标记继承路径色；路径与标记之间不留多余空格。
CatOS[经验] 审美化改动也要过功能回归：本轮 ls 排序、df 对齐、pkg 分列均未改 API；唯一需要同步的是测试断言中隐含的顺序假设，先把断言改成与新排序无关的稳定条目/计数，再跑 qemu/fsrec/caps/user/disk/env/heap/size 套件。
CatOS[经验] 探测类自旋等待对空设备/开放总线应加「稳定无信号采样」跳出：状态恒定 0x00/0xFF 连续 N 次即判空槽返回，再配迭代兜底；避免无设备端口上跑满百万级空转。本轮 ATA 空槽由 10M 迭代/槽降到 256 采样即退，内核启动省约 1.65s。
CatOS[经验] 启动耗时诊断先打点拆分 pre-kernel（BIOS/GRUB/ISO 加载）/kernel markers/post-marker 三段，区分固件引导开销与内核开销；GRUB 菜单倒计时属于 pre-kernel，组装期 timeout 替换比内核侧优化收益更大（本轮 5s vs 内核 1.65s）。
CatOS[经验] did-you-mean 一键确认用三态状态机最稳：arm（主循环给出建议时缓存命令名+原始参数，脚本模式 g_script_mode 下不 arm）/ consume（裸 y/Y 且无尾随参数回放后立即清）/ cover（任意其他主循环行派发前清掉）。回放复用既有 `shell_exec()` 路径，补全表、argv 拆分、重定向语义与用户直接输入完全一致；回放前用 `-> <line>` 黄/蓝回显实际执行行，与原始 `y` 输入区分。问询上下文不持有该状态，故无需在每个确认点额外豁免。
CatOS[经验] 目录项写侧跨簇失败回滚：写入前记录每个已触碰簇号与槽位区间（起始槽+计数），中途 ATA 失败/链越界时按记录逐簇读回、把已写槽位墓碑为 0xE5 再写盘，避免悬挂半截 LFN run 被读侧误聚合；目录链尾追加新簇失败必须先解链（父簇指回原尾标记）再释放新簇，否则留孤儿链项。短条目 ntres(lcase) 字节按原始名大小写写：词干全小写置 0x10、扩展名全小写置 0x08，混合/全大写清零；LFN 别名恒大写、ntres=0。
CatOS[经验] CE 用户态 libc 雏形第一步取"头文件 inline 包装 + 单 .c 槽定义 + 构建脚本多链一个 .o"，比 ar 归档/独立 crt0 改动小：字符串/控制台辅助写成 static inline（优先走内核 vsyscall 槽，槽空回落 freestanding 实现），不产生跨 app 重复符号；真正的 libc.a 归档与启动代码分离留作后续小设计。
CatOS[经验] 递归文件操作语义分层：对外命令（rmdir）走严格内核校验（非空拒绝），对内递归清空器（rm -r）走 force 变体；后序遍历已自行删完所有后代，无需在内核侧重复非空扫描。cp 目标归一化先判 DST 是否已存在目录，是则拼上 src 基名（合并语义），否则原样作为覆盖/改名目标。
