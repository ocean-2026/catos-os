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
