# history.md - 易错点记录

格式：[所属项目][易错点]内容
判定准则：只要是因为管理不当或非技术原因导致的、细心就能避免的意外，就算易错点（2026-10-24 用户定稿）。

- [ot][易错点] execve 加载 CE 时 image 必须**跳过 16 字节 CE01 头**从 fb+16 拷入 0x02000000；若连头一起拷，0x02000000 处是 'CE01' 魔数数据，ce_main 实际在 +16 处，call 0x02000000 执行垃圾指令导致输出乱码（v1.9 乱码根因，已修）
- [ot][易错点] QEMU 新启动实例的 PS/2 键盘注入可能**完全失效**（HMP sendkey / QMP input-send-event / 长 hold 均 0% 成功率，实测 12+ 新实例全丢），仅长时间运行的老实例部分正常；VM 交互测试前先发单键探测键盘，不可假设键盘可用
- [ot][易错点] `pkill -f qemu-system` 会匹配 bash -c 包装进程的命令行导致**误杀自身 shell**（exit 137）；杀 QEMU 用 pidfile 或 `ps aux | grep "[q]emu" | awk '{print $2}' | xargs -r kill -9`
- [ot][易错点] exec_launch 用 i386 固定寄存器约束（S/D/c/a/d/b 共 6 个）触发 GCC "asm operand has impossible constraints"（32 位仅 8 个寄存器）；参数改走栈（"m" 内存操作数 push + push 立即数 + ret 跳转）
- [ot][易错点] QEMU monitor 是行式交互，新连接会先收到欢迎横幅（"type 'help' for more information"），脚本解析响应时勿把横幅当命令回显
- [ot][易错点] vga.py 的 q_cmd 超时 1s 会截断 xp 大输出（2000 字 dump 需 ~10s），monitor 命令积压拖垮后续 sendkey；读屏超时应 ≥10s
- [ot][易错点] v1.9 exec 跳板需完全位置无关（拷贝到固定区 0x0147c800 执行），跳板内引用外部符号地址（如 g_csc_fb）会导致跳板内容含绝对地址失效；用寄存器传递/自我计算替代
- [CatOS双线开发][易错点] 多项目并行共享同一 QEMU 环境时，用 `pkill -f qemu` / `pkill qemu` 全量清理会**连坐杀掉另一项目正在运行的 VM**（ot 链周期清理时游戏链 VM 被误杀 3 次）；正确做法：① 各项目用独立磁盘副本（adofai/disk-test.img）+ 独立 monitor socket（catos-mon.sock vs catos-mon2.sock）；② 清理只按自身 pidfile 精确 kill 或按 socket 文件归属匹配，禁止全杀；③ 共用 disk.img 时改盘前先确认对方未在用
- [ot][易错点] C 函数参数名遮蔽全局函数：my_readline(char *out, ...) 的参数 out 遮蔽了全局 out() 输出函数，函数体内调用 out() 报 "called object 'out' is not a function or function pointer"；参数名避免与全局函数重名，或改用 outln/out_int/直接 api->write0
- [ot][易错点] QEMU sendkey 长 hold 参数（sendkey o 100）会触发按键重复风暴，对已处于 #34 键盘失效的实例产生大量乱码注入（屏幕出现 0x13a14511 类内存 dump 乱码），实例即废；键盘失效时勿再用 hold 强化重试
- [ot][易错点] **`gcc ... | head -N` 管道会吞掉错误信息，且 `$?` 取的是 head 的退出码不是 gcc 的**：编译失败时旧 .o 文件保留，ld 仍用旧对象产出"新"产物 → 改码后产物不变（本轮回放：ACL 删除后 ocntools.ce 仍含 su/whoami 就是此坑）。判编译成败用 `gcc ...; echo $?`（无管道）或 `&&` 链；改码后必须核对产物内容/时间戳（strings/grep 产物关键字）
- [ot][易错点] qemu-img convert 在无 flock 支持的文件系统（hpvs_fs /home/user）报 "Failed to get resize/write lock"：目标路径换到 /tmp（overlay 支持锁），或 QEMU 磁盘镜像放 /tmp 再启动
- [ot][易错点] FAT32 目录项是 11 字节 8.3 名（无点）：注入脚本匹配名字要 `b'OCNTOOLSCE '`（名 8 字节 + 扩展 3 字节），写 `b'OCNTOOLS.CE '` 会匹配失败
- [ot][经验] Catos_Better_better 新内核 CE API 表：`ls` 带回调（ce_ls_cb：is_dir/name/size/ud），FS_DATA_MAX=512 未变、cat≤8KB 未变、无 delete/append/seek/swap；内核态有 uid 权限系统（perm 表，SYS_SAVE 强制检查）；用户态"重复实现"排查应以此 API 面为基线
- [ot][易错点] sed 替换串含 `&` 时 `&` 是"整个匹配"特殊符，替换文本里的 & 必须写 `\&`，否则 C 字符串被破坏（"save&quit" 两次被 sed 改写坏，最终用 Edit 工具精确替换修复）；同样地，build.sh 内联 python 正则若匹配已删除的源码数组（BANNER[] 外置后）会报 `AttributeError: 'NoneType' object has no attribute 'group'`，改版删除源码数据时要同步改检查脚本（build.sh banner width check 已改读 scripts/BANNER.TXT）
- [ot][易错点] push_files 内联参数必须填真实文件内容，不能填占位符后"再覆盖"——占位符会真的推上去（本次 audit/aesthetic/README/scripts 全被 PLACEHOLDER 覆盖过一轮，需二次推送真实内容修正，浪费一次 commit）
- [ot内核v2.5][易错点] QEMU 环境每轮重置：qemu-system-i386/qemu-img/磁盘都会消失，开工先 `sudo apt-get update && apt-get install -y qemu-system-x86 qemu-utils`，重建 4G 稀疏盘 + QEMU 内 `format` 命令**需二次确认 y** 才真正格式化
- [ot内核v2.5][易错点] 注入脚本 8.3 名必须与内核查询名完全一致：`minimal_ce.ce` 注入为 `MINIMAL_CE.CE` 时名字段截断为 `MINIMAL_`（10 字符超 8），内核查 `/bin/minimal.ce` 生成 `MINIMAL CE ` 不匹配 → err -2；注入用与查询一致的 `MINIMAL.CE` 即可
- [ot内核v2.5][易错点] shell 管道右侧**不能带 run 前缀**：`run /bin/gen_ce.ce | run /bin/sink_ce.ce` 会把右侧整串当命令路径（file2="run"）→ err -2；正确 `run /bin/gen_ce.ce | /bin/sink_ce.ce`
- [ot内核v2.5][易错点] cpu_switch_park_and_resume 保存 prev 为**协程上下文**（push ebp/ebx/esi/edi+esp），恢复 prev 必须走 cpu_switch_to；prev->resume_frame 不清 0 会误按 ISR 帧格式弹栈（pop gs 弹垃圾段 GP）——**调度让出方必须清 resume_frame，trap 入口清 t->resume_frame**
- [ot内核v2.5][易错点] tasks[] 是静态数组，task_spawn_ce 复用槽位时新字段（out_pipe/in_pipe/out_file/out_append）残留旧值 → pipe_task_cleanup 误释放管道；**任务创建必须显式初始化所有重定向字段为 -1/0**
- [node解释器][易错点] **tok_kw 用 token 长度当比较长度**：1 字符标识符 'f'/'n' 与 "false"/"null" 首字符相同即误判关键字命中 → 函数名 f 被解析成布尔字面量（"not a function"）、参数 n 被解析成 null（"bad binary op"）；关键字匹配必须：词元长度==关键字长度 && 全词 str_eqn
- [node解释器][易错点] **n_new 只初始化 op 字段**：链尾 next / 未用 a/b/c/body/flag/num/sp 全是 arena 垃圾 → 语句链/声明符链/实参链遍历读幽灵节点（报 callee-op=3 类"not a function"、非确定 OOM）；Node 分配必须全字段清零
- [node解释器][易错点] **解析错误恢复死循环**：perr 后 p_stmt 不消费 token，p_stmt_list 无限循环分配 AST → arena 耗尽"out of memory (lex)"（且非确定）；错误恢复必须"无进展强制 tadv 推进"
- [node解释器][易错点] **单字符 '<' '>' 词法器存原始字符(60/62)，求值器期望 OP_LT=3/OP_GT=6** → "bad binary op"；parser 层要把单字符比较符映射到 OP_* 常量（'=='/'<=' 等双字符已有映射，单字符漏了）
- [node解释器][易错点] `typeof x`（无括号）解析成相邻两标识符 → perr "expected )"；实现 `typeof` 为 unary 关键字（转 E_CALL 内建）即可支持两种写法
- [node解释器][易错点] 宿主侧验证命令：`gcc -O2 -w -I extapp -o /tmp/node_test extapp/node.c extapp/host_stub.c`；CE 构建 `sh extapp/build_ce.sh node.c`（从内核根目录、文件名不带路径，带路径会 cc1 找不到文件）；改 node.c 后两步都要跑
- [node解释器][易错点] 数组越界访问返回 VAL_UNDEF → `total + arr[i]` 报 "+ needs numbers/strings"（demo 曾用 `i<5` 配 3 元素数组自踩）；脚本边界与解释器严格报错行为要区分
- [ot内核v2.5/工具链][易错点] **构建工具与内核常量各自硬编码会漂移**：内核 v2.5 把 `CE_MAX_IMAGE` 从 64 KiB 提到 256 KiB（`include/ce.h` 0x40000），但两份 `mkce.py` 仍按 `0x10000` 拒绝镜像；自举产物 CC2.CE≈82KB 在宿主构建阶段就被拒，表现为「编译器正常、镜像出不来」。旧版对 80 KiB 镜像实测输出 `error: image too big (81920 bytes, max 65536)`；修法 = 运行时解析 ce.h 作唯一真值源 + 两份副本字节一致断言
- [ot内核/合流][记录] 2026-11-07 master 合流完成：render-split v2.7..v2.10（VMA P0、TD-01 修复、mmap/munmap slots 55/56、ELF32 PIE+ASLR 加载器、CE 回归套件 ce_regress.py、渲染前后端分离、拼音 IME）经 `662e7b3` 并入 master，并发 WIP 先以 `31cc3ed` 快照保护；工具链分支 `workbuddy/todo-20261106-1906`（mkce.py 上限运行时对齐 include/ce.h、构建期前移加载校验、tests/build_ce.sh 薄包装、test_mkce.py 48 例）经 `46b11d3` 并入；合并后 master 上 test_mkce 48/48 通过。此后以 D:/CatOs master 为唯一主线，D:/CatOs-render-split 仅作工作克隆
- [ot内核v2.5/工具链][易错点] `tests/build_ce.sh` 是 extapp 脚本的旧副本，却按 `tests/` 拼 `ce.ld` / `ce_stub.c` / `$ROOT/build/mkce.py`，三者全不存在，实测 exit 127 从未可用；改为薄包装委托 `kernel_v25/extapp/build_ce.sh`，只保留一套真实工具链
- [ot内核v2.5/工具链][易错点] 构建期不校验 bss / api_offset，错误只在 QEMU 里以 `task_spawn_ce` err -5 暴露；把加载器三条早退判定（img>CE_MAX_IMAGE、bss>CE_MAX_BSS、api+4>img）前移到 mkce.py。注意 `api+4 == img` 是合法边界（实测 spawn_fail.ce：image=4408 / api_off=4404），写成 `>=` 会误伤真实产物
- [ot内核v2.5/工具链][易错点] 本机跑 MinGW 构建脚本须显式给工具链 PATH 与 `PYTHON`（`PATH=/c/msys64/mingw32/bin:/usr/bin:/bin PYTHON=<python> sh tests/build_ce.sh ...`）；只设 PATH 会因 python 不在 PATH 报 `ERROR: no python found`，不设 PATH 则 gcc 以 127 失败
- [ot内核v2.5/工具链][易错点] **回退值与头文件恰好相等会让测试变假阳性**：`load_limits()` 找不到 ce.h 时回退内置 256 KiB，而 ce.h 当前也是 256 KiB，只断言「脚本读到的上限 == 头文件上限」在相对路径完全失效时照样全绿；必须连第三个返回值 `source`（是否指向真实 ce.h 路径）一起断言
- [ot内核v2.5/工具链][易错点] 失败用例复用同一个输出文件路径时，「拒绝后不应产出文件」的断言会被上一次成功用例的残留文件误伤；每个拒绝用例运行前先删除输出文件，再判 `not os.path.exists(out)`
- [CatOS/合流后][易错点] **构建必须 `(cd /d/CatOs/kernel_v25; env -i ... sh build.sh iso)` 子 shell**：会话工作目录可能停在旧工作树（`master-87a3e1a0`），直接跑会在旧代码上构建出旧 ISO，症状是"改了不生效"且不报错；cwd 永远显式声明
- [CatOS/合流后][易错点] MinGW 不在 PATH 时 gcc **静默 exit 1 且零输出**（不报 command not found 也不报编译错）；构建命令必须显式 `PATH=/d/msys/mingw32/bin:/usr/bin:/bin`，失败时先查退出码再查输出
- [CatOS/合流后][易错点] git ignore 规则是 **last-match-wins**：`*.iso` 的否定规则必须放在 `*.iso` **之后**；且 `git check-ignore` 对**已跟踪**文件不做检查，必须加 `--no-index` 才能验跟踪文件的 ignore 状态
- [CatOS/合流后][易错点] Python heredoc（`python - <<'EOF'`）做多行字符串替换在本机不可靠（替换静默不生效）；文本替换一律用编辑工具精确匹配，批量同构替换才用 python 脚本文件
- [CatOS/#90 mkdisk][易错点] bytearray 切片赋值会**自动扩容**：`d[0:11] = 22字节` 把 32 字节目录项撑成 43 字节且不报错；dent() 必须加 11 字节长度断言
- [CatOS/#90 mkdisk][易错点] `pe = mbr[446:]` 是切片**拷贝**不是视图——往 pe 写分区表等于写进副本，MBR 全零；要么用内存视图要么回写
- [CatOS/#90 mkdisk][易错点] FAT 表只在 write_metadata 落盘一次、alloc() 后未回写 → 目录项指向的簇在 FAT 里全 0，`ls` 能看到文件但 `run` err -4（"文件系统看着正常"的假象）；verify() 必须从**镜像字节**读 FAT 走链，不能信内存镜像
- [CatOS/共享机器][易错点] `taskkill /F /IM qemu-system-i386.exe` 会杀掉全机器所有人的 guest；只能按 PID 杀（`tests/qemu_driver.py` 的 `kill_qemu(proc)`），每个回归驱动用自己的 monitor 端口
- [CatOS/QEMU][易错点] `-soundhw` 在 QEMU 7.1 已移除，SB16 只能 `-device sb16,...,audiodev=snd0` + `-audiodev wav,id=snd0,path=<Windows 绝对路径>`；wav 输出的 RIFF/data 长度字段不可信（`quit` 不保证触发 `wav_fini_out` 回填），解析时自己 patch 长度再用 `wave` 模块复验
- [CatOS/内核][易错点] **syscall 跑在调用者 CR3 不是 kdir**：任何高 PDE 内核映射（如 VBE LFB）必须镜像进每个任务页目录，否则只在 ring0 上下文测试通过、ring3 一跑就 triple fault；VBE LFB 的 PTE 保持无 US 位即可不破坏"用户不可访问 VRAM"
- [CatOS/内核][易错点] `validate_uaddr(t, addr, 1)` 后按 n 字节裸写用户指针是 P0 惯犯模式（#78/#86 同根因）：第 2 页未映射时 ring0 #PF 直接停机；统一做法是按真实上界校验 + `copy_to_user()`/`copy_from_user()` 页表拷贝，未知长度字符串走 `u2k_str()` 内核快照
- [CatOS/v2.14 ocntools][易错点] 老 zip 里的 CE 应用三处时代残留缺一不可全修：入口必须在 `.text.ce_entry` 段（新构建链按段找入口）、API 指针必须是**全局** `__ce_api`（nm 提取要求 [DR] 符号，`static` 会丢）、`if (!api) { api->write(...) }` 是 NULL 解引用（正确写法是打印常量后 return -1）
- [CatOS/v2.14 回归][易错点] shell `format` 的确认输入走 `kb_readline`——**发 y 之后必须发 Enter**，否则确认读不完、后续所有键入被吞（表现为"format 后内核卡死"，实际是测试驱动没发回车）
- [CatOS/v2.14 回归][易错点] format 成功后内核要**重装内置应用到磁盘**，耗时数秒；期间键入全部丢失——必须轮询屏幕出现 "Installed missing built-in" 再发下一条命令，固定 sleep 2.5s 不够
- [CatOS/v2.14 回归][易错点] FAT `ls` 显示的是 8.3 目录项里**原创建时的大小写**（写 `C1.TAD` 删后显示 `c1.tad`，大小写转换取决于创建路径），文件名断言必须做大小写不敏感匹配
- [CatOS/v2.14 回归][易错点] HMP `screendump` 异步落盘 + 读屏折叠空格，关键断言前先做一次"沉淀读"（读屏只为触发等待）再取值断言；否则偶发读到半更新屏造成 flaky

## 2026-11-14 跨夜任务（v2.22 轮）易错点
- [CatOS][易错点] Windows 打包的 CatOs.7z 快照文本文件是 CRLF，Linux dash 下 sh 脚本报 "set: Illegal option -"：解包后先 dos2unix 归一 build 关键文件（v2.19 先例：归一前后 kernel.bin 字节一致）。
- [CatOS][易错点] /home/user 挂载为 hpvs_fs（不支持 QEMU 需要的 OFD 文件锁），QEMU 直接打开镜像报 "Failed to get write lock"：qemu_driver.boot 已统一把 iso/disk 暂存到 /tmp 再启动。
- [CatOS][易错点] QEMU 6.2 HMP `xp` 输出 16 位地址（00000000000b8000:），旧测试脚本按 8 位地址正则匹配全空导致 wait_for_prompt 全挂：正则已放宽为 ^[0-9a-fA-F]{8,16}:。
- [CatOS][易错点] 改内核后必须 sh build.sh iso 重打包；只 build kernel.bin 时 QEMU 仍引导旧 ISO，会得出"改动没生效"的假结论。
- [CatOS][易错点] /etc/passwd 旧格式第 7 字段是 uint32 passhash，"0" 表示无口令；v2.22 迁移为字符串 pw 字段时必须在 user_db_init 里把 legacy "0" 归一为空串，否则旧盘上 root 被当成"已设未知口令"锁在门外。
- [CatOS][易错点] env_expand 首版在扫描 key 前没跳过 '$' 本身，"$A" 因 '$' 非合法 key 字符而 k=0 → 死循环把 256 字节缓冲灌满 '$'（QEMU 上表现为 "run $A" 变 200 个 $ 的路径）。教训：解析变量引用时先显式 i+1 越过 '$' 再扫 key，且这类纯函数先用宿主 gcc 单测复现再上实机。
- [CatOS][易错点] 宿主 ce_sim（Python CE 模拟器）只认 MinGW gcc 的指令编码，Linux gcc 产物报 unsupported opcode 0x6a（push imm8）等：宿主快速回归在 Linux 上需先扩 ce_sim 的 opcode 表，FAIL 不代表内核问题。
CatOS[易错点] QEMU monitor screendump 偶发不产文件或 socket BrokenPipe（guest 三重故障 -no-reboot 直接退出）：测试脚本必须对 screendump 做 3 次重试并先删旧文件；断管时先怀疑 guest 崩溃而非脚本。
CatOS[易错点] 像素断言不能读 0xB8000（仅文本模式有效）；vbe24/gfx 模式断言一律走 screendump PPM。定位"输入行是否在屏幕中部"可用提示符前景色（LIGHT_GREEN=0x55FF55）的像素行带 + 从底部向上取连续绿带，避免屏上方残留旧 prompt 干扰 min_y。
CatOS[易错点] useq() 相等返回 1（*a==*b），从 strcmp 迁移比较条件时 `==0` 会反转语义（实机表现为 preset 'viewer' 匹配到 'admin' caps=0xFFFFFFFF）；改造比较逻辑时必须按返回值语义重写条件。
CatOS[易错点] `format` 是交互式确认命令（Proceed? y/n），回归测试用它做"门是否打开"探针时，后续命令会被 y/n 提示吞掉；探针后必须显式应答 n，或改用无副作用命令。
CatOS[易错点] Bash 里 `pkill -f qemu-system-i386` 会自杀（-f 匹配到自身命令行文本），用 `pkill -x qemu-system-i386` 按进程名精确匹配。
CatOS[易错点] UHCI 的 QH element pointer 是硬件 scratch 寄存器：控制器每完成一个 TD 就把 TD[0]（next 指针）写回 QH[1]；轮询式驱动每次重排 TD 必须同时重写 QH[1]，否则队列被 TERM 后永远沉默。
CatOS[易错点] UHCI FRBASEADD 是 32 位 I/O 寄存器，用 outw 只写低 16 位（帧表基址变成 0，控制器读中断向量表当帧表、FRNUM 照常走但永无传输）；4 字节寄存器一律 outl。
CatOS[易错点] CatOS 的 IRQ0 真入口是 cpu.s _irq_0 → task.c sched_timer_tick()；intr.c isr_irq 的 case 0 是永远不可达的死代码，往那里挂每 tick 钩子会静默失效（本次 usb_poll 中招 2 小时）。
CatOS[易错点] QEMU -usbdevice keyboard 附加后 sendkey 的键盘事件只进 USB HID 设备，PS/2 i8042 收不到任何键——测试 USB 键盘时类型注入必须走 USB 路径。
CatOS[易错点] QEMU PIIX3 的 UHCI 在 PCI dev1.fn2（8086:7020），PCI 扫描必须遍历 function，只扫 fn0 会漏掉。
CatOS[易错点] build.sh 的链接失败信息是 "undefined reference"（不含 "error" 字样），用 grep error 过滤构建输出会把链接失败漏判成成功；应过滤 'error|undefined' 并检查产物 mtime。
CatOS[易错点] QEMU 6.2 的 monitor device_add 设备名是 usb-kbd，-usbdevice keyboard 的别名 usb-keyboard 会报 "not a valid device model name"。
CatOS[易错点] QEMU 有 USB 键盘设备时 sendkey 事件只路由给 USB 设备（PS/2 收不到）；无 USB 键盘时 sendkey 走 PS/2——测试键盘路径前先确认当前 QEMU 设备构成。
CatOS[易错点] tick 上下文（IRQ0 处理路径）里不能跑带 usb_delay_ticks 忙等的代码：deadline 依赖 timer_ticks 前进而 tick 本身不返回，死循环；热插拔要用"tick 置标志 + 任务上下文消费"两段式。
CatOS[易错点] task_block(T_WAIT_KB) 无唤醒源时会永远睡死（USB 收割前 feed_report 才唤醒）——需要在等待循环里周期性调用 usb_service() 的场景应改 task_sleep_ticks 轮询。
CatOS[易错点] fsrec 回归必须带 -drive 硬盘（disk= 参数），无盘时 format/挂载路径行为完全不同——复现 fs 相关问题先确认盘是否存在（#116 教训）。
CatOS[易错点] ata.c 各等待循环上限 10M 次迭代（每次 inb+yield，秒级）：底层超时返回错误后，上层必须检查返回值，否则"有界等待"叠加成用户眼中的死循环。
CatOS[易错点] FAT32 驱动里 max_clus 是 FAT 表条目容量（fat_sz*512/4），不是数据区簇数；数据簇号上界必须用 vol_clusters+2，否则小卷上越界分配/统计翻倍（#118）。
CatOS[易错点] 新增跨模块调用（如 keyboard.c 调 usb_service）必须同步 #include 头文件，否则隐式声明默认返回 int 编译不报错但留下 ABI 隐患。
CatOS[易错点] qemu_driver.type_text 的特殊字符映射不全时（如反斜杠、@、#）字符会静默丢失——测试输入含符号的字符串前先确认映射表覆盖。
CatOS[易错点] Ubuntu 环境可能随时重置：qemu-system-i386 等已装工具会消失，表现为所有回归 FATAL "shell prompt never appeared"——排查顺序：which qemu-system-i386 先确认工具存在，再怀疑代码；重装用 sudo apt-get update && sudo apt-get install -y qemu-system-x86。
CatOS[易错点] git checkout <old-commit> -- kernel_v25 会静默回滚源码——排查环境问题时回滚验证后必须立刻恢复 HEAD 版本。
CatOS[易错点] 环境重置后 /usr/bin/qemu-system-i386 可能消失：真实可用入口是包装脚本 /home/user/bin/qemu-system-i386（内部 export LD_LIBRARY_PATH=/home/user/qemu-root/usr/lib/x86_64-linux-gnu:/home/user/qemu-root/lib/x86_64-linux-gnu 与 QEMU_MODULE_DIR，再 -L /home/user/qemu-bios 启动 /home/user/qemu-root/usr/bin/qemu-system-i386）；直接调裸二进制缺 libpmem/libfdt/librdmacm 等库起不来。回归/探针务必 --qemu 显式传包装脚本。
CatOS[易错点] ring0 shell 直调 fs_touch/fs_mkdir/fs_copy_file 时 ACL/fs_perm_check 不生效（守卫只在 syscall 层和 shell 显式调用点）；每新增一个"创建/删除目录条目"的 shell 命令，必须在命令层补 perm_write_on_parent（父目录写权限），#112 补了 mkdir/cp -r，#121 漏网的 touch 本轮补上。
CatOS[易错点] 覆盖已有字符串表项前必须先把旧缓冲清零再拼接：strcat_b 是追加语义不是替换；alias 表项重复赋值未清空旧值会得到旧值+新值的拼接串（#120）。
CatOS[易错点] 整屏 read_screen 会残留上一行 "permission denied"：判定新命令被拒/允许时，先 clear 再用 ls 结果（文件是否列出）复核，不要只在整屏文本里 grep denied，否则会把允许误判成拒绝（#121 探针虚惊）。
CatOS[易错点] v2.37 改终端主题只动 render.c clut_init() 的 vga16 表即可三端联动（text 后端据此编 DAC、vbe/vbe24 真彩后端直接取 CLUT RGB），不要逐后端找色值；但 gfx24 demo 是直接写 LFB 的固定诊断图，其方块/渐变/色条/底色被 tests/gfx24_regress.py 逐字节 eq_px 断言，改 demo 任何颜色必须同步改该套件期望值，否则 gfx24 回归挂。
CatOS[易错点] v24_set_scale 只改网格 cols/rows/字号不清 LFB，切换 size 档位后旧 demo 像素会残留在新网格外；必须在 set_scale 布局完成后整帧 gfx24_clear(主题底色)。
CatOS[易错点] fsrec_regress 偶发 exit 143（SIGTERM）多为上一个 qemu 进程没退干净、占用磁盘镜像，并非内核回归；先 pkill -x qemu-system-i386 再重跑即过，勿误判成新缺陷。
CatOS[易错点] 端口段：本代理独占 4570-4585，4444-4539 与 4586-4599 是并行审美代理保留段；gfx24_regress 硬编码 4462 落在保留段，本轮冒烟不要启动它，避免撞 monitor。
CatOS[易错点] 云端 Ubuntu 主机 cgroup memory.max≈4GiB、总内存 3.9GiB，QEMU `-m 4096` 启动即 "Cannot allocate memory"；实测可行上限约 3840MiB，且客户机 multiboot mem_upper 仍被 PCI hole 截到 ~3071MiB。大内存验证按主机余量降档（256/1024/3840），不要照搬 -m 4096，也不要把 mem_upper 小于 -m 标称值当成内存泄漏。
CatOS[易错点] 32 位内核做 KB→字节换算：mem_upper_kb<<10 在近 4GiB 会 uint32 回绕；必须用 uint64 中间量 `(uint64_t)kb*1024ull` 算完再截到 32 位物理地址，4GiB 本身不可表示，封顶到 0xFFFFF000。
CatOS[易错点] `pkill -f qemu-system-i386` 或 `pkill -f 端口号` 会匹配 `bash -c` 包装进程自身的命令行，把自己 shell 杀掉（exit 143）；清理 guest 只按 PID kill_qemu(proc)，不要用 -f 全命令行匹配。
CatOS[易错点] env_regress.py / size_regress.py 第一个位置参数是 catos.iso 路径，`--qemu` 是其后选项；把 --qemu 放首位会被当成 iso 路径，报 FileNotFoundError。
CatOS[易错点] size_regress.py 的绿带检测常量 GREEN=(0x55,0xFF,0x55) 是旧主题值；现代主题强调绿已是 #7EE787=(126,231,135)，基线（未改源码）跑同样 normal/compact/large-band FAIL。遇到该脚本 band 失败先 A/B 对照基线，勿误判成新回归；/etc/size 持久化检查在无磁盘镜像的 size_regress 流程里也会失败，属脚本未挂盘的既有现象。
CatOS[易错点] PE/COFF 段头（section header）开头有 8 字节 Name 字段，内核镜像结构体 pe_section_t 不能省：漏写 Name 后 VirtualSize/VirtualAddress/SizeOfRawData/PointerToRawData 整体错位 8 字节，字段读出全错、边界校验直接返回 -5，且编译期不报错；解析二进制格式时结构体字段顺序/偏移必须与磁盘布局逐字段对齐，最好用 objdump -h 与 Python 解包双重复核。
CatOS[易错点] qemu_driver._stage_image() 把 iso/磁盘拷到 /tmp/catos_qemu_stage 并在进程退出后删除，跨两遍 guest 的格式化/注入写入不会落回原始磁盘；自己写两遍驱动必须把磁盘暂存到本地 /tmp 目录，并在阶段间显式拷回原始磁盘再注入。工作区文件系统（hpvs_fs）不支持 QEMU image flock，直接在工作区路径起 guest 会报 "Failed to get write lock"，磁盘镜像必须放本地 /tmp。
CatOS[易错点] shell 交互询问可能是多步（PE->CE 先问 y/n，再问保存文件名，回车用默认）；自动化驱动必须对每一步提示各发一次按键，漏掉第二问会卡在文件名 readline，后续命令全部丢失。format 命令同理需要 y 后再回车。
CatOS[易错点] 并行分片共享 /tmp 暂存目录时，_stage_image 按同一 basename 拷贝镜像，QEMU 的 OFD write lock 会拒绝第二台 guest（"Failed to get write lock"，monitor 起不来，脚本误报 "could not reach the QEMU monitor"）；暂存目录必须按 PID（或分片 id）建独立子目录，磁盘副本也要用不同文件名。
CatOS[易错点] 用 C 宏把旧全局几何变量映射到卷结构体字段时，结构体成员名不能与宏同名——否则成员声明 `uint32_t part_start;` 会被预处理展开成 `uint32_t (V()->part_start);` 这类非法表达式；结构体字段改用 d_ 前缀（d_start/d_fatsz...），宏再映射过去。
CatOS[易错点] shell 自定义负数返回码不能撞 fs.h 既有 FS_ERR_* 段（NOENT=-1/NOTDIR=-2/INVAL=-3/FULL=-4/EXIST=-5）；否则 switch 出现 duplicate case value。mount/umount 的细分错误码从 -10 起避让。
CatOS[易错点] qemu_regress 启动时会清空 SHOTS/*.png，跨分片或跨轮次连续跑回归会把上一轮的证据截图一并删掉；验收截图必须在所有回归跑完之后再生成最后一轮，不要提前留存。
CatOS[易错点] 回归脚本硬编码 monitor 端口，并行分片各占一段；端口被占表现为连接 refused / "could not reach monitor"，不是内核故障。给 disk_regress、fsrec_regress 补 --port 覆盖，用本分片端口段（4720-4729）。
CatOS[易错点] shell.c 新增命令函数若调用了文件中靠后定义的静态 helper（如 atoi_），编译器先按隐式非静态声明处理，后面 static 定义会报 "static declaration follows non-static declaration"；要么补前置声明，要么在新函数内手写数字解析，不要直接复用后置静态函数。
CatOS[易错点] 给 shell 命令参数做临时分词（插 NUL 截断）时，形参必须是 char*（可变）；写成 const char* 后 *q=0 会报 "assignment of read-only location"。
CatOS[易错点] qemu_regress.py 启动时会 glob 删除 tests/qemu_shots/*.png，探针后生成的截图会被回归步骤清掉；正确顺序是先跑回归、最后再截探针图，或把探针截图放到独立目录。
CatOS[易错点] fortune 类随机源若只取 RTC 秒/分，同一秒内连续调用会落到同一格言；熵要混入 timer_ticks（10ms 刻度）与单调递增序列计数器（乘 2654435761 散列）。
CatOS[易错点] 80 列文本终端里气泡/格言文案超过约 72 字节会折行，折行段丢失左右边框；内置长文案按字节长度裁剪，中文 UTF-8 每字 3 字节要计入宽度。
CatOS[易错点] 前台 Bash 跑长 QEMU 探针偶发 exit 143（SIGTERM）且无输出；改 nohup 后台跑 + 日志文件即可复现，不是内核或探针逻辑崩溃。
CatOS[易错点] .catpkg 定长头 MAGIC 槽位宽度必须与实际字节数一致：mkpkg.py 声明 magic[8] 却只写了 7 字节（"CPKG01\0"），打包断言 len(header)==HEADER_SIZE 直接失败；定长头字段要在打包/解包两侧用同一常量核对，新增字段先改头布局再改两侧。
CatOS[易错点] 包构建顺序：gen_builtin.py 嵌入 .catpkg 之前，build.sh 必须先跑 mkpkg.py 生成容器；顺序反了会嵌入旧负载或缺文件，gen_builtin 直接 ERROR 退出。
CatOS[易错点] FAT32 无 LFN，包名/命令名/容器扩展名必须 8.3 友好（stem≤8、ext≤3）；P4 容器扩展名取 .catpkg（CAT），命令名 bc/stats/rps 均在 8 字符内。
CatOS[易错点] shell 内建工具整文件读缓冲要按最大目标文件预留：API.TXT 约 34.9 KiB，16 KiB 缓冲会误报 "file too large"；bss 静态缓冲不占镜像体积，可放大到 48 KiB。
CatOS[易错点] 并行分片共用 qemu_driver._stage_image 的 /tmp/catos_qemu_stage 目录，文件名固定 catos.iso/qemu_disk.img，并发 agent 互相覆盖；本分片曾因此启动别人陈旧内核，shell 新增命令全部报 "unknown command"。端到端驱动必须用私有舞台目录（/tmp/catos_p4_stage），iso 与磁盘两份都要私有复制。
CatOS[易错点] QEMU OFD 写锁：舞台目录若放在工作区 hpvs_fs（/home/user/...）上，文件系统不支持 flock，镜像一打开即 "Failed to get write lock"，qemu rc=1 退出；即使是私有目录也不能放工作区，舞台必须落在 /tmp（overlay，支持 flock）。
CatOS[易错点] 诊断"命令表加了命令却 unknown command"：先 strings 核对本地 kernel.bin 与 catos.iso 是否真含新命令串；镜像含串而 guest 不认，基本是舞台目录被并发覆盖（陈旧内核），不是内核代码缺陷。
CatOS[易错点] 整目录 `ls` 条目数超过 25 行文本屏后，早期安装的条目会滚出可见帧缓冲；回归若靠整屏抓名字做断言会假阴性（heap_regress ls-bin 踩过）。查单文件存在性走 `ls -l /bin/<file>` 单条目分支（每条一行不滚动），不要依赖整屏可见列表。
CatOS[易错点] 屏幕回显断言不能用 `text.rstrip().endswith(值)`：命令执行后尾随重绘的 shell prompt 会占末行，把值顶掉（size_regress size-persisted 踩过）。按命令回显位置用正则取值，例如 `/etc/size\s+0(?!\d)`。
CatOS[易错点] 回归驱动的 Python stdout 是块缓冲落盘，分步 PASS 行滞后于 guest 实际状态；日志短时不刷新不等于卡死，判读以进程退出码和汇总行（steps=/PASS=）为准。
CatOS[易错点] 探针清屏前的旧回显会留在 25 行网格里：umount 后 ls /docs、umount 后 df 这类"目录已空/行已消失"断言，若不先 `clear`，substring 会匹配到上一屏残留文本造成假阴性；清屏后再抓当前帧。
CatOS[易错点] mount 要求挂载点目录已预建：目录不存在时先返回 "target directory not found"，走不到去重/非空/占用判定；测重复挂载、根卷重绑前先 mkdir 各测试点。
CatOS[易错点] 控制台回滚查看器只画最近 rows-1 条存档行：help 顶部彩蛋行滚出后无法在真机回看，help 是否登记要靠源码 help_row 调用点 + kernel.bin rodata 字符串交叉取证，不能只看可见帧。
CatOS[易错点] `strings` 默认最小长度 4，2~3 字符命令名（sl/wc/df）抓不到；核对短串用 `strings -n 2`。
CatOS[易错点] 文本模式调色板：改了共享 CLUT 不等于文本后端生效——`be_text.c` 的 DAC 仍要在 `text_dac_reset()` 里从 `render_clut(i)` 重新写一遍 6-bit 寄存器；否则 text↔gfx 往返后文本模式会吃到 gfx 遗留的 256 色 DAC。
CatOS[易错点] `render_rgb_to_vga` 阈值量化会把 `#60A5FA` 蓝判成青色槽（b/g 都≥128）；主题色要先在 CLUT 0..15 做最近邻匹配，命中就直接用槽号，别再走 3-bit BGR 阈值。
CatOS[易错点] shell 内建树遍历/多行换行不要在栈上放大缓冲：`names[24][FS_NAME_MAX]` 再加递归前缀串和三行换行缓冲，在当前 shell 栈容量下会刷出同串 `?` 乱码并污染后续帧；条目收集应走静态 bss 或堆上缓冲，递归连接器同理。
CatOS[易错点] 排序后回归断言不能再依赖 FAT 序的尾部条目：`disk_regress ls-bin` 原先抓 `node.ce`/`syn.go`（FAT 序尾部），字母序排序后尾部变为 zhdemo；断言要换成字母序末位或计数行，否则假阴性。
CatOS[易错点] 框线长度要用显示宽度而不是字节数：UTF-8 CJK 一字三字节两列宽，按 slen 算边框会偏短、右侧竖条错位；写一个 disp_width() 按 lead 字节判 1/2 列宽。
CatOS[易错点] 启动计时探针 read_screen 固定 2.0s 等待，会把 S9 之后的 harness 轮询粒度误判为内核耗时（约 2.15s 墙钟）；内核就绪必须以 S9 uptime 为准，墙钟三段拆分时要把 post-last-marker 单独标注为探测脚本 artifact，不能计入内核启动时间。
CatOS[易错点] 全量回归一键脚本在 600s 前台预算内会被 SIGTERM（exit 143），size 套件环境侧 PPM 解析/monitor 往返慢（每步约 2 分钟）；长套件必须后台 nohup + 轮询日志，且按日志时间戳确认是本轮新鲜产物——旧基线日志（Nov16）不能复用，被中断的套件只跑了前几个检查点，重跑要数全 PASS 条数。
CatOS[易错点] `screen_text()` 只转储当前 25 行可视区：回放/提示后紧跟长列表（ls 18 条、/bin 三十余条）会把 shell 刚打印的 `-> <cmd>` 回显行滚出屏幕，断言回显标记会假阴性。选单行输出的建议目标（date/whoami）做回显断言；长列表用例改断言列表条目内容（如注入的 pesample.exe），不要追滚动中的提示行。
CatOS[易错点] `pkill -f qemu-system-i386` 会匹配到执行该命令的自身 `bash -c` 命令行（串里含同样的二进制名），把当前 shell 一起杀掉，后台任务瞬间 exit 1、子进程群被连带回收；清残留要先 `pgrep -x qemu-system-i386` 按 PID 精确杀，或命令行里规避该子串。
CatOS[易错点] pending 类交互状态（did-you-mean 待确认）的识别只能挂主提示符循环：命令内部的 y/n 问询（PE->CE offer、format 闸门、gfx consent、useradd wizard）走各自 `kb_readline`/`kb_getkey`，不经主循环，天然隔离；切勿把「输入是 y 就回放」下沉到 readline 全局层，否则会污染所有问询上下文。
CatOS[易错点] extapp/build 残留半成品中间件会让全量重建在某应用上挂掉：上次被中断的 .exe 留下残缺 PE，下次 ld 覆盖前 nm/objdump 报 "file format not recognized"，继而 "ce_main not at image offset 0"；单独重编该 app 又能过，假象是偶发。整盘重建前先清 `extapp/build/*.exe *.o *.bin *.nobss.exe`。
CatOS[易错点] LFN 条目读回保留原始大小写，只有无损 8.3 短条目路径才统一小写展示；回归断言混合大小写文件名时要分两条路径写期望（8.3→小写，LFN→原样），不能一律按小写 substring 匹配造成假阴性。
CatOS[易错点] 公共层把 __ce_api 槽收编成单个 .c 定义时，若用强符号链接进所有 app，未改造 app 本地的槽定义会撞 "multiple definition"；过渡期公共定义必须标 weak，旧 app 强定义自动覆盖，改造一个迁一个，最后再去掉 weak。
CatOS[易错点] `fs_rmdir` 非空判定回调 `rmdir_cb` 旧条件 `!scmp(name,".") && !scmp(name,"..")` 要求名字同时等于 "." 和 ".."，恒假，计数永远 0，非空目录被当成空目录放行；正确写法是 `scmp(name,".")!=0 && scmp(name,"..")!=0`（跳过两个点条目后计数真实条目）。
CatOS[易错点] 严格化目录非空检查后，既有 `rm -r` 递归清空器暴露出 fat 层嵌套 delete-name 潜在重入问题（见问题清单 #155）；本批把 `fs_rmdir` 拆成 strict（rmdir 命令）/force（rm -r 后序清空器）两条路径，force 保持 v2.24 基线行为，不在本批改 fat 内部。
CatOS[易错点] /etc/passwd 长口令记录在 VGA 按 80 列折行，回归抓屏文本里十六进制串会被换行/空格打断；正则匹配前先 `re.sub(r"\s+","",txt)` 归一化，否则 128 位 digest 的 32 hex 断言假阴性。
CatOS[易错点] 任务退出路径先唤醒 parent 再 maybe_autoreap，父进程被置 READY 后 autoreap 判定「无人等待」把僵尸直接回收成 T_UNUSED；等待方若只认 ZOMBIE（如 task_wait_child_or_stop）就永远等一个已回收槽位死循环。退出路径必须「先 reap 判定（parent 仍在 WAIT_CHILD 才保留僵尸）、后唤醒 parent」，且 wait 类循环要防御性接受 T_UNUSED 作为子任务已退出。
CatOS[易错点] QEMU 开机后立刻 sendkey/type_text，PS/2 控制器自检/初始化残留会产生幽灵按键（屏幕出现 18118128138 一类数字噪声）污染第一条命令；开机后先丢弃性 type 一次或等 1-2s 再开始正式断言输入，否则 argv 里混入垃圾尾巴导致 bg 解析、参数断言随机假阴性。
CatOS[易错点] fs_perm_check 里文件 mode 是经典 octal triad（r=4/w=2/x=1），而 syscall need 是独立 PERM_READ=1/WRITE=2/EXEC=4 位掩码；直接 `(have & need)` 相与会把 4xx 读位误拒、把执行位 1 误当成 READ 放行。取出 triad 后必须先翻译成 PERM_* 再比对（见问题清单 #157）；附加组/组位分支挂在组位判定上，掩码翻译错了组可读访问会整体假阴性。
CatOS[易错点] 附加组成员在磁盘 /etc/group 上按用户名存，加载时才对 /etc/passwd 解析成 uid；权限检查路径只能拿 uid 查组库（user_in_group），不能要求任务凭据里带附加组数组。老记录只有主 gid：组判定先比主 gid，未命中再遍历附加组，缺省组库为空时行为与单 gid 基线一致。
CatOS[易错点] 后台/重定向 CE 应用（如 `hello > file`）是异步任务，chown/chmod 紧随重定向下发时应用可能还在写；抓屏断言要给足命令间隔，且 VGA 旧行残留会让上一条 permission denied 污染下一条断言，读取状态行前先 clear 或只取最后一条 jobs/结果行。
CatOS[易错点] 探针驱动后台任务时，任务寿命必须覆盖全部断言窗口：`daemon memlog 6 500` 3 秒自退，dm-3 的 log 检查在 ~4.5s 必拿「no such task」；`spinner 4 &` 2 秒跑完，fg 到达时已被回收报「fg: no such job」。短寿命自退型任务一律换成 ≥ 断言总耗时的参数（memlog 60x500 / spinner 20），或默认无限循环由探针显式 kill。
CatOS[易错点] 隔离实验 stash/checkout 会把未提交的配套改动一并还原：v2.52 期间为 jobctl 放宽的 spinner 上限 8→60→200（未提交），v2.54 隔离验证后 spinner 回到 60，`spinner 180` 被钳到默认 8 tick 约 1-2 秒退出，jobctl 的 jobs-lists-running 随机假失败。从工作树复跑针对性套件前，先核对探针依赖的未提交源码还在不在（git diff 具体文件），而不是只看 HEAD 提交了什么。
CatOS[易错点] TTF 复合字形变换标志位：0x0100 是 WE_HAVE_INSTRUCTIONS（指令块在所有组件之后），不是 TWO_BY_TWO；三个缩放标志正确值是 WE_HAVE_A_SCALE=0x0008、WE_HAVE_AN_X_AND_Y_SCALE=0x0040、WE_HAVE_A_TWO_BY_TWO=0x0080。标志对错位会把后续组件字段当成 2×2 矩阵读，轮廓点整体错位且不报错。
CatOS[易错点] TTF 复合组件无 CF_WORDS(0x0001) 标志时偏移参数是两个字节（rd8×2，游标推进 2），不是 rd16×2 推进 4；再按 CF_XY_VALUES(0x0002) 决定有符号/无符号，缺该标志属点匹配参数（本批解析层直接拒绝）。字节分支多读 2 字节会让后面所有组件的 gid/变换字段连环错读。
CatOS[易错点] 多分支合并前先把已跟踪的构建产物 stash/移出工作树：带 .ce/.catpkg 本地改动的工作树 `merge --ff-only` 会被"本地修改会被覆盖"挡住；产物噪声不进 docs-only commit，打包时统一排除 */obj/*、*.ppm、*.bin、*/night/* 等目录。
CatOS[易错点] TSC 超时计算的单位倍率：cpu_mhz 是 cycles/1e6 秒（MHz），N 秒超时应读 `mhz*1000000u*N`；写成 `mhz*1000*N` 会把 N 秒缩成 N/1000 秒（30 s → 30 ms），进蓝屏立即判超时退出，按键分支根本走不到。定位手段：BSS 调试字在 enter/drawn/key/timeout 各阶段写不同值，monitor `xp` 读回即可看出落在哪条分支；校准值还要加 `mhz>0 && mhz<10000` 闸门防 bogus 除数。
CatOS[易错点] 崩溃屏不要直写 0xB8000 帧缓冲再混前端写：前端 `render_flush()` 在写入 wide CJK 后若挂 `pending_wide` 会调 `redraw_all()`，`be->clear()` + 按 RAM grid 逐格重刷 VRAM；直写内容不在 grid 里，旧网格（prompt/空格）会把直写行整行盖掉，只有经过前端写的行幸存。紧急屏一律走纯前端路径：`vga_set_color` → `vga_clear()` → 全部 `vga_write_at`/`vga_write_at_utf8`，grid 与 VRAM 自洽。
CatOS[易错点] 单行 ASCII 写接口不要喂 UTF-8 CJK 字节：`vga_write_at` 按单字节单格铺，中文三字节被拆成三个半宽格，后续 ASCII 被错位移到行右半截。含中文的行必须走 `vga_write_at_utf8`；纯英文注记行就不要夹带汉字，两段调用分开（前缀 ASCII 定位 + 后缀 utf8）。
