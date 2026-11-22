/* lang.c -- v2.21 user-visible language setting (T50).
 *
 * /etc/lang contains exactly "en\n" or "zh\n".  Anything else (missing
 * file, no FAT32 volume, garbage) means English -- the documented default.
 * We deliberately do NOT create /etc/lang on boot: a read-only or absent
 * volume must not turn into a write attempt, and `lang zh` creates it on
 * demand via fs_write_text(). */

#include "lang.h"
#include "fs.h"
#include "hive.h"

static int g_lang = LANG_EN;

static const char *const tbl_en[L_COUNT] = {
    "\n[!] No FAT32 volume on IDE drive.  Type 'format' first.\n",
    "Format FAT32 volume?  This erases ALL files.\n",
    "Aborted.\n",
    "FAT32 volume formatted.\n",
    "error: file not found\n",
    "error: permission denied\n",
    "error: bad arguments\n",
    "net: NIC is down\n",
    "snd: no SB16 detected (add -device sb16,audiodev=snd0)\n",
    "no Bochs-VBE or VMware-SVGA framebuffer found\n",
    "Switch back to text mode? Press Y to confirm: ",
    "usage: lang [en|zh]\n",
    "language set to ",
    "warning: could not save /etc/lang (setting is session-only)\n",
    "Password: ",
    "passwd: passwords differ\n",
    "no background jobs\n",
    "log: no such task\n",
    "warning: could not save /etc/env (setting is session-only)\n",
    "display: running ",          /* v2.60: followed by mode name */
    "^C: interrupting foreground task\n",  /* v2.60: ^C during fg task */
    "theme set to %s",            /* v2.61: theme applied confirmation */
};

static const char *const tbl_zh[L_COUNT] = {
    "\n[!] IDE 磁盘上没有 FAT32 卷，请先执行 format。\n",
    "要格式化 FAT32 卷吗？这会删除全部文件。\n",
    "已取消。\n",
    "FAT32 卷格式化完成。\n",
    "错误：找不到文件\n",
    "错误：权限不足\n",
    "错误：参数不正确\n",
    "网络：网卡未就绪\n",
    "声音：未检测到 SB16（可加 -device sb16,audiodev=snd0）\n",
    "未找到 Bochs-VBE 或 VMware-SVGA 帧缓冲\n",
    "返回文本模式？按 Y 确认：",
    "用法：lang [en|zh]\n",
    "语言已设为 ",
    "警告：无法保存 /etc/lang（仅本次会话生效）\n",
    "密码：",
    "passwd：两次密码不一致\n",
    "当前没有后台任务\n",
    "log：找不到该任务\n",
    "警告：无法保存 /etc/env（仅本次会话生效）\n",
    "显示：运行于 ",             /* v2.60: 后接模式名 */
    "^C：中断前台任务\n",        /* v2.60: 前台任务中按 Ctrl+C */
    "主题已设置为 %s"           /* v2.61: 主题切换确认 */
};

const char *tr(int id) {
    if (id < 0 || id >= L_COUNT) return "";
    return g_lang == LANG_ZH ? tbl_zh[id] : tbl_en[id];
}

int lang_get(void) { return g_lang; }

const char *lang_name(void) {
    return g_lang == LANG_ZH ? "zh" : "en";
}

static int lang_parse(const char *v) {
    if (!v) return LANG_EN;
    int i = 0;
    while (v[i] == ' ' || v[i] == '\t' || v[i] == '\r') i++;
    return (v[i] == 'z' && v[i + 1] == 'h') ? LANG_ZH : LANG_EN;
}

void lang_init(void) {
    g_lang = LANG_EN;
    if (!fs_fs_ready()) return;
    /* v2.62: registry first. */
    const char *v = hive_sz("HKLM\\System\\Language", "");
    if (v && *v) { g_lang = lang_parse(v); return; }
    /* legacy fallback: adopt /etc/lang */
    char buf[16];
    int n = fs_read_text("/etc/lang", buf, (int)sizeof(buf));
    if (n <= 0) return;
    buf[n] = 0;
    g_lang = lang_parse(buf);
    hive_set_sz("HKLM\\System\\Language", g_lang == LANG_ZH ? "zh" : "en");
}

