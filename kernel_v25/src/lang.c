/* lang.c -- v2.21 user-visible language setting (T50).
 *
 * /etc/lang contains exactly "en\n" or "zh\n".  Anything else (missing
 * file, no FAT32 volume, garbage) means English -- the documented default.
 * We deliberately do NOT create /etc/lang on boot: a read-only or absent
 * volume must not turn into a write attempt, and `lang zh` creates it on
 * demand via fs_write_text(). */

#include "lang.h"
#include "fs.h"
#include "reg.h"

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
    const char *v = reg_get("system", "lang");
    if (v && *v) { g_lang = lang_parse(v); return; }
    /* legacy fallback: adopt /etc/lang */
    char buf[16];
    int n = fs_read_text("/etc/lang", buf, (int)sizeof(buf));
    if (n <= 0) return;
    buf[n] = 0;
    g_lang = lang_parse(buf);
    reg_set_now("system", "lang", g_lang == LANG_ZH ? "zh" : "en");
}

int lang_set(int l) {
    if (l != LANG_EN && l != LANG_ZH) return -1;
    g_lang = l;
    if (!fs_fs_ready()) return -2;
    reg_set_now("system", "lang", l == LANG_ZH ? "zh" : "en");
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
