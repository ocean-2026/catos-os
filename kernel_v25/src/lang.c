/* lang.c -- v2.21 user-visible language setting (T50).
 *
 * /etc/lang contains exactly "en\n" or "zh\n".  Anything else (missing
 * file, no FAT32 volume, garbage) means English -- the documented default.
 * We deliberately do NOT create /etc/lang on boot: a read-only or absent
 * volume must not turn into a write attempt, and `lang zh` creates it on
 * demand via fs_write_text(). */

#include "lang.h"
#include "fs.h"

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
    "log: no such task\n"
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
    "log：找不到该任务\n"
};

const char *tr(int id) {
    if (id < 0 || id >= L_COUNT) return "";
    return g_lang == LANG_ZH ? tbl_zh[id] : tbl_en[id];
}

int lang_get(void) { return g_lang; }

const char *lang_name(void) {
    return g_lang == LANG_ZH ? "zh" : "en";
}

void lang_init(void) {
    g_lang = LANG_EN;
    if (!fs_fs_ready()) return;
    char buf[16];
    int n = fs_read_text("/etc/lang", buf, (int)sizeof(buf));
    if (n <= 0) return;
    buf[n] = 0;
    /* tolerate "zh", "zh\n", " zh\n" -- the file is user editable */
    int i = 0;
    while (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r') i++;
    if (buf[i] == 'z' && buf[i + 1] == 'h') g_lang = LANG_ZH;
}

int lang_set(int l) {
    if (l != LANG_EN && l != LANG_ZH) return -1;
    g_lang = l;
    if (!fs_fs_ready()) return -2;
    const char *v = (l == LANG_ZH) ? "zh\n" : "en\n";
    if (fs_write_text("/etc/lang", v, 3) != 0) return -2;
    return 0;
}