int lang_set(int l) {
    if (l != LANG_EN && l != LANG_ZH) return -1;
    g_lang = l;
    if (!fs_fs_ready()) return -2;
    hive_set_sz("HKLM\\System\\Language", l == LANG_ZH ? "zh" : "en");
    /* legacy mirror */
    const char *v = (l == LANG_ZH) ? "zh\n" : "en\n";
    if (fs_write_text("/etc/lang", v, 3) != 0) return -2;
    return 0;
}

/* ---- v2.62: help-text localisation ------------------------------------
 * Only the human-readable column (descriptions, headings, notes) is
 * translated; command names/arguments stay English so the scripting
 * surface keeps a stable spelling.  Unmapped strings fall back to the
 * original English, so this table is safe to extend incrementally. */
static const struct { const char *en, *zh; } help_zh[] = {
    /* headings */
    {"fun (NIGHT-P3 Linux-style easter eggs)", "趣味彩蛋（NIGHT-P3 Linux 风格）"},
    {"display (front-end / back-ends)", "显示（前端 / 后端）"},
    {"Chinese (GB2312 display + pinyin input)", "中文（GB2312 显示 + 拼音输入）"},
    {"processes (ring3 tasks, preemptive)", "进程（ring3 任务，抢占式）"},
    {"users & permissions (v2.13, initial user: root)", "用户与权限（初始用户 root）"},
    {"environment & registry (v2.62, persisted in /etc/reg)", "环境与注册表（持久化于 /etc/reg）"},
    {"list registry sections, or a section's keys", "列出注册表节，或某节的键"},
    {"read a registry value", "读取注册表键值"},
    {"set a registry value (admin)", "设置注册表键值（管理员）"},
    {"delete a registry key (admin)", "删除注册表键（管理员）"},
    {"audio (Sound Blaster 16, ISA)", "音频（Sound Blaster 16，ISA）"},
    {"network (PCnet)", "网络（PCnet）"},
    {"filesystem (FAT32 on IDE, persistent)", "文件系统（IDE 上的 FAT32，持久化）"},
    {"registry (v2.62, file-backed /etc/reg)", "注册表（v2.62，文件式 /etc/reg）"},
    /* notes */
    {"Up/Down history | Left/Right/Home/End edit the line", "上/下 历史命令 | 左/右/Home/End 编辑行"},
    {"Ctrl+Up/Down   scroll back console output (any key returns)", "Ctrl+上/下  回卷控制台输出（任意键返回）"},
    {"after 'did you mean:', press y once to run that suggestion", "出现“你是否想输入”提示后，按一次 y 执行建议命令"},
    {"Bare NAME is looked up in cwd then /bin (PATH); 'run FILE' uses", "裸名称先在当前目录再 /bin（PATH）中查找；“run 文件”使用"},
    {"the path verbatim with NO lookup (a bare NAME there is cwd-only).", "字面路径，不查找（其中的裸名称仅指当前目录）。"},
    {"(arrows move, ^O save, ^X exit, ^K del line, ^U clear)", "（方向键移动，^O 保存，^X 退出，^K 删行，^U 清空）"},
    /* common descriptions */
    {"show this message", "显示本帮助"},
    {"list all bound key bindings by group", "按组列出所有按键绑定"},
    {"clear the screen", "清屏"},
    {"show/set UI language (default en, /etc/lang)", "查看/设置界面语言（默认 en）"},
    {"print text back (-n no newline, -c COLOR)", "回显文本（-n 不换行，-c 颜色）"},
    {"clear the screen (alias of clear)", "清屏（clear 的别名）"},
    {"system info banner", "系统信息横幅"},
    {"re-run /etc/autostart scripts", "重新运行 /etc/autostart 脚本"},
    {"print OS name", "打印操作系统名称"},
    {"show RTC date/time", "显示 RTC 日期/时间"},
    {"show RAM size (multiboot)", "显示内存大小（multiboot）"},
    {"set foreground color (0..255; text mode maps to 16)", "设置前景色（0..255；文本模式映射到 16）"},
    {"list tasks (tid, uid, ring, state)", "列出任务（tid、uid、ring、状态）"},
    {"terminate a task", "终止任务"},
    {"current session user", "当前会话用户"},
    {"list accounts (/etc/passwd)", "列出账户"},
    {"show/set post-crash action: reboot|halt (/etc/panicmode)", "查看/设置崩溃后动作：reboot|halt"},
    {"switch back to text mode (needs physical Y confirmation)", "切回文本模式（需物理按 Y 确认）"},
    {"apply dark or light token palette (v2.61, requires VBE/gfx24)", "应用暗色/亮色配色（需 VBE/gfx24）"},
    {"toggle pinyin IME (works in shell and edit)", "切换拼音输入法（shell 与编辑器通用）"},
    {"list a directory", "列出目录"},
    {"change directory (use .. or /)", "切换目录（用 .. 或 /）"},
    {"create a directory", "创建目录"},
    {"create an empty file", "创建空文件"},
    {"print a file (text, up to 64 KiB)", "打印文件（文本，最大 64 KiB）"},
    {"delete a file", "删除文件"},
    {"remove an empty directory", "删除空目录"},
    {"copy a file", "复制文件"},
    {"move a file (copy + delete)", "移动文件（复制 + 删除）"},
    {"seconds since boot + task count", "启动以来的秒数 + 任务数"},
    {"list background jobs ('run X &') with pending log", "列出后台任务及待读日志"},
    {"nano-style full-screen editor", "nano 风格全屏编辑器"},
    {"get / set a registry value (v2.62)", "查看/设置注册表键值"},
    {"list all registry sections / a section's keys", "列出所有注册表节 / 某节的键"},
    {"alias of ls (all options supported)", "ls 的别名（支持全部选项）"},
    {"create/edit a file (new file is created on open)", "创建/编辑文件（打开不存在的文件时自动创建）"},
    {"print a file (was cat)", "打印文件（原 cat）"},
    {"first N / last N lines (was head/tail)", "前 N 行 / 后 N 行（原 head/tail）"},
    {"recursive delete (-f suppresses errors)", "递归删除（-f 忽略错误）"},
    {"show PATH; drives bare-NAME lookup (default /bin)", "显示 PATH；裸名称按 PATH 查找（默认 /bin）"},
    {"run builtin or CE program as root (uid 0)", "以 root（uid 0）运行内置命令或 CE 程序"},
    {"Shift / F2", "Shift / F2"},
    {"toggle pinyin IME; 1-9 pick, Up/Down select, Space confirm", "切换拼音输入法；1-9 选第 N 个，上下键选择，空格确认"},
    {"nano-style full-screen editor; -c/-c N/-c -N prints", "nano 风格全屏编辑器；-c/-c N/-c -N 为打印模式"},
    {"gfx [MODE|CMD]", "gfx [模式|命令]"},
    {"edit", "编辑"},
    {"scroll", "回卷"},
    {"history", "历史"},
    {"IME", "输入法"},
    {"console", "控制台"},
    {"file", "文件"},
    {"move cursor left / right (UTF-8 aware)", "左右移动光标（识别 UTF-8）"},
    {"page IME candidates while IME active", "输入法开启时翻页候选词"},
    {"jump to line start / end", "跳到行首 / 行尾"},
    {"delete char before cursor", "删除光标前的字符"},
    {"delete char at cursor", "删除光标处的字符"},
    {"move one word left / right", "左右移动一个单词"},
    {"page up / down one editor screen", "编辑器中上下翻一屏"},
    {"unbound on the command line", "命令行中未绑定"},
    {"scroll back/forward one full page per press", "每次向前/向后回卷一整页"},
    {"console / command line", "控制台 / 命令行"},
    {"previous / next command in history", "上一条 / 下一条历史命令"},
    {"toggle pinyin IME", "切换拼音输入法"},
    {"pick candidate by number", "按数字选择候选词"},
    {"pick the first candidate", "选择第一个候选词"},
    {"candidate page up / down", "候选词翻页"},
    {"arrows page too", "方向键也可翻页"},
    {"cancel composition, clear pinyin string", "取消输入，清空拼音串"},
    {"commit the raw pinyin string", "直接提交原始拼音"},
    {"cancel input line / send SIGINT to fg task", "取消当前输入 / 向前台任务发 SIGINT"},
    {"suspend fg task to background (SIGTSTP)", "把前台任务挂到后台（SIGTSTP）"},
    {"complete command / path", "补全命令 / 路径"},
    {"save file", "保存文件"},
    {"exit editor; ask on modified / discard", "退出编辑器；有改动时询问"},
    {"Ctrl+Y discards", "Ctrl+Y 放弃修改"},
    {"delete line / clear all / help", "删除行 / 清空 / 帮助"},
    {"command line / editor / IME", "命令行 / 编辑器 / 输入法"},
    {"command line / editor", "命令行 / 编辑器"},
    {"command line", "命令行"},
    {"editor", "编辑器"},
    {"IME active", "输入法开启时"},
    {"-", "—"},
    /* v2.75: help/keys 全量汉化（63 条 help + 1 条 keys），与 shell.c 原文逐字对齐 */
    {"raw ES1370/ES1371 register readback", "读取 ES1370/ES1371 寄存器原始值"},
    {"paint the whole frame + 4 corner marks (coverage probe)", "整屏填色 + 四角标记（覆盖率探针）"},
    {"read-only dump of all ES137x registers 0x00..0x3C", "只读转储 ES137x 全部寄存器 0x00..0x3C"},
    {"ASCII cow (or Tux) says MSG", "ASCII 奶牛（或 Tux）说出 MSG"},
    {"steam train drives by (CHOO CHOO)", "蒸汽火车驶过（呜——）"},
    {"print a random fortune", "输出一条随机箴言"},
    {"month calendar, or full year with YEAR", "当月日历；带 YEAR 则输出全年"},
    {"repeat TEXT until Ctrl+C", "重复输出 TEXT 直到 Ctrl+C"},
    {"display mode switcher + diagnostics:", "显示模式切换 + 诊断："},
    {"bold+italic+underline / style fg #RRGGBB|canvas / style bg surfaceRaised", "粗体+斜体+下划线 / style fg #RRGGBB|canvas / style bg surfaceRaised"},
    {"raw VGA/VBE register diagnostic", "VGA/VBE 寄存器原始值诊断"},
    {"built-in UTF-8 Chinese + truecolour demo", "内置 UTF-8 中文 + 真彩演示"},
    {"warm reboot the machine", "热重启机器"},
    {"halt the CPU (QEMU: close window)", "挂起 CPU（QEMU 中请关闭窗口）"},
    {"power off via ACPI PM1_CNT (root/CAP_SYS; honest reboot fallback)", "通过 ACPI PM1_CNT 关机（需 root/CAP_SYS；失败则如实回退为重启）"},
    {"render the full-screen crash report (--no-reboot for test)", "渲染全屏崩溃报告（测试用 --no-reboot）"},
    {"execute a CE app (in background with &)", "执行 CE 程序（加 & 则后台运行）"},
    {"run a program as root (uid 0)", "以 root（uid 0）运行程序"},
    {"uid/gid/capabilities of a user", "用户的 uid/gid/能力集"},
    {"capability bit legend + session caps", "能力位图例 + 当前会话能力"},
    {"switch session user ('su' = alias)", "切换会话用户（su 为别名）"},
    {"add user (CAP_USERADM)", "添加用户（需 CAP_USERADM）"},
    {"create group / append member (CAP_USERADM)", "创建组 / 追加成员（需 CAP_USERADM）"},
    {"detach a service as a background daemon", "将服务分离为后台守护进程"},
    {"list groups and members (v2.53 /etc/group)", "列出组及成员（/etc/group）"},
    {"manage accounts / caps", "管理账户 / 能力"},
    {"persistent /etc/acl", "持久化到 /etc/acl"},
    {"live task monitor (CPU% per task, any key quits)", "实时任务监视器（各任务 CPU%，任意键退出）"},
    {"show a background job's captured output", "查看后台任务捕获的输出"},
    {"follow that job until it exits", "持续跟踪该任务直到退出"},
    {"resume a stopped/background job in foreground", "将停止/后台任务转入前台继续运行"},
    {"resume a stopped job in background", "将停止的任务转入后台继续运行"},
    {"suspend a running job (SIGTSTP; same as Ctrl+Z)", "挂起运行中的任务（SIGTSTP，等同 Ctrl+Z）"},
    {"set variable (empty V deletes); env lists all", "设置变量（V 为空则删除）；env 列出全部"},
    {"delete variable; $K / ${K} / $? expand in typed lines", "删除变量；键入行中 $K / ${K} / $? 会展开"},
    {"full hardware dump (CPU/mem/disk/NIC/PCI)", "完整硬件信息（CPU/内存/磁盘/网卡/PCI）"},
    {"audio info: -h -v -l -p -c -d N -w -j -s -q -t (long forms too)", "音频信息：-h -v -l -p -c -d N -w -j -s -q -t（也支持长选项）"},
    {"play a tone (default 440 Hz / 800 ms)", "播放测试音（默认 440 Hz / 800 ms）"},
    {"HTTP GET (http://IP[:port]/path)", "HTTP GET（http://IP[:port]/path）"},
    {"ICMP echo (3 tries)", "ICMP 回显（3 次）"},
    {"resolve a hostname", "解析主机名"},
    {"send one UDP datagram and wait for a reply", "发送一个 UDP 数据报并等待回复"},
    {"dump NIC/driver state", "转储网卡/驱动状态"},
    {"list: -l long, -t/-lt tree, --color-scale, --blocks=COLS", "列出：-l 详细、-t/-lt 树形、--color-scale、--blocks=COLS"},
    {"remove an empty directory (was rmdir)", "删除空目录（原 rmdir）"},
    {"package manager (/pkg + remote HTTP)", "包管理器（/pkg + 远程 HTTP）"},
    {"set/show remote package repository base URL", "设置/查看远程软件仓库根 URL"},
    {"download <repo>/PKGS.TXT into local index", "下载 <repo>/PKGS.TXT 到本地索引"},
    {"download + verify + install a signed .catpkg", "下载 + 校验 + 安装签名的 .catpkg"},
    {"print first N lines (default 10)", "输出前 N 行（默认 10）"},
    {"print last N lines (default 10)", "输出后 N 行（默认 10）"},
    {"line/word/byte count", "行/词/字节计数"},
    {"directory tree (alias: ls -t; -lt adds sizes)", "目录树（ls -t 的别名；-lt 附带大小）"},
    {"erase the FAT32 volume (y/n confirm)", "擦除 FAT32 卷（需 y/n 确认）"},
    {"check FAT mirror + scan dir tree; --purge wipes bad entries", "检查 FAT 镜像 + 扫描目录树；--purge 清除坏项"},
    {"list the mount table", "列出挂载表"},
    {"bind a FAT32 partition to an empty dir", "把 FAT32 分区绑定到空目录"},
    {"detach the partition bound to DIR", "卸载绑定到 DIR 的分区"},
    {"per-mountpoint volume usage", "各挂载点的卷使用情况"},
    {"show the last fatal fault (vector, CR2, PF bits)", "显示最近一次致命错误（向量、CR2、缺页标志位）"},
    {"show the saved panic record (crashlog --full for raw)", "显示保存的崩溃记录（crashlog --full 查看原始内容）"},
    {"audio (SB16 ISA / ES1370-1371 PCI, POSIX options)", "音频（SB16 ISA / ES1370-1371 PCI，POSIX 选项）"},
    {"CatOS v0.3 (five-pillar kernel) - commands:", "CatOS v0.3（五柱内核）—— 命令列表："},
    {"deletes pinyin letter while IME active", "输入法开启时删除拼音字符"},
    {"digital rain over the whole screen (any key quits)", "全屏数字雨（任意键退出）"},
    {"ASCII steam train rolls across the console (any key quits)", "ASCII 蒸汽火车横穿控制台（任意键退出）"},
};

#define HELP_ZH_COUNT ((int)(sizeof(help_zh)/sizeof(help_zh[0])))

static int help_zh_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

const char *trhelp(const char *en) {
    if (g_lang != LANG_ZH) return en;
    for (int i = 0; i < HELP_ZH_COUNT; i++)
        if (help_zh[i].en && en && 0 == help_zh_cmp(help_zh[i].en, en))
            return help_zh[i].zh;
    return en;
}

const char *trs(const char *en, const char *zh) {
    return (g_lang == LANG_ZH && zh) ? zh : en;
}
