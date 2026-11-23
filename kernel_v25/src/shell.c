#include "shell.h"
#include "vga.h"
#include "render.h"      /* display front-end: mode policy + text styles */
#include "keyboard.h"
#include "ime.h"
#include "usb.h"
#include "rtc.h"
#include "io.h"
#include "fs.h"
#include "ata.h"
#include "fat.h"
#include "diag.h"       /* faultinfo: last-fault record */
#include "ce.h"
#include "net.h"
#include "task.h"
#include "hw.h"
#include "snd.h"        /* v2.11 SB16 info / self-test commands */
#include "gfx24.h"      /* v2.12 24bpp truecolour drawing surface */
#include "svga.h"       /* v2.20 VMware SVGA II framebuffer driver */
#include "e1000.h"       /* v2.21 #55 e1000 NIC state for netdiag */
#include "es1371.h"      /* v2.21 #56 ES1370/1371 audio state for sndinfo */
#include "lang.h"       /* v2.21 T50: en/zh user-visible messages      */
#include "bsod.h"       /* v2.59 crash screen: bsod/blue + panicmode      */
#include "clip.h"       /* v2.57 kernel clipboard */
#include "mouse.h"      /* v2.57 PS/2 mouse state diagnostics */
#include "keys.h"       /* centralized key-binding legend (keys command)  */
#include "pager.h"
#include "crypto.h"   /* v2.76: SHA-256 for the .catpkg digest */      /* v2.59 generic console pager                    */
#include "hive.h"       /* v2.67 binary tree registry (/etc/hive)          */
#include "palette.h"    /* v2.67 semantic colour roles from the hive        */
#include "power.h"      /* v2.67 ACPI power-off (no APM INT 15h)            */
#include "hivecmd.h"
#include "posix.h"
#include "sec.h"
#include "dash.h"    /* v2.67 `hive` command front-end                   */
#include "ssh.h"     /* v2.71 SSH-2 server session serving               */

void spawn_and_maybe_wait(const char *path, int argc, char **argv, int bg, int uid);

/* v2.52: tid of the newest '&' background job -- fg/bg/stop fall back to
 * it when called without an argument. */
static int g_last_bg_tid;
/* v2.60: TID of the currently running foreground task (set during wait).
 * Ctrl+C sends SIGINT, Ctrl+Z sends SIGTSTP. Cleared when the task exits
 * or is terminated. */
static int g_fg_tid = -1;

/* ---------- tiny libc ---------- */
static int strcmp_(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int slen_(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

/* v2.61: limited-length string compare for gfx subcommand dispatch.
 * v2.68.2: the old loop compared the (n+1)-th byte after all n matched,
 * so `strncmp_("perm,size", "perm", 4)` returned ','-'\0' instead of 0.
 * That silently broke `perm_first` in the ls long-format builder (and
 * every other exact-prefix dispatch).  Return 0 when all n bytes match. */
static int strncmp_(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;              /* both strings ended together */
    }
    return 0;                               /* all n bytes matched */
}
static int session_is_admin(void);   /* v2.62: fwd decl for cmd_reg gate */

/* forward decls: the shared command dispatcher and the boot autostart
 * runner (/etc/autostart scripts, v2.17) */
static void shell_exec(char *cmd, char *arg);
/* v2.68: edit -c uses the former standalone printers internally */
static void cmd_cat(const char *arg);
static void cmd_head(char *arg);
static void cmd_tail(char *arg);
static void env_expand(char *line, int cap);   /* v2.71: fwd decl for the sshd bridge */
static int  atoi_(const char *s);
static int  ls_u2a(uint32_t v, char *o);
static void boot_autostart(void);
static int  parse_redir(char *arg, char **out_redir, int *append, char **pipe_to);
static void skip_sp(const char **pp);   /* v2.61: advance past spaces                   */
/* v2.61: fwd-decl gfx sub-commands referenced from cmd_gfx above */
static void cmd_svgainfo(void);
static void cmd_size(const char *arg);
static void cmd_display(void);
/* v2.65: POSIX-style sndinfo (forward decl; full body after strcat_b) */
static void cmd_sndinfo(char *arg);
/* v2.65: exit status of the most recently completed command, readable as
 * $? in typed lines and /etc/autostart scripts. */
static int g_last_rc;
static uint32_t g_caps;      /* v2.67: fwd decl for the hive command gate */

/* P1: set while /etc/autostart scripts run their commands.  Interactive
 * prompts (e.g. the PE->CE conversion offer in `run`) must default to
 * refusal in this mode -- a script cannot answer a keyboard question. */
static int      g_script_mode = 0;

static int g_uid;         /* session uid/gid (defined with the session code) */
static int g_gid;         /* tentative decls: the real defs sit below */
static int redir_abspath(const char *name, char *out, int outlen);

/* v2.59: pager non-interactive latch.  Set around the dispatch of a line
 * that is redirected to a file, piped, backgrounded, or running inside a
 * script / non-tty session; the pager then streams the full output
 * through without ever waiting for a key. */
static int shell_line_noninteractive(char *arg,
                                     char **out_redir, int *append,
                                     char **pipe_to, int *out_bg) {
    parse_redir(arg, out_redir, append, pipe_to);
    int bg = 0;
    /* trailing '&' background marker (mirrors the dispatcher's own check) */
    char *tail = arg;
    while (*tail) tail++;
    while (tail > arg && tail[-1] == ' ') tail--;
    if (tail > arg && tail[-1] == '&' &&
        (tail - arg < 2 || tail[-2] == ' ')) bg = 1;
    if (out_bg) *out_bg = bg;
    return (*out_redir || *pipe_to || bg || g_script_mode) ? 1 : 0;
}

/* v2.59: ring0 builtin stdout redirection tee.
 *
 * Ring3 `.ce` programs already wire `>`/`>>` through the per-task
 * `out_file` field consumed in SYS_WRITE (task.c).  The ring0 builtins
 * (help/ls/echo/...) write straight through vga_write() and had no capture
 * path, so `help > file` dropped the output on the floor.  This small tee
 * mirrors every character the builtin prints into a per-command static
 * buffer, and on command completion flushes it to the redirect target with
 * the session's own write permission check (same gate as #167).  Pipes
 * and `&` backgrounding stay on the existing task ring/pipe path; only a
 * plain file redirect is mirrored here.  Buffer is capped so a runaway
 * builtin cannot grow it without bound; overflow truncates the tail. */
#define SH_REDIR_BUF 8192
static char  g_redir_buf[SH_REDIR_BUF];
static uint32_t g_redir_len;
static int   g_redir_active;
static char  g_redir_path[FS_PATH_MAX];
static int   g_redir_append;

static void redir_tee_char(char c) {
    if (!g_redir_active) return;
    if (g_redir_len < SH_REDIR_BUF - 1) g_redir_buf[g_redir_len++] = c;
}

/* Install the builtin redirect for this command line.  Returns 1 when a
 * plain file redirect was armed (the tee then captures builtin output),
 * 0 otherwise (pipe / background / none). */
static int redir_arm(char *out_redir, int append, char *pipe_to) {
    g_redir_active = 0;
    g_redir_len = 0; g_redir_buf[0] = 0;
    g_redir_path[0] = 0; g_redir_append = append;
    if (!out_redir || !*out_redir || pipe_to) return 0;

    char ab[FS_PATH_MAX];
    if (redir_abspath(out_redir, ab, sizeof(ab)) != 0) {
        vga_write(trs("run: bad redirect path\n", "run: 重定向路径错误\n"));
        return 0;
    }
    /* #167 parity: ring0 pre-write must check the target with the session
     * identity up front, instead of letting a non-root session truncate or
     * create a protected file. */
    if (fs_perm_check(ab, g_uid, g_gid, PERM_WRITE) != 0) {
        vga_write(trs("run: permission denied: '", "run: 权限不足: '")); vga_write(ab); vga_write("'\n");
        return 0;
    }
    int i;
    for (i = 0; ab[i] && i < (int)sizeof(g_redir_path) - 1; i++)
        g_redir_path[i] = ab[i];
    g_redir_path[i] = 0;
    g_redir_active = 1;
    pager_suppress_abort(1);   /* v2.76: a redirected listing must not truncate */
    vga_set_tee(redir_tee_char);
    /* v2.59: a builtin whose stdout goes to a file must never park on a
     * More bar -- there is no terminal to answer it, and the bar text would
     * otherwise be captured into the file by the tee.  Arm the pager latch
     * here as well as per-line in shell_exec, so nested/wrapped dispatch
     * paths (outer strip + inner re-entry) cannot miss it. */
    pager_set_noninteractive(1);
    return 1;
}

/* Flush the captured builtin output to the redirect target.  `>` truncates
 * once up front, then every later chunk appends (v2.5.1 multi-line rule). */
static void redir_flush(void) {
    if (!g_redir_active) return;
    g_redir_buf[g_redir_len] = 0;
    if (g_redir_len > 0) {
        if (!g_redir_append)
            fs_write_raw_mode(g_redir_path, "", 0, 0);      /* truncate/create */
        fs_write_raw_mode(g_redir_path, g_redir_buf, g_redir_len, 1);
    }
    g_redir_active = 0;
    g_redir_len = 0; g_redir_buf[0] = 0;
    g_redir_path[0] = 0;
    vga_set_tee(0);
    pager_suppress_abort(0);
}

static void itoa(int v, char *out, int width) {
    char tmp[12]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i < width) tmp[i++] = '0';
    if (neg) tmp[i++] = '-';
    int j = 0; while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static void print_int(int v) {
    char b[12]; itoa(v, b, 0); vga_write(b);
}
static void print_hex(uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { vga_write("0"); return; }
    while (v > 0) { b[i++] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    while (i > 0) { char c = b[--i]; vga_putc(c); }
}
static void print_pad(int v, int w) {
    char b[12]; itoa(v, b, w); vga_write(b);
}

/* ---------- multiboot info ---------- */
extern uint32_t multiboot_info_ptr;

typedef struct {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
} multiboot_info_t;

static multiboot_info_t *mbi;

/* ---------- commands ---------- */
/* v2.37 P0-4: fixed two-column help.  Command field is 24 chars wide,
 * description starts in column 26.  Section headers / notes print raw. */
/* v2.59: paged help rows.  Each helper feeds the one line through the
 * shared pager; once the user quits ('q') the rest is dropped and the
 * helpers become no-ops until cmd_help() calls pager_end(). */
/* v2.75: CJK descriptions are two cells per glyph and do not fit the fixed
 * 26-column description field, so a Chinese row would wrap mid-sentence and
 * visually collide with the next row.  Put the command on its own line and
 * the description on an indented line instead -- clean in both directions. */
static int has_cjk(const char *s) {
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return 1;
    return 0;
}

static void help_row(const char *cmd, const char *desc) {
    if (pager_quit()) return;
    desc = trhelp(desc);                  /* v2.62: localise description */
    if (has_cjk(desc)) {
        char l1[96], l2[192];
        int a = 0, b = 0;
        l1[a++] = ' '; l1[a++] = ' ';
        for (const char *p = cmd; *p && a < (int)sizeof(l1) - 1; p++) l1[a++] = *p;
        l1[a] = 0;
        for (int i = 0; i < 6 && b < (int)sizeof(l2) - 1; i++) l2[b++] = ' ';
        for (const char *p = desc; *p && b < (int)sizeof(l2) - 1; p++) l2[b++] = *p;
        l2[b] = 0;
        pager_line(l1);
        pager_line(l2);
        return;
    }
    char line[160];
    line[0] = 0;
    /* command field is 24 chars wide, description starts in column 26 */
    int n = 0;
    for (const char *p = cmd; *p; p++) { if (n < (int)sizeof(line)-1) line[n++] = *p; }
    while (n < 24 && n < (int)sizeof(line)-1) line[n++] = ' ';
    if (n < (int)sizeof(line)-2) { line[n++] = ' '; line[n++] = ' '; }
    for (const char *p = desc; *p && n < (int)sizeof(line)-1; p++) {
        /* Chinese descriptions are UTF-8 and 2 cells wide per glyph, but
         * pager_line prints raw bytes; keep the byte-fit guard. */
        line[n++] = *p;
    }
    line[n] = 0;
    pager_line(line);
}
static void help_head(const char *s) {
    if (pager_quit()) return;
    s = trhelp(s);                        /* v2.62: localise heading */
    con_set_fg_rgb(pal_rgb(PAL_ACCENT));
    char line[128];
    int n = 0; line[n++] = ' '; line[n++] = '-'; line[n++] = ' ';
    for (const char *p = s; *p && n < (int)sizeof(line)-1; p++) line[n++] = *p;
    line[n++] = ' '; line[n++] = '-'; line[n] = 0;
    pager_line(line);
    con_set_fg_rgb(pal_rgb(PAL_FG));
}
static void help_note(const char *s)  { if (pager_quit()) return; s = trhelp(s); char line[128]; line[0] = ' '; int n = 1; for (const char *p = s; *p && n < (int)sizeof(line)-1; p++) line[n++] = *p; line[n] = 0; pager_line(line); }

static void cmd_help(void) {
    con_suppress_cursor(1);                       /* v2.60: batch paint, no flicker */
    pager_begin(1);
    pager_line(trhelp("CatOS v0.3 (five-pillar kernel) - commands:"));  /* v2.75: 经翻译表 */
    help_row("help",        "show this message");
    help_row("keys|shortcuts", "list all bound key bindings by group");
    help_row("clear",       "clear the screen");
    help_row("lang [en|zh]","show/set UI language (default en, /etc/lang)");
    help_note("Up/Down history | Left/Right/Home/End edit the line");
    help_note("Ctrl+Up/Down   scroll back console output (any key returns)");
    help_note("after 'did you mean:', press y once to run that suggestion");
    help_row("echo TEXT",   "print text back (-n no newline, -c COLOR)");
    help_row("cls",         "clear the screen (alias of clear)");
    help_row("about",       "system info banner");
    help_row("sndregs",     "raw ES1370/ES1371 register readback");
    help_row("fprobe",      "paint the whole frame + 4 corner marks (coverage probe)");
    help_row("sndmap",      "read-only dump of all ES137x registers 0x00..0x3C");
    help_row("autostart",   "re-run /etc/autostart scripts");
    help_row("uname",       "print OS name");
    help_row("date",        "show RTC date/time");
    help_row("mem",         "show RAM size (multiboot)");
    help_row("color NUM",   "set foreground color (0..255; text mode maps to 16)");

    help_head("fun (NIGHT-P3 Linux-style easter eggs)");
    help_row("cowsay [-f tux] MSG", "ASCII cow (or Tux) says MSG");
    help_row("sl",          "steam train drives by (CHOO CHOO)");
    help_row("rain",        "digital rain over the whole screen (any key quits)");
    help_row("train",       "ASCII steam train rolls across the console (any key quits)");
    help_row("fortune",     "print a random fortune");
    help_row("cal [YEAR]",  "month calendar, or full year with YEAR");
    help_row("yes [TEXT]",  "repeat TEXT until Ctrl+C");

    help_head("display (front-end / back-ends)");
    help_row("gfx [MODE|CMD]", "display mode switcher + diagnostics:"
             "\n     MODE = gfx | vbe | gfx24\n     CMD  = svgainfo | size"
             "\n          | rgb FG [BG] | palette IDX RRGGBB | display");
    help_row("text",        "switch back to text mode (needs physical Y confirmation)");
    help_row("style SPEC",  "bold+italic+underline / style fg #RRGGBB|canvas / style bg surfaceRaised");
    help_row("theme NAME",  "apply dark or light token palette (v2.61, requires VBE/gfx24)");
    help_row("vdiag",       "raw VGA/VBE register diagnostic");

    help_head("Chinese (GB2312 display + pinyin input)");
    help_row("zhdemo",      "built-in UTF-8 Chinese + truecolour demo");
    help_row("Shift / F2", "toggle pinyin IME; 1-9 pick, Up/Down select, Space confirm");
    help_row("reboot",      "warm reboot the machine");
    help_row("hang",        "halt the CPU (QEMU: close window)");
    help_row("shutdown",    "power off via ACPI PM1_CNT (root/CAP_SYS; honest reboot fallback)");
    help_row("bsod|blue",   "render the full-screen crash report (--no-reboot for test)");
    help_row("panicmode",   "show/set post-crash action: reboot|halt (/etc/panicmode)");

    help_head("processes (ring3 tasks, preemptive)");
    help_row("run FILE.ce [args..] [&]", "execute a CE app (in background with &)");
    help_row("ps",          "list tasks (tid, uid, ring, state)");
    help_row("kill TID",    "terminate a task");
    help_row("sudo CMD",    "run a program as root (uid 0)");

    help_head("users & permissions (v2.13, initial user: root)");
    help_row("whoami",      "current session user");
    help_row("id [USER]",   "uid/gid/capabilities of a user");
    help_row("lsuser",      "list accounts (/etc/passwd)");
    help_row("caps",        "capability bit legend + session caps");
    help_row("login U [PW]","switch session user ('su' = alias)");
    help_row("useradd N [UID [GID [CAPS]]]", "add user (CAP_USERADM)");
    help_row("groupadd G [MEMBER]", "create group / append member (CAP_USERADM)");
    help_row("daemon CMD [args]", "detach a service as a background daemon");
    help_row("groups", "list groups and members (v2.53 /etc/group)");
    help_row("userdel N | passwd N PW | chcaps HEX N", "manage accounts / caps");
    help_row("chmod OCT PATH | chown UID[:GID] PATH", "persistent /etc/acl");
    help_row("uptime",      "seconds since boot + task count");
    help_row("top",         "live task monitor (CPU% per task, any key quits)");
    help_row("jobs",        "list background jobs ('run X &') with pending log");
    help_row("log TID",     "show a background job's captured output");
    help_row("log -f TID",  "follow that job until it exits");
    help_row("fg [%]TID",   "resume a stopped/background job in foreground");
    help_row("bg [%]TID",   "resume a stopped job in background");
    help_row("stop [%]TID",  "suspend a running job (SIGTSTP; same as Ctrl+Z)");

    help_head("environment & registry (v2.62, persisted in /etc/reg)");
    help_row("reg list [SEC]", "list registry sections, or a section's keys");
    help_row("reg get SEC KEY", "read a registry value");
    help_row("reg set SEC KEY VAL", "set a registry value (admin)");
    help_row("reg del SEC KEY", "delete a registry key (admin)");
    help_row("export K=V",  "set variable (empty V deletes); env lists all");
    help_row("unset K",     "delete variable; $K / ${K} / $? expand in typed lines");
    help_row("PATH",        "show PATH; drives bare-NAME lookup (default /bin)");
    help_row("hwinfo",      "full hardware dump (CPU/mem/disk/NIC/PCI)");

    help_head("audio (SB16 ISA / ES1370-1371 PCI, POSIX options)");
    help_row("sndinfo [OPT]", "audio info: -h -v -l -p -c -d N -w -j -s -q -t (long forms too)");
    help_row("sndtest [HZ MS]", "play a tone (default 440 Hz / 800 ms)");

    help_head("network (PCnet)");
    help_row("curl URL",    "HTTP GET (http://IP[:port]/path)");
    help_row("ping IP",     "ICMP echo (3 tries)");
    help_row("dns NAME",    "resolve a hostname");
    help_row("udp IP PORT TEXT", "send one UDP datagram and wait for a reply");
    help_row("netdiag",     "dump NIC/driver state");

    help_note("Bare NAME is looked up in cwd then /bin (PATH); 'run FILE' uses");
    help_note("the path verbatim with NO lookup (a bare NAME there is cwd-only).");

    help_head("filesystem (FAT32 on IDE, persistent)");
    help_row("ls [PATH]",   "list: -l long, -t/-lt tree, --color-scale, --blocks=COLS");
    help_row("dir [PATH]",  "alias of ls (all options supported)");
    help_row("cd [PATH]",   "change directory (use .. or /)");
    help_row("mkdir PATH",  "create a directory");
    help_row("edit FILE",   "create/edit a file (new file is created on open)");
    help_row("edit -c FILE", "print a file (was cat)");
    help_row("edit -c N FILE | edit -c -N FILE", "first N / last N lines (was head/tail)");
    help_row("rm FILE",     "delete a file");
    help_row("rm -d DIR",   "remove an empty directory (was rmdir)");
    help_row("rm -r DIR",   "recursive delete (-f suppresses errors)");
    help_row("cp SRC DST",  "copy a file");
    help_row("mv SRC DST",  "move a file (copy + delete)");
    /* NIGHT-P4: package manager + migrated tools */
    help_row("pkg list|info|install|remove NAME", "package manager (/pkg + remote HTTP)");
    help_row("pkg repo [URL]",     "set/show remote package repository base URL");
    help_row("pkg update",         "download <repo>/PKGS.TXT into local index");
    help_row("pkg fetch NAME|URL", "download + verify + install a signed .catpkg");
    help_row("head [-n N] FILE", "print first N lines (default 10)");
    help_row("tail [-n N] FILE", "print last N lines (default 10)");
    help_row("wc [-l] [-w] [-c] FILE", "line/word/byte count");
    help_row("tree [DIR]",  "directory tree (alias: ls -t; -lt adds sizes)");
    help_row("format",      "erase the FAT32 volume (y/n confirm)");
    help_row("fsck",        "check FAT mirror + scan dir tree; --purge wipes bad entries");
    help_row("mount",       "list the mount table");
    help_row("mount <1..4|hd0pN> DIR", "bind a FAT32 partition to an empty dir");
    help_row("umount DIR",  "detach the partition bound to DIR");
    help_row("df",          "per-mountpoint volume usage");
    help_row("faultinfo",   "show the last fatal fault (vector, CR2, PF bits)");
    help_row("crashlog",    "show the saved panic record (crashlog --full for raw)");
    help_row("edit FILE",   "nano-style full-screen editor; -c/-c N/-c -N prints");
    help_note("(arrows move, ^O save, ^X exit, ^K del line, ^U clear)");
    pager_end();
    con_unsuppress_cursor();                      /* v2.60: re-enable cursor paint */
}

/* v2.58: grouped key-binding legend.  The rows come straight from the
 * centralized keys_desc table (keys_table.c) -- the same codes the
 * keyboard decoder and dispatch branches act on, so adding a binding
 * there shows up here automatically (no second hand-written list). */
static void cmd_keys(void) {
    for (int g = 0; g < KG__COUNT; g++) {
        con_set_fg_rgb(pal_rgb(PAL_ACCENT));
        vga_write("  -- ");
        vga_write(trhelp(keys_group_name((enum keys_group)g)));
        vga_write(" --\n");
        con_set_fg_rgb(pal_rgb(PAL_FG));
        for (int i = 0; i < keys_desc_count; i++) {
            const keys_desc_t *r = &keys_desc[i];
            if (r->group != g) continue;
            vga_write("  ");
            char lab[24];
            int n = 0;
            for (const char *p = r->labels; *p && n < 22; ) lab[n++] = *p++;
            lab[n] = 0;
            vga_write(trhelp(lab));
            while (n < 22) { vga_putc(' '); n++; }
            vga_write("  ");
            vga_write(trhelp(r->desc));
            vga_write("  (");
            vga_write(trhelp(r->scene));
            if (strcmp_(r->limit, "-") != 0) {
                vga_write("; ");
                vga_write(trhelp(r->limit));
            }
            vga_write(")\n");
        }
    }
}

/* ---------- SB16 audio (v2.11) -----------------------------------------
 * The shell runs in ring0 with interrupts enabled, so it can call the
 * driver directly; ring3 programs go through the four v2.11 ABI slots.
 * To get sound out of QEMU >= 7.1 (-soundhw was removed):
 *   -audiodev wav,id=snd0,path=snd.wav
 *   -device sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0
 * ---------------------------------------------------------------------- */
/* ---------- T50: language setting --------------------------------------- */
static void cmd_lang(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("lang: "); vga_write(lang_name());
        vga_write(trs("   (usage: lang [en|zh])\n", "   (用法：lang [en|zh])\n"));
        return;
    }
    int l;
    if (arg[0] == 'e' && arg[1] == 'n' && (arg[2] == 0 || arg[2] == ' ')) l = LANG_EN;
    else if (arg[0] == 'z' && arg[1] == 'h' && (arg[2] == 0 || arg[2] == ' ')) l = LANG_ZH;
    else { vga_write(tr(L_LANG_USAGE)); return; }

    int r = lang_set(l);
    vga_write(tr(L_LANG_SET)); vga_write(lang_name()); vga_write("\n");
    if (r == -2) vga_write(tr(L_LANG_PERSIST_FAIL));
}

/* v2.65: the old zero-argument diagnostic was replaced by the full
 * POSIX-style sndinfo implementation below (after strcat_b). */

/* Read one unsigned decimal from *pp; leave 0 unchanged when absent. */
static uint32_t snd_parse_u32(const char **pp) {
    const char *p = *pp;
    while (*p == ' ') p++;
    uint32_t v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++; any = 1;
    }
    *pp = p;
    return any ? v : 0;
}

static void cmd_sndtest(const char *arg) {
    uint32_t freq = 440u, ms = 800u;
    if (*arg) {
        uint32_t f = snd_parse_u32(&arg);
        uint32_t m = snd_parse_u32(&arg);
        if (f) freq = f;
        if (m) ms = m;
    }
    if (!snd_info(SND_INFO_READY)) {
        vga_write(trs("snd: no SB16 - SNDTEST SKIP\n", "snd: 没有 SB16 —— SNDTEST 跳过\n"));
        return;
    }
    int before = snd_info(SND_INFO_IRQS);
    vga_write("snd: playing "); print_int((int)freq);
    vga_write(" Hz for ");     print_int((int)ms);
    vga_write(" ms\n");

    int rc = snd_tone(freq, ms);
    if (rc < 0) {
        vga_write(trs("snd: play failed rc=", "snd: play 失败 rc=")); print_int(rc);
        vga_write("\nSNDTEST FAIL\n");
        return;
    }
    /* Wait for the DMA-completion IRQ to clear SND_INFO_PLAYING.  `hlt`
     * parks the CPU until the next interrupt; the shell is ring0 with
     * IF=1, so PIT/SB16 interrupts still arrive.  Upper bound 4 s. */
    uint32_t deadline = task_uptime_ms() + 4000u;
    while (snd_info(SND_INFO_PLAYING) && task_uptime_ms() < deadline)
        __asm__ volatile ("hlt");

    int after = snd_info(SND_INFO_IRQS);
    vga_write("snd: irqs="); print_int(after - before);
    vga_write(" busy=");     print_int(snd_info(SND_INFO_PLAYING));
    vga_write("\n");
    /* v2.21 #56: QEMU's ES1370 has no working DAC interrupt (STATUS is
     * read-only there), so completion is a software timeout driven by
     * timer_ticks.  Requiring an IRQ delta would fail an otherwise
     * perfectly played tone.  SB16 keeps the strict IRQ check. */
    int completed = (snd_info(SND_INFO_BACKEND) == 2) ? 1 : (after > before);
    if (completed && !snd_info(SND_INFO_PLAYING))
        vga_write("SNDTEST PASS\n");
    else
        vga_write(trs("SNDTEST FAIL (no DMA completion IRQ)\n", "SNDTEST 失败（没有 DMA 完成中断）\n"));
}

/* ---------- display front-end commands ---------------------------------
 * These commands only talk to render.h.  They never touch a VGA register
 * or a framebuffer, and there is deliberately no pixel-drawing command:
 * graphics mode is a TEXT mode with more colours and styles. */
static int style_name_eq(const char *p, int n, const char *name) {
    int i = 0;
    while (i < n && name[i] && p[i] == name[i]) i++;
    return i == n && name[i] == 0;
}

static void cmd_style(const char *arg) {
    if (!*arg) {
        vga_write(trs("usage: style SPEC   SPEC = decimal mask, 'off', or names\n"
                      "       bold dim italic underline inverse strike hidden\n"
                      "       joined with '+' e.g. style bold+underline\n"
                      "       style fg #RRGGBB|canvas   style bg surfaceRaised\n",
                      "用法：style SPEC   SPEC = 十进制掩码、'off' 或名称\n"
                      "       粗体 暗色 斜体 下划线 反显 删除线 隐藏\n"
                      "       用 '+' 连接，例如 style bold+underline\n"
                      "       style fg #RRGGBB|canvas   style bg surfaceRaised\n"));
        return;
    }
    const char *p = arg;
    /* optional fg/bg specifier before attribute list */
    while (*p == ' ') p++;
    if (strncmp_(p, "fg ", 3) == 0 || strncmp_(p, "fg\t", 3) == 0) {
        uint32_t rgb;
        if (con_parse_color(p + 3, &rgb) == 0) {
            con_set_fg_rgb(rgb);
            vga_write("fg set to 0x"); print_hex(rgb); vga_write("\n");
        } else {
            vga_write(trs("style: bad colour spec (see 'help')\n", "style: 颜色规格错误（见 help）\n"));
        }
        return;
    }
    if (strncmp_(p, "bg ", 3) == 0 || strncmp_(p, "bg\t", 3) == 0) {
        uint32_t rgb;
        if (con_parse_color(p + 3, &rgb) == 0) {
            con_set_bg_rgb(rgb);
            vga_write("bg set to 0x"); print_hex(rgb); vga_write("\n");
        } else {
            vga_write(trs("style: bad colour spec (see 'help')\n", "style: 颜色规格错误（见 help）\n"));
        }
        return;
    }
    uint8_t a = 0;
    if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') { a = (uint8_t)(a * 10 + (*p - '0')); p++; }
    } else {
        while (*p) {
            if (*p == '+' || *p == ' ') { p++; continue; }
            const char *s = p;
            while (*p && *p != '+' && *p != ' ') p++;
            int n = (int)(p - s);
            if      (style_name_eq(s, n, "off"))      a = 0;
            else if (style_name_eq(s, n, "none"))     a = 0;
            else if (style_name_eq(s, n, "bold"))     a |= CON_ATTR_BOLD;
            else if (style_name_eq(s, n, "dim"))      a |= CON_ATTR_DIM;
            else if (style_name_eq(s, n, "italic"))   a |= CON_ATTR_ITALIC;
            else if (style_name_eq(s, n, "underline"))a |= CON_ATTR_UNDERLINE;
            else if (style_name_eq(s, n, "inverse"))  a |= CON_ATTR_INVERSE;
            else if (style_name_eq(s, n, "strike"))   a |= CON_ATTR_STRIKE;
            else if (style_name_eq(s, n, "hidden"))   a |= CON_ATTR_HIDDEN;
            else {
                vga_write(trs("style: unknown token (see 'help')\n", "style: 未知标记（见 help）\n"));
                return;
            }
        }
    }
    con_set_attrs(a);
    vga_write("style mask = 0x");
    print_hex((uint32_t)a);
    vga_write("\n");
}

/* v2.72: `colors` -- the missing runtime half of the registry palette.
 * `reg set HKLM\Software\CatOS\Terminal\Colors\Background "#101418"` used
 * to take effect only after a reboot, and even then the boot code re-applied
 * the named scheme and clobbered it.  This command re-reads the roles from
 * the hive and pushes them onto the live frame in one step.
 *
 *   colors                      reload + apply (no restart needed)
 *   colors save                 persist the hive to disk
 *   colors reset                restore the built-in defaults and apply
 *   colors set <role> <#rgb>    write one role, apply immediately
 *   colors ls                   list the console roles
 */
static void cmd_colors(const char *arg) {
    const char *p = arg;
    while (*p == ' ') p++;

    if (!strncmp_(p, "save", 4)) {
        hive_save();
        vga_write(trs("colors: hive saved -- these colours persist across reboot\n", "colors: hive 已保存——这些颜色重启后仍然生效\n"));
        return;
    }
    if (!strncmp_(p, "reset", 5)) {
        pal_reset();
        con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
        con_repaint_frame();
        vga_write(trs("colors: built-in defaults restored and applied\n", "colors: 已恢复内置默认并应用到当前画面\n"));
        return;
    }
    if (!strncmp_(p, "set", 3)) {
        p += 3;
        while (*p == ' ') p++;
        char role_name[20]; int n = 0;
        while (*p && *p != ' ' && n < 19) role_name[n++] = *p++;
        role_name[n] = 0;
        while (*p == ' ') p++;
        char spec[16]; int m = 0;
        while (*p && *p != ' ' && m < 15) spec[m++] = *p++;
        spec[m] = 0;
        int role = -1;
        for (int i = 0; i < PAL_COUNT; i++)
            if (!strcmp_(role_name, pal_name(i))) { role = i; break; }
        if (role < 0 || !spec[0]) {
            vga_write(trs("usage: colors set <role> <#RRGGBB>   (roles: colors ls)\n", "用法：colors set <角色> <#RRGGBB>（角色名见 colors ls）\n"));
            return;
        }
        if (pal_set(role, spec) != 0) {
            vga_write(trs("colors set: invalid colour (want #RRGGBB or a name)\n", "colors set: 颜色无效（需要 #RRGGBB 或颜色名）\n"));
            return;
        }
        con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
        con_repaint_frame();
        vga_write(trs("colors: ", "colors: ")); vga_write(role_name);
        vga_write(" = "); vga_write(pal_hex(role));
        vga_write(hive_dirty() ? "  (run `colors save` to persist)\n" : "  (saved)\n");
        return;
    }

    int fb = pal_reload();
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
    con_repaint_frame();
    vga_write(trs("colors: reloaded from the hive and applied\n", "colors: 已从 hive 重新读取并应用\n"));
    if (fb) {
        vga_write(trs("colors: ", "colors: "));
        print_int(fb);
        vga_write(trs(" role(s) fell back to defaults (key missing or malformed)\n", " 个角色回退到默认值（键缺失或格式错）\n"));
    }
    for (int i = 0; i < PAL_COUNT; i++) {
        vga_write("  "); vga_write(pal_name(i));
        for (int k = (int)slen_(pal_name(i)); k < 14; k++) vga_write(" ");
        vga_write(pal_hex(i));
        if (i == PAL_BG) vga_write(trs("   <- canvas", "   <- 画布底色"));
        vga_write("\n");
    }
}

/* v2.67: theme = a named colour scheme written into the hive.  Every role
 * the shell paints (prompt, command, argument, error, warning, hint, and
 * the crash screen) is a registry value now; no colour is compiled in. */
static void cmd_theme(const char *arg) {
    const char *p = arg;
    skip_sp(&p);
    if (!*p) {
        vga_write("current theme: "); vga_write(pal_scheme_name()); vga_write("\n");
        vga_write("background "); vga_write(pal_hex(PAL_BG));
        vga_write("  foreground "); vga_write(pal_hex(PAL_FG)); vga_write("\n");
        vga_write(trs("usage: theme dark|light|solarized|mono [--crash]\n", "用法：theme dark|light|solarized|mono [--crash]\n"));
        vga_write("       --crash also rewrites the crash-screen roles\n");
        vga_write("       one role: hive set HKLM\\Software\\CatOS\\Terminal\\Colors\\Error \"#F85149\"\n");
        return;
    }
    char name[16];
    int n = 0;
    while (*p && *p != ' ' && n < 15) name[n++] = *p++;
    name[n] = 0;
    skip_sp(&p);
    int crash = 0;
    if (!strncmp_(p, "--crash", 7)) crash = 1;
    if (pal_apply_scheme(name, crash) != 0) {
        vga_write(trs("theme: unknown scheme '", "theme: 未知配色方案 '")); vga_write(name);
        vga_write("' (dark|light|solarized|mono)\n");
        return;
    }
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
    /* v2.72: con_set_style_rgb() re-stamps the cells' snapshot background;
     * the frame itself must be cleared with the new colour in the same
     * breath, otherwise the old canvas survives wherever no text is drawn. */
    con_repaint_frame();
    /* v2.67: the i18n entry is a TEMPLATE ("theme set to %s"); printing
     * it raw leaked the %s into the terminal. */
    vga_write("theme: applied '"); vga_write(name);
    vga_write("' -- colors stored in HKLM\\Software\\CatOS\\Terminal\\Colors");
    vga_write(hive_dirty() ? " (run `hive save` to persist)\n" : " and saved\n");
}

/* v2.67: `hive` (alias `reg`) -- binary tree registry front-end.
 *   hive / hive ls [-R] [PATH] / hive get PATH / hive set PATH VALUE /
 *   hive setdw PATH N / hive mkkey PATH / hive del PATH / hive delkey PATH /
 *   hive export / hive import / hive chk / hive save                     */
static void cmd_reg(const char *arg) {
    g_last_rc = hive_cmd_run((char *)arg, g_uid, g_caps);
}

static void skip_sp(const char **pp) { while (**pp == ' ') (*pp)++; }

static uint32_t parse_hex(const char **pp) {
    const char *p = *pp;
    uint32_t v = 0;
    while (*p) {
        int d;
        if (*p >= '0' && *p <= '9')      d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        v = v * 16 + (uint32_t)d;
        p++;
    }
    *pp = p;
    return v;
}

static void cmd_palette(const char *arg) {
    const char *p = arg;
    skip_sp(&p);
    if (!*p) { vga_write(trs("usage: palette IDX RRGGBB\n", "用法：palette IDX RRGGBB\n")); return; }
    int idx = 0;
    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
    skip_sp(&p);
    uint32_t rgb = parse_hex(&p);
    if (idx < 0 || idx > 255) { vga_write(trs("palette: index must be 0..255\n", "palette: 索引必须在 0..255 之间\n")); return; }
    con_set_palette((uint8_t)idx, rgb);
    vga_write("palette["); print_int(idx);
    vga_write("] = "); print_hex(rgb); vga_write("\n");
}

static void cmd_rgb(const char *arg) {
    const char *p = arg;
    skip_sp(&p);
    uint32_t fg = parse_hex(&p);
    skip_sp(&p);
    uint32_t bg = *p ? parse_hex(&p) : 0;
    con_set_style_rgb(fg, bg, con_get_attrs());
    vga_write("fg="); print_hex(fg);
    vga_write(" bg="); print_hex(bg); vga_write("\n");
}

/* v2.60: unified display-mode switcher + graphics diagnostics.
 * Without args, cycles to the first available backend in priority order
 * gfx→vbe→gfx24.  Subcommands `gfx svgainfo|size|rgb|palette|display`
 * forward to the matching handler so the legacy standalone commands
 * remain aliases without duplicating logic. */
static void cmd_gfx(char *arg) {
    if (arg && *arg && (!strcmp_(arg, "mode") || !strcmp_(arg, "--query"))) {
        vga_write(trs("gfx mode: ", "图形模式："));
        vga_write(con_mode_name(con_get_mode()));
        vga_write(trs("  cols=", "  列=")); print_int(con_cols());
        vga_write(trs(" rows=", " 行=")); print_int(con_rows());
        vga_write("\n");
        return;
    }
    if (!arg || !*arg) {
        /* no arg: cycle first usable backend */
        int modes[] = { CON_MODE_GFX, CON_MODE_VBE, CON_MODE_VBE24 };
        for (int i = 0; i < (int)(sizeof(modes)/sizeof(modes[0])); i++) {
            int r = con_request_mode(modes[i], CON_ORIGIN_CONSOLE);
            if (r == CON_ENODEV) continue;
            if (r != 0) { vga_write(trs("gfx: switch failed\n", "gfx: 切换失败\n")); return; }
            con_clear();                           /* fresh canvas, no demo pixels */
            if (modes[i] == CON_MODE_GFX) {
                con_set_style(CON_WHITE, CON_BLUE, CON_ATTR_BOLD);
                vga_write("  CatOS gfx - 40x12 styled text, 256 colours\n");
                con_set_style_rgb(0xFFD700, 0x000000, CON_ATTR_BOLD);
                vga_write("  中文显示测试：你好，世界！\n");
                con_set_style(CON_YELLOW, CON_BLACK, CON_ATTR_UNDERLINE);
                vga_write("  underline   ");
                con_set_style(CON_LIGHT_CYAN, CON_BLACK, CON_ATTR_ITALIC);
                vga_write("italic   ");
                con_set_style(CON_LIGHT_GREEN, CON_BLACK, CON_ATTR_BOLD);
                vga_write("bold\n");
                con_set_style(CON_WHITE, CON_RED, CON_ATTR_INVERSE);
                vga_write("  inverse  ");
                con_set_style(CON_WHITE, CON_BLACK, CON_ATTR_STRIKE);
                vga_write("strike  ");
                con_set_style(200, CON_BLACK, 0);
                vga_write("grey-200\n");
                con_set_style(CON_LIGHT_GREY, CON_BLACK, 0);
                vga_write("  'text' to leave; 'gfx rgb/palette/display' for more\n\n");
            } else if (modes[i] == CON_MODE_VBE) {
                con_set_style_rgb(0xFFFFFF, 0x103050, CON_ATTR_BOLD);
                vga_write("  CatOS VBE 800x600x32 — truecolour terminal\n");
                con_set_style_rgb(0xFFD700, 0x000000, 0);
                vga_write("  中文显示测试：你好，世界！\n");
                con_set_style_rgb(0x7CFC00, 0x000000, CON_ATTR_UNDERLINE);
                vga_write("  16.7M 色，每个字符的颜色都是真正的 RGB\n");
                con_set_style(CON_LIGHT_GREY, CON_BLACK, 0);
                vga_write("  type 'text' + confirm Y to leave\n\n");
            } else {
                con_set_style_rgb(0xFFFFFF, 0x103050, CON_ATTR_BOLD);
                vga_write("  CatOS gfx24 800x600x24 — draw-ready canvas\n");
                con_set_style_rgb(0xFFD700, 0x000000, 0);
                vga_write("  中文显示测试：你好，世界！\n");
                con_set_style(CON_LIGHT_GREY, CON_BLACK, 0);
                vga_write("  type 'text' + confirm Y to leave\n\n");
            }
            return;
        }
        vga_write(trs("gfx: no suitable backend found\n", "gfx: 找不到可用的图形后端\n"));
        return;
    }
    /* subcommand dispatch: fall through to legacy handlers with "gfx" prefix */
    {
        const char *p = arg; skip_sp(&p);
        if (!strncmp_(p, "svgainfo", 8) && (p[8] == 0 || p[8] == ' '))
            cmd_svgainfo();
        else if (!strncmp_(p, "size", 4) && (p[4] == 0 || p[4] == ' '))
            cmd_size(p + 4);
        else if (!strncmp_(p, "rgb", 3) && (p[3] == 0 || p[3] == ' '))
            cmd_rgb(p + 3);
        else if (!strncmp_(p, "palette", 7) && (p[7] == 0 || p[7] == ' '))
            cmd_palette(p + 7);
        else if (!strncmp_(p, "display", 7) && (p[7] == 0 || p[7] == ' '))
            cmd_display();
        else if (!strcmp_(arg, "gfx")   ||
                 !strcmp_(arg, "vbe")   ||
                 !strcmp_(arg, "gfx24") ||
                 !strcmp_(arg, "vbe24")) {
            int wanted = CON_MODE_QUERY;
            if (!strcmp_(arg, "gfx"))         wanted = CON_MODE_GFX;
            else if (!strcmp_(arg, "vbe"))    wanted = CON_MODE_VBE;
            else                              wanted = CON_MODE_VBE24;
            int r = con_request_mode(wanted, CON_ORIGIN_CONSOLE);
            if (r == CON_ENODEV) {
                vga_write("gfx: "); vga_write(tr(L_GFX_NONE));
                vga_write("\n");
            } else if (r != 0) {
                vga_write(trs("gfx: switch failed\n", "gfx: 切换失败\n"));
            } else {
                con_clear();
                /* v2.62: persist the chosen boot display mode to the registry */
                hive_set_sz("HKLM\\System\\VideoMode", arg); hive_save();
                vga_write(tr(L_DISPLAY_MODE)); vga_write("\n");
            }
        } else {
            vga_write(trs("gfx: unknown subcommand '", "gfx: 未知子命令 '")); vga_write(arg);
            vga_write("'\nusage: gfx [svgainfo|size|rgb|palette|display|"
                      "gfx|vbe|gfx24]\n");
        }
    }
}

static void cmd_text(void) {
    int r = con_request_mode(CON_MODE_TEXT, CON_ORIGIN_CONSOLE);
    if (r == CON_EACCES)      vga_write(trs("text: denied - mode unchanged\n", "text: 已拒绝——模式未变\n"));
    else if (r == CON_ENODEV) vga_write("text: backend unavailable\n");
    else if (r != 0)          vga_write(trs("text: switch failed\n", "text: 切换失败\n"));
    else {
        /* #57: leaving graphics mode restores the 16-colour VGA DAC and
         * palette (be_text::enter does that), but the old graphics-era
         * cells would still be repainted cell-by-cell.  Wipe the screen so
         * the user gets a clean text console instead of a stale picture
         * rendered in text-mode colours. */
        vga_clear();
        con_set_style(CON_LIGHT_GREY, CON_BLACK, 0);
        vga_write("text: back to 80x25 text mode (CJK via dynamic font slots)\n");
    }
}

static void cmd_display(void) {
    vga_write("display backend  : "); vga_write(con_backend_name()); vga_write("\n");
    vga_write("mode             : "); vga_write(con_mode_name(con_get_mode()));
    vga_write("\n");
    vga_write("logical grid     : "); print_int(con_cols());
    vga_write(" x "); print_int(con_rows()); vga_write("\n");
    vga_write("physical pixels  : "); print_int(con_info(CON_INFO_PIXEL_W));
    vga_write(" x "); print_int(con_info(CON_INFO_PIXEL_H)); vga_write("\n");
    vga_write("colours / depth  : "); print_int(con_info(CON_INFO_COLORS));
    vga_write(" / "); print_int(con_info(CON_INFO_BPP)); vga_write(" bpp\n");
    vga_write("style bitmask    : "); print_hex((uint32_t)con_info(CON_INFO_ATTRS));
    vga_write("\nCJK support      : ");
    vga_write(con_info(CON_INFO_CJK) ? "yes" : "no (degrades to '?')");
    vga_write("\nGB2312 glyphs    : "); print_int(con_info(CON_INFO_HZ_COUNT));
    vga_write("\nlogical cell     : "); print_int(con_info(CON_INFO_CELL_W));
    vga_write("x"); print_int(con_info(CON_INFO_CELL_H));
    vga_write(" px (hanzi = 2x1 cells)");
    vga_write("\nanimated blink    : ");
    vga_write(con_info(CON_INFO_BLINK) ? "yes (0.5 s, PIT-driven)" : "no");
    vga_write("\nfast scrolling    : ");
    vga_write(con_info(CON_INFO_SCROLL_FAST) ? "framebuffer move" : "full repaint");
    vga_write("\nvbe accel         : ");
    if (con_get_mode() == CON_MODE_VBE) {
        vga_write(con_info(CON_INFO_WC) ? "MTRR-WC" : "UC(fallback)");
        vga_write(" + ");
        vga_write(con_info(CON_INFO_HW_SCROLL) ? "HW-pan" : "re-render");
    } else {
        vga_write("n/a (only in VBE mode)");
    }
    vga_write("\nIME               : F2 toggles pinyin (shell + edit)");
    vga_write("\nuser drawing API : ");
    if (con_get_mode() == CON_MODE_VBE24)
        vga_write("GFX24 (gfx_info/gfx_rect/gfx_blit, kernel-side writes)\n");
    else
        vga_write("inactive (framebuffers stay supervisor-only)\n");
    vga_write("graphics HW      : ");
    vga_write(svga_probe() ? "VMware SVGA II (15AD:0405)" : "Bochs/QEMU VBE dispi");
    vga_write(" | last gfx24 hw=");
    vga_write(gfx24_info(GFX_INFO_BACKEND) == 1 ? "SVGA-II" : "Bochs");
    vga_write(" hw_bpp="); print_int(gfx24_info(GFX_INFO_HW_BPP));
    vga_write(" stride="); print_int(gfx24_info(GFX_INFO_STRIDE));
    vga_write("\ndirty-cell tracker: ");
    print_int(con_info(CON_INFO_CELLS_DRAWN));
    vga_write(" drawn, ");
    print_int(con_info(CON_INFO_CELLS_SKIPPED));
    vga_write(" skipped (identical repaints cost 0 VRAM writes)\n");
    vga_write("glyph scale       : ");
    print_int(con_info(CON_INFO_CELL_SCALE));
    vga_write(" (0 compact 8x8 / 1 normal 8x16 / 2 large 16x32)\n");
    vga_write("switch policy    : ->gfx/vbe/vbe24 free; ->text needs physical Y\n");
}

/* v2.20: raw SVGA II register dump.  Useful when a VMware guest shows a
 * black screen: `id` must be 0x9000000x, `active` 1 after vbe/gfx24. */
static void cmd_svgainfo(void) {
    if (!svga_probe()) {
        vga_write(trs("svgainfo: no VMware SVGA II device (15AD:0405) on the PCI bus\n", "svgainfo: PCI 总线上没有 VMware SVGA II 设备（15AD:0405）\n"));
        vga_write("          QEMU std VGA uses Bochs dispi instead; use 'vdiag'\n");
        return;
    }
    vga_write("SVGA II adapter  : present\n");
    vga_write("id               : 0x"); print_hex(svga_info(SVGA_INFO_ID));
    vga_write("  caps=0x"); print_hex(svga_info(SVGA_INFO_CAPS));
    vga_write("  io=0x"); print_hex(svga_info(SVGA_INFO_IO_BASE));
    vga_write("\nactive           : "); print_int(svga_info(SVGA_INFO_ACTIVE));
    vga_write("  mode "); print_int(svga_info(SVGA_INFO_WIDTH));
    vga_write("x"); print_int(svga_info(SVGA_INFO_HEIGHT));
    vga_write("x"); print_int(svga_info(SVGA_INFO_BPP));
    vga_write("bpp  pitch="); print_int(svga_info(SVGA_INFO_PITCH));
    vga_write("\nframebuffer      : 0x"); print_hex(svga_info(SVGA_INFO_FB_PHYS));
    vga_write(" size="); print_int(svga_info(SVGA_INFO_FB_BYTES));
    vga_write("\nFIFO             : 0x"); print_hex(svga_info(SVGA_INFO_FIFO_PHYS));
    vga_write(" size="); print_int(svga_info(SVGA_INFO_FIFO_BYTES));
    vga_write(" updates="); print_int(svga_info(SVGA_INFO_FLUSHES));
    vga_write("\n");
}

static void cmd_about(void) {
    vga_clear();
    con_set_fg_rgb(pal_rgb(PAL_ACCENT));
    vga_write(trs(
        " ###   ###  #####  ###   ####\n"
        "#   # #   #   #   #   # #\n"
        "#     #####   #   #   #  ###\n"
        "#   # #   #   #   #   #     #\n"
        " ###  #   #   #    ###  ####\n",
        " ###   ###  #####  ###   ####\n"
        "#   # #   #   #   #   # #\n"
        "#     #####   #   #   #  ###\n"
        "#   # #   #   #   #   #     #\n"
        " ###  #   #   #    ###  ####\n"));
    con_set_fg_rgb(pal_rgb(PAL_FG));
    vga_write(trs("  A tiny Unix-like OS on GRUB Multiboot\n", "  基于 GRUB Multiboot 的微型类 Unix 操作系统\n"));
    vga_write(trs("  Architecture : i386 (32-bit x86)\n", "  体系结构     : i386（32 位 x86）\n"));
    vga_write(trs("  Bootloader   : GRUB 2 (ISO9660 / El Torito)\n", "  引导程序     : GRUB 2（ISO9660 / El Torito）\n"));
    vga_write(trs("  Kernel       : interrupts + paging + tasks\n", "  内核         : 中断 + 分页 + 任务\n"));
    vga_write(trs("                 ring3 CE apps via int 0x80, uid perms\n", "                 ring3 CE 程序经 int 0x80，按 UID 鉴权\n"));
    vga_write(trs("  Display      : front-end + text/gfx back-ends ('display')\n", "  显示         : 前端 + 文本/图形后端（display）\n"));
    vga_write(trs("  Input        : PS/2 keyboard (IRQ-driven)\n", "  输入         : PS/2 键盘（中断驱动）\n"));
    vga_write(trs("  Filesystem   : FAT32 on IDE (persistent)\n", "  文件系统     : IDE 上的 FAT32（持久化）\n"));
    vga_write(trs("  Runs on      : QEMU / VMware / Bochs / real PC\n", "  运行平台     : QEMU / VMware / Bochs / 真实 PC\n"));
}


/* v2.72: `fprobe` -- answers "does the host really show the whole frame?".
 * Fills the entire 800x600 surface, then marks the four corners and the
 * last pixel column/row.  If a corner mark is cut off, the surface the
 * host displays is smaller than the mode we programmed and the fix belongs
 * in the SVGA mode set, not in the console renderer.  On a healthy path the
 * window is edge-to-edge dark with four bright corner blocks. */
static void geom_num(char *dst, int max, int v);   /* v2.72, defined below */
static int  strcat_b(char *dst, int max, const char *src);
static void cmd_fprobe(void) {
    int w = con_info(CON_INFO_PIXEL_W), h = con_info(CON_INFO_PIXEL_H);
    if (w <= 0 || h <= 0) { con_write("fprobe: no graphics mode active\n"); return; }
    gfx24_clear(0x000000);
    gfx24_rect(0, 0, w, 2, 0x00FF00);          /* top edge    */
    gfx24_rect(0, h - 2, w, 2, 0x00FF00);      /* bottom edge */
    gfx24_rect(0, 0, 2, h, 0x00FF00);          /* left edge   */
    gfx24_rect(w - 2, 0, 2, h, 0x00FF00);      /* right edge  */
    gfx24_rect(0, 0, 40, 40, 0xFF0000);        /* top-left    RED   */
    gfx24_rect(w - 40, 0, 40, 40, 0x00FFFF);   /* top-right   CYAN  */
    gfx24_rect(0, h - 40, 40, 40, 0xFFFF00);   /* bottom-left YELLOW*/
    gfx24_rect(w - 40, h - 40, 40, 40, 0xFF00FF); /* bottom-right MAGENTA */
    con_write("fprobe: filled ");
    char b[48]; b[0] = 0;
    geom_num(b, (int)sizeof(b), w); strcat_b(b, (int)sizeof(b), "x");
    geom_num(b, (int)sizeof(b), h); strcat_b(b, (int)sizeof(b), " with 4 corner marks\n");
    con_write(b);
    con_write("if a corner is missing, the host surface is smaller than the mode\n");
}

static void cmd_sndmap(void) {
    if (!es1371_info(ES1371_INFO_READY)) {
        con_write("sndmap: no ensoniq device ready\n");
        return;
    }
    con_write("sndmap: dword readback at io_base+off (read-only, no writes)\n");
    char b[24];
    for (int row = 0; row < 4; row++) {
        for (int i = 0; i < 4; i++) {
            int off = row * 16 + i * 4;
            char ob[8]; ob[0] = 0;
            const char *hx = "0123456789ABCDEF";
            ob[0] = '0'; ob[1] = 'x';
            ob[2] = hx[(off >> 4) & 15]; ob[3] = hx[off & 15]; ob[4] = 0;
            con_write(" ");
            con_write(ob); con_write("=");
            int v = es1371_rd_raw(off);
            if (v < 0) { con_write("----"); continue; }
            con_write("0x");
            b[0] = 0;
            for (int k = 0; k < 8; k++) {
                char c = hx[((uint32_t)v >> ((7 - k) * 4)) & 15];
                b[k] = c; b[k + 1] = 0;
            }
            con_write(b);
        }
        con_write("\n");
    }
}

/* v2.72: `sndregs` -- raw ES1370/ES1371 readback.  The VMware box is silent
 * and QEMU has no ES1371 model, so this command exists to get real numbers
 * back from the machine that actually misbehaves:
 *   - if FRAMECNT never moves while a tone plays, the DMA channel was never
 *     armed (register map / sample-rate programming bug);
 *   - if FRAMECNT moves but there is no sound, the AK4531 codec is still
 *     muted (the codec is write-only, so it cannot be read back).
 * Deliberately prints raw words: interpreting them is a separate step. */
static void cmd_sndregs(void) {
    vga_write("snd: ensoniq id=");
    print_hex(0x1274u);
    vga_write(" dev=");
    print_hex((uint32_t)es1371_info(ES1371_INFO_DEVICE));
    vga_write(" io=0x");
    print_hex((uint32_t)es1371_info(ES1371_INFO_IOBASE) & 0xFFFFu);
    vga_write(" irq=");
    print_int(es1371_info(ES1371_INFO_IRQ));
    vga_write(" ready=");
    print_int(es1371_info(ES1371_INFO_READY));
    vga_write("\n  CONTROL=0x");
    print_hex((uint32_t)es1371_info(ES1371_INFO_CONTROL) & 0xFFFFu);
    vga_write("  STATUS=0x");
    print_hex((uint32_t)es1371_info(ES1371_INFO_STATUS) & 0xFFFFu);
    vga_write("  SERIAL=0x");
    print_hex((uint32_t)es1371_info(ES1371_INFO_SERIAL) & 0xFFFFu);
    vga_write("\n  SCOUNT=0x");
    print_hex((uint32_t)es1371_info(ES1371_INFO_SCOUNT));
    vga_write("  FRAMECNT=0x");
    print_hex((uint32_t)es1371_info(ES1371_INFO_FRAMECNT));
    vga_write("  PORT20=0x");
    print_hex((uint32_t)es1371_info(ES1371_INFO_SERR) & 0xFFFFu);
    vga_write("\n  rate=");
    print_int(es1371_info(ES1371_INFO_RATE));
    vga_write(" bufsz=");
    print_int(es1371_info(ES1371_INFO_BUFSZ));
    vga_write("\n");
    if ((uint32_t)es1371_info(ES1371_INFO_DEVICE) == 0x1371u) {
        vga_write("  note: ES1371 (VMware).  This driver programs it with the\n"
                  "        ES1370 register layout and never configures the\n"
                  "        AK4531 codec (its codec interface is write-only and\n"
                  "        unverified), so silence here is expected until that\n"
                  "        is rewritten against these readings.\n");
        g_last_rc = 3;   /* distinct status: 'device known, path unverified' */
    } else {
        vga_write("  note: ES1370 (QEMU).  Codec unmute is unnecessary on\n"
                  "        this model; silence means arm_dma() failed.\n");
        g_last_rc = 0;
    }
}

static void cmd_uname(void) {
    vga_write(trs("CatOS i386 0.3 (grub-multiboot, 5-pillar kernel)\n", "CatOS i386 0.3（GRUB Multiboot，五大支柱内核）\n"));
}

static void cmd_date(void) {
    rtc_time_t t; rtc_read(&t);
    vga_write(trs("Current RTC: ", "当前实时时钟："));
    print_pad(t.year, 4); vga_write("-");
    print_pad(t.month, 2); vga_write("-");
    print_pad(t.day, 2); vga_write(" ");
    print_pad(t.hour, 2); vga_write(":");
    print_pad(t.minute, 2); vga_write(":");
    print_pad(t.second, 2);
    vga_write("\n");
}

/* =====================================================================
 * NIGHT-P3: Linux-style easter eggs + UX enhancements.
 * All additions in this block are marked "// NIGHT-P3" so the main
 * agent can spot merge conflicts in the shell command table.
 * ===================================================================== */

/* ---------- NIGHT-P3: cowsay ----------------------------------------- */
static const char *COW_ART[] = {
    "        ^__^",
    "        (oo)\\_______",
    "        (__)\\       )\\/\\",
    "            ||----w |",
    "            ||     ||",
    0
};
static const char *TUX_ART[] = {
    "        .--.",
    "       |o_o |",
    "       |:_/ |",
    "      //   \\ \\",
    "     (|     | )",
    "     /'\\_   _/`\\",
    "     \\___)=(___/",
    0
};

/* display width: ASCII 1, UTF-8 2-byte 1, 3-byte CJK 2, 4-byte 4.
 * Used to size speech/quote boxes so CJK borders line up. */
static int disp_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if ((*p & 0xE0) == 0xC0) { w += 1; p += 2; }
        else if ((*p & 0xF0) == 0xE0) { w += 2; p += 3; }
        else if ((*p & 0xF8) == 0xF0) { w += 2; p += 4; }
        else { w += 1; p += 1; }
    }
    return w;
}

/* display width of the first n bytes (UTF-8 aware), for cursor placement. */
static int disp_width_n(const char *s, int n) {
    int w = 0, i = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (i < n && *p) {
        if ((*p & 0xE0) == 0xC0) { w += 1; p += 2; i += 2; }
        else if ((*p & 0xF0) == 0xE0) { w += 2; p += 3; i += 3; }
        else if ((*p & 0xF8) == 0xF0) { w += 2; p += 4; i += 4; }
        else { w += 1; p += 1; i += 1; }
    }
    return w;
}

static void cmd_cowsay(const char *arg) {
    const char *role = "cow";
    const char *msg = arg;
    /* optional "-f tux" role selector */
    if (msg[0] == '-' && msg[1] == 'f') {
        const char *p = msg + 2;
        while (*p == ' ') p++;
        const char *r = p;
        while (*p && *p != ' ') p++;
        int rlen = (int)(p - r);
        if (*p == ' ') { p++; while (*p == ' ') p++; }
        if (rlen == 3 && r[0] == 't' && r[1] == 'u' && r[2] == 'x')
            role = "tux";
        msg = p;
    }
    char mbuf[44]; int k = 0;
    while (*msg && k < 42) { mbuf[k++] = *msg++; }
    mbuf[k] = 0;
    if (!mbuf[0]) {
        const char *def = !strcmp_(role, "tux") ? "Hello, I am Tux!" : "Moo!";
        k = 0; while (def[k] && k < 42) { mbuf[k] = def[k]; k++; }
        mbuf[k] = 0;
    }
    int w = disp_width(mbuf);
    vga_write("  ");
    for (int i = 0; i < w + 2; i++) vga_putc('-');
    vga_write("\n");
    vga_write("  < "); vga_write(mbuf); vga_write(" >\n");
    vga_write("  ");
    for (int i = 0; i < w + 2; i++) vga_putc('-');
    vga_write("\n");
    const char **art = !strcmp_(role, "tux") ? TUX_ART : COW_ART;
    for (int i = 0; art[i]; i++) { vga_write(art[i]); vga_write("\n"); }
}

/* ---------- NIGHT-P3: sl (steam train animation) -------------------- */
static const char *SL_LOCO[] = {
    "       [_]         ",
    "    ___________    ",
    "   |  O     O  |   ",
    "   |___________|   ",
    "  /|           |\\  ",
    " (O)           (O) ",
    0
};
static const char *SL_SMOKE[] = {
    "  o          ",
    "   o    o    ",
    " o      o    ",
    0
};

/* ---------- v2.75: CLI animations (ported from deliverables/tui-preview) */
/* 10-row ASCII steam train from tui-preview/ascii小火车.txt.  Drawn as a
 * clipped horizontal window so it works on both 80-col text mode and the
 * 100-col gfx grid. */
static const char *TRAIN_ART[] = {
"      ====        ________                ___________",
"  _D _|  |_______/        \__I_I_____===__|_________|",
"   |(_)---  |   H\________/ |   |        =|___ ___|      _________________",
"   /     |  |   H  |  |     |   |         ||_| |_||     _|                \_____A",
"  |      |  |   H  |__--------------------| [___] |   =|                        |",
"  | ________|___H__/__|_____/[][]~\_______|       |   -|                        |",
"  |/ |   |-----------I_____I [][] []  D   |=======|____|________________________|_",
"__/ =| o |=-~~\  /~~\  /~~\  /~~\ ____Y___________|__|__________________________|_",
" |/-=|___|=    ||    ||    ||    ||_____/~\___/          |___ |  ___ |  ___ |",
"  \_/      \__/  \__/  \__/  \__/      \_/               \___|  \___|  \___|",
0
};
static const char SPACES100[] =
"                                                                                                    ";

static int anim_anykey(void) { return kb_getkey_timed(0) >= 0; }

static void anim_end(void) {
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
    con_clear();
    con_unsuppress_cursor();
}

/* `train`: the tui-preview choo-choo, two passes, any key quits. */
static void cmd_train(void) {
    int cols = con_cols(), rows = con_rows();
    if (cols < 40 || rows < 14) { vga_write(trs("train: console too small\n", "train: 控制台太小\n")); return; }
    con_suppress_cursor(1);
    con_clear();
    int wmax = 0;
    for (int r = 0; TRAIN_ART[r]; r++) {
        int n = slen_(TRAIN_ART[r]);
        if (n > wmax) wmax = n;
    }
    int y0 = (rows - 12) / 2;
    if (y0 < 2) y0 = 2;
    for (int pass = 0; pass < 2; pass++) {
        for (int off = cols; off > -wmax - 2; off -= 3) {
            if (anim_anykey()) { anim_end(); return; }
            /* wipe the band with the canvas colour, then draw the window */
            con_set_style_rgb(pal_rgb(PAL_BG), pal_rgb(PAL_BG), 0);
            for (int r = 0; r < 11; r++)
                con_write_at(0, (uint8_t)(y0 + r), SPACES100);
            for (int r = 0; TRAIN_ART[r]; r++) {
                const char *s = TRAIN_ART[r];
                int len = slen_(s);
                /* v2.66 lessons: smoke/loco in accent, rest in info */
                con_set_fg_rgb(r < 2 ? pal_rgb(PAL_WARNING) : pal_rgb(PAL_ACCENT));
                for (int i = 0; i < len; i++) {
                    int x = off + i;
                    if (x < 0 || x >= cols) continue;
                    con_putc_at((uint8_t)x, (uint8_t)(y0 + r), s[i]);
                }
            }
            if (pass == 0 && off > cols / 2)
                con_set_fg_rgb(pal_rgb(PAL_SUCCESS));
            if (pass == 0 && off > cols / 2)
                con_write_at((uint8_t)2, (uint8_t)(y0 + 11), "  * CHOO CHOO *");
            task_sleep_ticks(4);          /* ~40 ms / frame */
        }
    }
    anim_end();
}

/* `rain`: full-screen digital rain (tui-preview/字符雨.bat), any key quits. */
#define RAIN_MAX_COLS 128
static uint32_t rain_seed = 0x2545F491u;
static uint32_t rain_rnd(void) {
    rain_seed = rain_seed * 1103515245u + 12345u;
    return (rain_seed >> 16) & 0x7FFF;
}
static const char rain_chars[] = "0123456789ABCDEF@#$%&*+-<>";

static void cmd_rain(void) {
    int cols = con_cols(), rows = con_rows();
    if (cols < 20 || rows < 10) { vga_write(trs("rain: console too small\n", "rain: 控制台太小\n")); return; }
    if (cols > RAIN_MAX_COLS) cols = RAIN_MAX_COLS;
    static int16_t head[RAIN_MAX_COLS];
    static int16_t trail[RAIN_MAX_COLS];
    static int16_t speed[RAIN_MAX_COLS];
    static int16_t acc[RAIN_MAX_COLS];
    for (int x = 0; x < cols; x++) {
        head[x] = (int16_t)(rain_rnd() % rows);
        trail[x] = (int16_t)(6 + rain_rnd() % 16);
        speed[x] = (int16_t)(1 + rain_rnd() % 3);
        acc[x] = 0;
    }
    con_suppress_cursor(1);
    con_set_style_rgb(pal_rgb(PAL_BG), pal_rgb(PAL_BG), 0);
    con_clear();
    while (!anim_anykey()) {
        for (int x = 0; x < cols; x++) {
            acc[x] += speed[x];
            if (acc[x] < 4) continue;
            acc[x] -= 4;
            int old = head[x];
            head[x]++;
            if (old >= 0 && old < rows) {
                con_set_fg_rgb(0x33CC44);            /* trail: mid green */
                con_putc_at((uint8_t)x, (uint8_t)old,
                            rain_chars[rain_rnd() % (sizeof(rain_chars) - 1)]);
            }
            if (head[x] >= 0 && head[x] < rows) {
                con_set_fg_rgb(0xEAFFF2);            /* head: near-white */
                con_putc_at((uint8_t)x, (uint8_t)head[x],
                            rain_chars[rain_rnd() % (sizeof(rain_chars) - 1)]);
            }
            int tail = head[x] - trail[x];
            if (tail >= 0 && tail < rows) {
                con_set_style_rgb(pal_rgb(PAL_BG), pal_rgb(PAL_BG), 0);
                con_putc_at((uint8_t)x, (uint8_t)tail, ' ');
            }
            if (tail > rows) {                        /* column finished: respawn */
                head[x] = (int16_t)-(rain_rnd() % 15);
                trail[x] = (int16_t)(6 + rain_rnd() % 16);
                speed[x] = (int16_t)(1 + rain_rnd() % 3);
            }
        }
        task_sleep_ticks(2);                          /* ~20 ms / frame */
    }
    anim_end();
}

static void cmd_sl(void) {
    for (int f = 0; f < 16; f++) {
        vga_clear();
        int col = 64 - f * 4;
        if (col < 0) col = 0;
        vga_write("\n\n\n");
        vga_write("  ");
        for (int i = 0; i < col; i++) vga_putc(' ');
        vga_write(SL_SMOKE[f % 3]); vga_write("\n");
        for (int r = 0; SL_LOCO[r]; r++) {
            vga_write("  ");
            for (int i = 0; i < col; i++) vga_putc(' ');
            vga_write(SL_LOCO[r]); vga_write("\n");
        }
        if (f < 5) vga_write("\n  * CHOO CHOO! *\n");
        task_sleep_ticks(6);   /* ~60 ms per frame, tick = 10 ms */
    }
}

/* ---------- NIGHT-P3: fortune (random quotes) ------------------------ */
static const char *FORTUNES[] = {
    "The early bird catches the worm, but the second mouse gets the cheese.",
    "Talk is cheap. Show me the code. -- Linus Torvalds",
    "Simplicity is the soul of efficiency. -- Austin Freeman",
    "Premature optimization is the root of all evil. -- Donald Knuth",
    "Code should be written for humans first, computers second. -- M. Fowler",
    "First, solve the problem. Then, write the code. -- John Johnson",
    "Experience is the name everyone gives to their mistakes. -- Oscar Wilde",
    "Life is what happens when you are busy planning. -- John Lennon",
    "Stay hungry, stay foolish. -- Steve Jobs",
    "The only way to do great work is to love what you do. -- Steve Jobs",
    "In the middle of difficulty lies opportunity. -- Albert Einstein",
    "However slow you go, do not stop. -- Confucius",
    "The journey of a thousand miles begins with a single step. -- Lao Tzu",
    "Knowledge is power. -- Francis Bacon",
    "Well begun is half done. -- Aristotle",
    "Where there is a will there is a way.",
    "Practice makes perfect.",
    "A bug in the hand is worth two in the backlog.",
    "Hard things: cache invalidation, naming, off-by-one errors.",
    "Hello, world.",
    "千里之行，始于足下。—— 老子",
    "学而不思则罔，思而不学则殆。—— 孔子",
    "天行健，君子以自强不息。—— 《周易》",
    "不积跬步，无以至千里。—— 荀子",
    "路漫漫其修远兮，吾将上下而求索。—— 屈原",
    "宝剑锋从磨砺出，梅花香自苦寒来。",
    "读书破万卷，下笔如有神。—— 杜甫",
    "业精于勤，荒于嬉。—— 韩愈",
    "山重水复疑无路，柳暗花明又一村。—— 陆游",
    "海内存知己，天涯若比邻。—— 王勃",
    "只要功夫深，铁杵磨成针。",
    "代码写完不是结束，测试通过才算开始。",
    0
};
#define NFORT ((int)(sizeof(FORTUNES) / sizeof(FORTUNES[0])) - 1)

static uint32_t g_fortune_seq = 0;   /* NIGHT-P3: breaks same-second repeats */
static void cmd_fortune(void) {
    rtc_time_t t; rtc_read(&t);
    extern volatile uint32_t timer_ticks;
    g_fortune_seq++;
    uint32_t r = (uint32_t)t.second * 131u + (uint32_t)t.minute * 17u
               + timer_ticks + g_fortune_seq * 2654435761u;
    const char *q = FORTUNES[r % (uint32_t)NFORT];
    /* v2.45: border sized by display width (CJK counts 2) so the box
     * lines up for non-ASCII quotes; kept single-line to stay within 80. */
    int w = disp_width(q);
    vga_write("  ");
    for (int i = 0; i < w + 2; i++) vga_putc('-');
    vga_write("\n");
    vga_write("  | "); vga_write(q); vga_write(" |\n");
    vga_write("  ");
    for (int i = 0; i < w + 2; i++) vga_putc('-');
    vga_write("\n");
}

/* ---------- NIGHT-P3: cal (month / year calendar) ------------------- */
static const char *CAL_MON[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static int cal_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static int cal_days(int y, int m) {
    static int dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2) return cal_leap(y) ? 29 : 28;
    return dm[m - 1];
}
/* Sakamoto: returns 0 = Sunday .. 6 = Saturday */
static int cal_wday(int y, int m, int d) {
    static int t[12] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    int r = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return r < 0 ? r + 7 : r;
}
static void cal_build_month(int y, int m, char rows[7][22]) {
    for (int i = 0; i < 7; i++)
        for (int k = 0; k < 22; k++) rows[i][k] = 0;   /* #143: clear stale cells */
    int o = 0;
    /* center the "Mon YYYY" title over the 21-column weekday grid */
    for (int sp = 0; sp < 6; sp++) rows[0][o++] = ' ';
    const char *mn = CAL_MON[m - 1];
    rows[0][o++] = mn[0]; rows[0][o++] = mn[1]; rows[0][o++] = mn[2];
    rows[0][o++] = ' ';
    char yb[6]; itoa(y, yb, 4);
    for (int k = 0; yb[k]; k++) rows[0][o++] = yb[k];
    rows[0][o] = 0;
    const char *wd = "Su Mo Tu We Th Fr Sa";
    for (int k = 0; wd[k]; k++) rows[1][k] = wd[k];
    rows[1][20] = 0;
    int col = cal_wday(y, m, 1);
    int dim = cal_days(y, m);
    int row = 2;
    for (int d = 1; d <= dim; d++) {
        int off = col * 3;
        char db[4]; itoa(d, db, 0);
        if (d < 10) { rows[row][off] = ' '; rows[row][off + 1] = db[0]; }
        else        { rows[row][off] = db[0]; rows[row][off + 1] = db[1]; }
        rows[row][off + 2] = ' ';
        col++;
        if (col == 7) { col = 0; row++; }
    }
    for (int r = 2; r < 7; r++) rows[r][20] = 0;
}
static void cal_print_one(int y, int m) {
    char rows[7][22];
    cal_build_month(y, m, rows);
    for (int r = 0; r < 7; r++) {
        vga_write("  "); vga_write(rows[r]); vga_write("\n");
    }
}
static void cal_print_year(int y) {
    for (int g = 0; g < 4; g++) {
        char blocks[3][7][22];
        for (int k = 0; k < 3; k++)
            cal_build_month(y, g * 3 + k + 1, blocks[k]);
        for (int r = 0; r < 7; r++) {
            vga_write("  ");
            for (int k = 0; k < 3; k++) {
                vga_write(blocks[k][r]);
                int len = slen_(blocks[k][r]);
                while (len < 21) { vga_putc(' '); len++; }
                if (k < 2) vga_write("  ");
            }
            vga_write("\n");
        }
        vga_write("\n");
    }
}
static void cmd_cal(const char *arg) {
    rtc_time_t t; rtc_read(&t);
    const char *p = arg;
    while (*p == ' ') p++;
    if (*p) {
        int y = 0;
        while (*p >= '0' && *p <= '9') { y = y * 10 + (*p - '0'); p++; }
        if (y < 1970 || y > 2099) {
            vga_write("cal: year out of range (1970..2099)\n");
            return;
        }
        cal_print_year(y);
    } else {
        cal_print_one(t.year, t.month);
    }
}

/* ---------- NIGHT-P3: yes (repeat, Ctrl+C aware) -------------------- */
static void cmd_yes(const char *arg) {
    const char *word = *arg ? arg : "y";
    for (int i = 0; i < 600; i++) {
        vga_write(word); vga_write("\n");
        int k = kb_getkey_timed(1);   /* 1 tick = 10 ms poll */
        if (k == 0x03) {              /* Ctrl+C */
            vga_write("^C\n");
            break;
        }
    }
}

/* ---------- NIGHT-P3 UX: echo with -n / -c COLOR --------------------- */
static const struct { const char *name; uint8_t idx; } ECHO_COLORS[] = {
    {"red",      VGA_LIGHT_RED},
    {"green",    VGA_LIGHT_GREEN},
    {"blue",     VGA_LIGHT_BLUE},
    {"yellow",   VGA_YELLOW},
    {"cyan",     VGA_LIGHT_CYAN},
    {"magenta",  VGA_LIGHT_MAGENTA},
    {"white",    VGA_WHITE},
    {"grey",     VGA_LIGHT_GREY},
    {"gray",     VGA_LIGHT_GREY},
    {"darkgrey", VGA_DARK_GREY},
    {"brown",    VGA_BROWN},
    {0, 0}
};
static int echo_color_lookup(const char *s) {
    for (int i = 0; ECHO_COLORS[i].name; i++)
        if (!strcmp_(s, ECHO_COLORS[i].name)) return ECHO_COLORS[i].idx;
    return -1;
}
/* v2.57: kernel clipboard.  `clip` prints the selection; `clip -c` /
 * `clip clear` empties it.  Content is UTF-8 (may hold hanzi). */
static void cmd_clip(char *arg) {
    char *p = arg;
    while (*p == ' ') p++;
    if (!strcmp_(p, "-c") || !strcmp_(p, "clear")) {
        clip_clear();
        vga_write("clip: cleared\n");
        return;
    }
    if (*p) { vga_write(trs("clip: unknown argument\n", "clip: 未知参数\n")); return; }
    if (clip_empty()) { vga_write("clip: (empty)\n"); return; }
    vga_write(clip_get());
    vga_putc('\n');
}

/* v2.57: mouse / scrollback diagnostic for tests and inspection.
 * Prints absolute pixel position, button mask and scrollback view offset. */
static void cmd_mouse(void) {
    int x, y; uint8_t btn;
    mouse_get_state(&x, &y, &btn);
    vga_write("mouse: x="); print_int(x);
    vga_write(" y="); print_int(y);
    vga_write(" btn="); print_int(btn);
    vga_write(" sb="); print_int(con_scrollback_view());
    vga_write(" pk="); print_int((int)mouse_packet_count());
    vga_write(" irq="); print_int((int)mouse_irq_count());
    vga_write(" ab="); print_int((int)mouse_aux_bytes());
    vga_write(" kb="); print_int((int)mouse_kbd_bytes_irq12());
    int id, wh, pk; mouse_dev_info(&id, &wh, &pk);
    vga_write(" id="); print_int(id);
    vga_write(" wheel="); print_int(wh);
    vga_write(" pkt="); print_int(pk);
    vga_putc('\n');
}

static void cmd_echo(char *arg) {
    int nl = 1, color = -1;
    char *p = arg;
    while (*p == '-') {
        if (p[1] == 'n' && (p[2] == ' ' || p[2] == 0)) {
            nl = 0; p += 2; while (*p == ' ') p++;
        } else if (p[1] == 'c' && p[2] == ' ') {
            char *q = p + 3;
            while (*q == ' ') q++;
            char *cn = q;
            while (*q && *q != ' ') q++;
            char save = *q; *q = 0;
            color = echo_color_lookup(cn);
            *q = save; p = q; while (*p == ' ') p++;
        } else break;
    }
    uint8_t old = con_get_fg();
    if (color >= 0) vga_set_fg((uint8_t)color);
    vga_write(p);
    if (nl) vga_write("\n");
    vga_set_fg(old);
}

/* ---------- NIGHT-P3 UX: unknown-command suggestion ------------------ */
static const char *BUILTIN_NAMES[] = {
    "help", "lang", "export", "setenv", "env", "unset", "alias", "df",
    "mount", "umount",
    "unalias", "clear", "cls", "gfx",
    "svgainfo", "text", "style", "rgb", "palette", "display", "size",
    "useredit", "usb", "vdiag", "echo", "about", "autostart", "uname",
    "date", "mem", "reboot", "hang", "shutdown", "dir", "ls", "cd", "mkdir",
    "rm", "cp", "mv", "format", "edit", "run", "sudo",
    "whoami", "id", "lsuser", "users", "caps", "login", "su", "useradd",
    "userdel", "passwd", "chcaps", "chmod", "chown", "history", "ps",
    "top", "jobs", "log", "hwinfo", "sndinfo", "sndtest", "netcfg",
    "uptime", "kill", "curl", "ping", "dns", "udp", "netdiag", "sshd",
    "faultinfo", "crashlog", "fsck", "color",
    "bsod", "blue", "panicmode",
    "cowsay", "sl", "rain", "train", "fortune", "cal", "yes",   /* NIGHT-P3 */
    "pkg", "wc", "tree",       /* NIGHT-P4 */
    "theme",                                  /* v2.61: dark / light token palette switch */
    "hive", "reg",                            /* v2.67: binary tree registry */
    "colors",                                 /* v2.72: live palette from hive */
    0
};
/* tiny Levenshtein for short command words */
static int cmd_lev(const char *a, const char *b) {
    int la = slen_(a), lb = slen_(b);
    int d[16][16];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            d[i][j] = d[i - 1][j - 1] + cost;
            if (d[i - 1][j] + 1 < d[i][j]) d[i][j] = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < d[i][j]) d[i][j] = d[i][j - 1] + 1;
        }
    return d[la][lb];
}
static const char *suggest_cmd(const char *cmd) {
    const char *best = 0; int bestd = 99;
    for (int i = 0; BUILTIN_NAMES[i]; i++) {
        const char *name = BUILTIN_NAMES[i];
        int d = cmd_lev(cmd, name);
        /* typed a prefix of the real command -> strong match */
        if (slen_(cmd) >= 2 && slen_(name) > slen_(cmd)) {
            int ok = 1;
            for (int k = 0; cmd[k]; k++)
                if (cmd[k] != name[k]) { ok = 0; break; }
            if (ok && d > 1) d = 1;
        }
        if (d < bestd) { bestd = d; best = name; }
    }
    return bestd <= 2 ? best : 0;
}

static void cmd_mem(void) {
    if (mbi && (mbi->flags & 1)) {
        vga_write("Lower memory (0-640K): "); print_int(mbi->mem_lower); vga_write(" KB\n");
        vga_write("Upper memory (>1MB)  : "); print_int(mbi->mem_upper); vga_write(" KB\n");
        vga_write("Total approx         : ");
        print_int((mbi->mem_lower + mbi->mem_upper) / 1024);
        vga_write(" MB\n");
        /* PMM view: frames the allocator actually manages (after the kernel
         * image and reserved DMA areas), not the raw bootloader report. */
        vga_write("PMM allocator        : used ");
        print_int(hw_num(HW_MEM_USED_KB) / 4);
        vga_write(" / free ");
        print_int(hw_num(HW_MEM_FREE_KB) / 4);
        vga_write(" frames (4K pages)\n");
    } else {
        vga_write(trs("Memory info not provided by GRUB.\n", "GRUB 未提供内存信息。\n"));
    }
}

static void cmd_reboot(void) {
    vga_write("Rebooting...\n");
    outb(0x64, 0xFE);
    while (1) __asm__ volatile ("hlt");
}

/* v2.67.1: `hang` used to be an unconditional cli/hlt -- one keystroke and
 * the machine is gone, which is exactly the "foreground task can seize the
 * console" hole.  It now honours the foreground budget: it still wedges the
 * CPU (that is the point of the command) but only until the watchdog says
 * the budget is spent, then it gives the console back.  AllowUnbounded=1
 * restores the old, deliberately fatal behaviour. */
static void cmd_hang(void) {
    int unbounded = hive_dw("HKSEC\\Policy\\Foreground\\AllowUnbounded", 0);
    vga_write(unbounded ? "Halting (unbounded). Press Ctrl-Alt-Del or close the VM.\n"
                        : "Halting until the foreground budget expires...\n");
    if (!unbounded) {
        while (!sec_fg_expired()) __asm__ volatile ("hlt");
        vga_write("hang: budget spent, interrupts re-enabled -- console returned\n");
        sec_audit(g_uid, "fg-timeout", "hang", 124);
        return;
    }
    __asm__ volatile ("cli");
    while (1) __asm__ volatile ("hlt");
}

/* v2.61: power-off via APM BIOS protected-mode call (INT 15h AX=5001h).
 * Falls back to a warm reboot if the APM call is unavailable or fails.
 * Needs CAP_SYS or root. */
static void cmd_shutdown(void) {
    /* v2.67: NO APM INT 15h.  A protected-mode INT 15h / AX=5001h went
     * through our own IDT gate (vector 0x15), raised an exception, and the
     * panic path drew a BSOD -- that was the "shutdown always bluescreens"
     * report.  ACPI PM1_CNT is what QEMU / VMware / UEFI implement. */
    g_last_rc = 1;
    con_write("shutdown: ACPI PM1_CNT (SLP_EN)\n");
    if (power_shutdown() == 0) {
        /* if the write worked the machine is already off */
        for (volatile uint32_t spin = 0; spin < 4000000u; spin++)
            if ((spin & 0xFFFFFu) == 0) outb(0x80, 0);
        con_write("shutdown: ACPI had no effect, trying legacy power ports\n");
    }
    power_shutdown_ports();
    for (volatile uint32_t spin = 0; spin < 2000000u; spin++)
        if ((spin & 0xFFFFFu) == 0) outb(0x80, 0);
    /* honest last resort: this firmware cannot power off */
    con_write("shutdown: firmware reports no power-off; rebooting instead\n");
    outb(0x64, 0xFE);
    while (1) __asm__ volatile ("hlt");
}

/* ---------- filesystem commands ---------- */
static void cmd_cd(const char *arg) {
    int r = fs_chdir(arg);
    if (r == FS_ERR_NOENT)      vga_write(trs("cd: no such directory\n", "cd: 找不到目录\n"));
    else if (r == FS_ERR_NOTDIR) vga_write(trs("cd: not a directory\n", "cd: 不是目录\n"));
}

/* v2.24 forward declarations (helpers live across shell.c) */
static int fs_copy_file(const char *src, const char *dst);

/* v2.24: write-permission check on the PARENT of a would-be entry -- the
 * shell is ring0, so nobody else does it.  Without this any user could
 * mkdir/cp straight into /etc or /bin (the ACL default only guards files
 * written through the redirect path). */
static int perm_write_on_parent(const char *path, char *parent, int pcap) {
    char abs[FS_PATH_MAX];
    fs_canon_path(path, abs, sizeof(abs));
    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) return -1;
    int plen = (int)(leaf - abs);
    if (plen == 1) { if (pcap > 0) parent[0] = '/'; parent[1] = 0; }
    else {
        if (plen >= pcap) return -1;
        for (int i = 0; i < plen - 1; i++) parent[i] = abs[i];
        parent[plen - 1] = 0;
    }
    return fs_perm_check(parent, g_uid, g_gid, PERM_WRITE) != 0 ? -13 : 0;
}

/* v2.24: mkdir [-p] PATH.  Without -p the parent must already exist;
 * with -p every missing level is created (FS_ERR_EXIST tolerated). */
static void cmd_mkdir_p(const char *arg, int parents) {
    if (!*arg) { vga_write(trs("usage: mkdir [-p] PATH\n", "用法：mkdir [-p] PATH\n")); return; }
    char parent[FS_PATH_MAX];
    if (perm_write_on_parent(arg, parent, sizeof(parent)) == -13) {
        vga_write(trs("mkdir: permission denied\n", "mkdir: 权限不足\n"));
        return;
    }
    if (!parents) {
        int r = fs_mkdir(arg);
        if (r == FS_OK)            return;
        if (r == FS_ERR_NOENT)      vga_write(trs("mkdir: parent directory not found\n", "mkdir: 父目录不存在\n"));
        else if (r == FS_ERR_NOTDIR) vga_write(trs("mkdir: parent is not a directory\n", "mkdir: 父目录不是目录\n"));
        else if (r == FS_ERR_EXIST) vga_write(trs("mkdir: file or directory already exists\n", "mkdir: 文件或目录已存在\n"));
        else if (r == FS_ERR_INVAL) vga_write(trs("mkdir: invalid name\n", "mkdir: 名称无效\n"));
        else if (r == FS_ERR_FULL)  vga_write("mkdir: disk full (FAT32)\n");
        return;
    }
    /* -p: create every missing level, tolerate existing ones */
    char cur[FS_PATH_MAX];
    int ci = 0;
    const char *p2 = arg;
    if (*p2 == '/') { cur[ci++] = '/'; p2++; }
    cur[ci] = 0;
    while (*p2) {
        const char *seg = p2;
        while (*p2 && *p2 != '/') p2++;
        int sl = (int)(p2 - seg);
        if (sl && ci + sl + 1 < (int)sizeof(cur)) {
            for (int k = 0; k < sl; k++) cur[ci++] = seg[k];
            cur[ci] = 0;
            if (fs_is_dir(cur)) { /* already there */ }
            else {
                int r = fs_mkdir(cur);
                if (r != FS_OK && r != FS_ERR_EXIST) {
                    vga_write(trs("mkdir: cannot create '", "mkdir: 无法创建 '"));
                    vga_write(cur);
                    vga_write("' (");
                    print_int(r);
                    vga_write(")\n");
                    return;
                }
            }
        }
        if (*p2 == '/') {
            if (ci + 1 < (int)sizeof(cur)) cur[ci++] = '/';
            cur[ci] = 0;
            p2++;
        }
    }
}

static void cmd_touch(const char *arg) {
    if (!*arg) { vga_write(trs("usage: touch FILE\n", "用法：touch FILE\n")); return; }
    /* #121: creating a NEW entry needs write perm on its parent, the same
     * guard mkdir/cp -r already apply; a touch of an existing file does not
     * modify the directory entry so it keeps the old behaviour. */
    uint32_t tc, ts;
    if (fs_resolve_file(arg, &tc, &ts) != 0) {
        char parent[FS_PATH_MAX];
        if (perm_write_on_parent(arg, parent, sizeof(parent)) == -13) {
            vga_write(trs("touch: permission denied\n", "touch: 权限不足\n")); return;
        }
    }
    int r = fs_touch(arg);
    if (r == FS_OK)            return;
    if (r == FS_ERR_NOENT)      vga_write(trs("touch: parent directory not found\n", "touch: 父目录不存在\n"));
    else if (r == FS_ERR_NOTDIR) vga_write("touch: is a directory\n");
    else if (r == FS_ERR_INVAL) vga_write(trs("touch: invalid name\n", "touch: 名称无效\n"));
    else if (r == FS_ERR_FULL)  vga_write("touch: disk full (FAT32)\n");
}

/* split a string into words (in place); returns word count, max bounded */
static int split_words(char *s, char **out, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        out[n++] = s;
        while (*s && *s != ' ') s++;
        if (*s) { *s = 0; s++; }
    }
    return n;
}

/* bounded concatenation: never writes past d+size; returns 0 if it fit */
static int strcat_b(char *d, int size, const char *s) {
    if (!d || size <= 0) return -1;
    while (*d && size > 0) { d++; size--; }
    if (size <= 1) return -1;
    while (*s && size > 1) { *d++ = *s++; size--; }
    *d = 0;
    return *s ? -1 : 0;
}

/* =====================================================================
 * v2.65: POSIX-style short/long option parser, shared by builtins that
 * want -x / --long / --long=V / -xV / -x V / -abc clustering.  Spec entries
 * are indexed; bit i of `seen` marks spec[i].  Other commands can adopt it
 * by supplying their own spec table -- no per-command hand-written
 * string walking needed.
 * ===================================================================== */
#define SI_OPT_MAX 12
typedef struct { char sc; const char *ln; int takes_arg; } si_spec_t;
typedef struct { unsigned seen; char val[SI_OPT_MAX][64]; } si_opts_t;
#define SI_HAS(o, i) ((o)->seen & (1u << (i)))

static int si_next_token(char **pp, char *out, int cap) {
    char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; return 0; }
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && n < cap - 1) out[n++] = *p++;
    out[n] = 0; *pp = p;
    return 1;
}

/* 0 = ok, -1 unknown, -2 ambiguous, -3 missing value, -4 unexpected value */
static int si_parse(char *line, const si_spec_t *spec, int nspec,
                    si_opts_t *o, char *err, int errcap) {
    o->seen = 0; err[0] = 0;
    char *p = line;
    char tok[96];
    while (si_next_token(&p, tok, sizeof(tok))) {
        if (tok[0] != '-' || tok[1] == 0) continue;   /* positional (unused) */
        if (!strcmp_(tok, "--")) break;
        if (tok[1] == '-') {
            const char *body = tok + 2;
            char name[64]; int nl = 0;
            while (body[nl] && body[nl] != '=' && nl < 63) { name[nl] = body[nl]; nl++; }
            name[nl] = 0;
            const char *val = (body[nl] == '=') ? body + nl + 1 : 0;
            int hit = -1, cnt = 0;
            for (int i = 0; i < nspec; i++) {
                if (!strcmp_(name, spec[i].ln)) { hit = i; cnt = 1; break; }
                if (strncmp_(name, spec[i].ln, nl) == 0) { hit = i; cnt++; }
            }
            if (cnt == 0) { strcat_b(err, errcap, "unknown option --"); strcat_b(err, errcap, name); return -1; }
            if (cnt > 1)  { strcat_b(err, errcap, "ambiguous option --"); strcat_b(err, errcap, name); return -2; }
            int i = hit;
            o->seen |= (1u << i);
            if (spec[i].takes_arg) {
                char buf[64];
                if (!val) {
                    if (!si_next_token(&p, buf, sizeof(buf))) {
                        strcat_b(err, errcap, "option --"); strcat_b(err, errcap, spec[i].ln);
                        strcat_b(err, errcap, " needs a value"); return -3;
                    }
                    val = buf;
                }
                int k = 0; while (val[k] && k < 63) { o->val[i][k] = val[k]; k++; }
                o->val[i][k] = 0;
            } else if (val) {
                strcat_b(err, errcap, "option --"); strcat_b(err, errcap, spec[i].ln);
                strcat_b(err, errcap, " takes no value"); return -4;
            }
            continue;
        }
        for (int j = 1; tok[j]; j++) {
            char c = tok[j];
            int hit = -1;
            for (int i = 0; i < nspec; i++) if (spec[i].sc == c) { hit = i; break; }
            if (hit < 0) {
                strcat_b(err, errcap, "unknown option -"); strcat_b(err, errcap, &tok[j]);
                return -1;
            }
            int i = hit;
            o->seen |= (1u << i);
            if (spec[i].takes_arg) {
                char buf[64];
                const char *val = tok[j + 1] ? tok + j + 1 : 0;
                if (!val && !si_next_token(&p, buf, sizeof(buf))) {
                    strcat_b(err, errcap, "option -"); strcat_b(err, errcap, &tok[j]);
                    strcat_b(err, errcap, " needs a value"); return -3;
                }
                if (!val) val = buf;
                int k = 0; while (val[k] && k < 63) { o->val[i][k] = val[k]; k++; }
                o->val[i][k] = 0;
                break;                       /* rest of cluster is the value */
            }
        }
    }
    return 0;
}

/* ---------- sndinfo: POSIX rewrite (v2.65) ----------------------------
 * Interface (options / formats / exit codes) follows the spec exactly.
 * Telemetry comes from the live driver: currently SB16 (ISA) or
 * ES1370/1371 (PCI).  Fields the driver does not export (period, avail,
 * underrun counters, AC97 mixer on the PCI path) are reported as n/a
 * instead of being invented.  Only card 0 exists until multi-card
 * enumeration lands -- -d N>0 returns exit 3.
 * -------------------------------------------------------------------- */
static void si_line(const char *s) { vga_write(s); vga_write("\n"); }
static void si_num(const char *pre, int v, const char *post) {
    vga_write(pre); print_int(v); vga_write(post); vga_write("\n");
}

static const char *si_card_name(int be) {
    if (be == 2) return (es1371_info(ES1371_INFO_DEVICE) == 0x1371)
                       ? "ES1371 PCI Audio (VMware)" : "ES1370 PCI Audio";
    return "Sound Blaster 16";
}

/* SB16 mixer volume register (4 high bits) -> percent */
static void si_pct_line(const char *name, int reg) {
    int v = snd_mixer_read(reg);
    vga_write(name); vga_write(": ");
    if (v < 0) { si_line("n/a"); return; }
    print_int(((v >> 4) * 100) / 15); vga_write("% [unmuted]\n");
}

static void si_ctrl_block(int be) {
    si_line("Card 0 mixer controls:");
    if (be != 1) {
        si_line("  n/a - this backend has no AC97 mixer register access");
        return;
    }
    si_pct_line("Master Playback", 0x22);
    si_pct_line("Master Capture",  0x28);
    int spk = snd_mixer_read(0x2E);
    vga_write("Speaker: "); si_line(spk >= 0 && (spk & 0x0C) ? "on" : "off");
    int mic = snd_mixer_read(0x30);
    vga_write("Mic Boost: ");
    if (mic < 0) si_line("n/a");
    else { vga_write("+"); print_int((mic & 0x07) * 3); si_line("dB"); }
}

static void si_pcm_block(int be, int capture) {
    if (capture) {
        si_line("PCM pcm0c (capture):");
        si_line("State: n/a (no capture path in current driver)");
        vga_write("\n");
        return;
    }
    si_line("PCM pcm0p (playback):");
    si_line(snd_info(SND_INFO_PLAYING) ? "State: RUNNING" : "State: STOPPED");
    vga_write("Sample rate: "); print_int(snd_info(SND_INFO_RATE));
    vga_write(" Hz, "); vga_write(be == 2 ? "2ch S16LE" : "1ch U8"); vga_write("\n");
    si_num("Buffer size: ", snd_info(SND_INFO_BUFSZ), 0);
    si_line(" frames (period/avail not exported by driver)");
    si_line("Underrun count: n/a");
    vga_write("\n");
}

static void si_list_block(int be) {
    vga_write("Card 0: "); si_line(si_card_name(be));
    vga_write("Driver: "); vga_write(snd_backend_name());
    si_line(" (CatOS Audio Subsys v0.3)");
    if (be == 2) {
        vga_write("PCI: vendor=0x"); print_hex((uint32_t)es1371_info(ES1371_INFO_VENDOR));
        vga_write(" device=0x");     print_hex((uint32_t)es1371_info(ES1371_INFO_DEVICE));
        vga_write(" irq=");          print_int(es1371_info(ES1371_INFO_IRQ));
        vga_write("\n");
        /* legacy regression tokens (tests/es1371_regress.py) */
        vga_write("snd: pci dev=0x"); print_hex((uint32_t)es1371_info(ES1371_INFO_DEVICE));
        vga_write(" iobase=0x");      print_hex((uint32_t)es1371_info(ES1371_INFO_IOBASE));
        vga_write(" irq=");           print_int(es1371_info(ES1371_INFO_IRQ));
        vga_write("\n");
    } else {
        vga_write("ISA: iobase=0x"); print_hex(SND_IOBASE);
        vga_write(" irq="); print_int(SND_IRQ_LINE);
        vga_write(" dma="); print_int(SND_DMA8_CHAN);
        vga_write("\n");
        /* legacy regression token (tests/snd_regress.py asserts on it) */
        vga_write("snd: SB16 ready iobase="); print_hex(SND_IOBASE);
        vga_write(" irq="); print_int(SND_IRQ_LINE);
        vga_write(" dma="); print_int(SND_DMA8_CHAN); vga_write("\n");
    }
    si_line("PCM Devices:");
    if (be == 2) si_line("pcm0p: Playback, channels 1/2, rates 5.5k~44.1k, fmt S16LE");
    else         si_line("pcm0p: Playback, channels 1, rates 5k~45k, fmt U8");
    si_line("pcm0c: Capture,  channels n/a, rates n/a, fmt n/a (unsupported)");
    si_ctrl_block(be);   /* prints its own "Card 0 mixer controls:" header */
}

static void si_short_block(int be) {
    vga_write("0 "); vga_write(si_card_name(be));
    si_line(" pcm0p:play pcm0c:n/a");
}

static void si_json_block(int be) {
    si_line("{");
    si_line("  \"cards\": [");
    si_line("    {");
    si_line("      \"card_id\": 0,");
    vga_write("      \"name\": \""); vga_write(si_card_name(be)); si_line("\",");
    vga_write("      \"driver\": \""); vga_write(snd_backend_name()); si_line("\",");
    si_line("      \"pcm\": [");
    si_line("        {\"id\":\"pcm0p\",\"dir\":\"playback\",\"ch_min\":1,\"ch_max\":2},");
    si_line("        {\"id\":\"pcm0c\",\"dir\":\"capture\",\"ch_min\":0,\"ch_max\":0}");
    si_line("      ]");
    si_line("    }");
    si_line("  ]");
    si_line("}");
}

static void si_help(void) {
    si_line("Usage: sndinfo [OPTIONS]");
    si_line("");
    si_line("  -h, --help           this help");
    si_line("  -v, --version        sndinfo + audio driver version");
    si_line("  -l, --list           list sound cards / PCM devices (default)");
    si_line("  -p, --pcm            live PCM stream state");
    si_line("  -c, --ctrl           mixer controls (volume / mute / gain)");
    si_line("  -d N, --dev N        query only card N");
    si_line("  -w, --watch          continuously refresh until a key is pressed");
    si_line("  -j, --json           JSON output (for osh scripts / GUI panels)");
    si_line("  -s, --short          one-line mode for pipes");
    si_line("  -q, --quiet          set exit code only, print nothing");
    si_line("  -t, --tone           play one test tone after the query");
    si_line("");
    si_line("Exit: 0 ok  1 bad args  2 no audio device  3 bad card id  4 driver query failed");
}

static void cmd_sndinfo(char *arg) {
    static const si_spec_t spec[SI_OPT_MAX] = {
        { 'h', "help",    0 }, { 'v', "version", 0 },
        { 'l', "list",    0 }, { 'p', "pcm",     0 },
        { 'c', "ctrl",    0 }, { 'd', "dev",     1 },
        { 'w', "watch",   0 }, { 'j', "json",    0 },
        { 's', "short",   0 }, { 'q', "quiet",   0 },
        { 't', "tone",    0 }, {  0, 0,          0 }
    };
    enum { O_H, O_V, O_L, O_P, O_C, O_D, O_W, O_J, O_S, O_Q, O_T };
    si_opts_t o;
    char err[128];
    g_last_rc = 0;

    if (si_parse(arg, spec, 11, &o, err, sizeof(err)) != 0) {
        vga_write("sndinfo: "); vga_write(err); vga_write("\n");
        si_line("sndinfo: use -h for help");
        g_last_rc = 1; return;
    }
    int quiet = SI_HAS(&o, O_Q);   /* -q: status only, never print text */
    if (SI_HAS(&o, O_H)) { si_help(); return; }
    if (SI_HAS(&o, O_V)) {
        si_line("sndinfo 0.65.0 (catos-userland)");
        vga_write("audio driver: "); vga_write(snd_backend_name());
        si_line(" / CatOS Audio Subsys v0.3");
        return;
    }
    if (!snd_info(SND_INFO_READY)) {
        if (!quiet) si_line("sndinfo: audio subsystem not initialized / no sound card");
        g_last_rc = 2; return;
    }
    if (snd_info(SND_INFO_ERROR) != 0) {
        if (!quiet) si_line("sndinfo: driver query interface failed");
        g_last_rc = 4; return;
    }
    if (SI_HAS(&o, O_D)) {
        char *end = o.val[O_D];
        int id = 0, any = 0;
        while (*end >= '0' && *end <= '9') { id = id * 10 + (*end - '0'); end++; any = 1; }
        if (!any || *end) {
            if (!quiet) {
                vga_write(trs("sndinfo: --dev expects a numeric card id, got '", "sndinfo: --dev 需要a numeric card id, got '"));
                vga_write(o.val[O_D]); si_line("'");
            }
            g_last_rc = 1; return;
        }
        if (id != 0) {
            if (!quiet) si_num("sndinfo: card ", id, " does not exist");
            g_last_rc = 3; return;
        }
    }
    if (quiet) return;                        /* exit code already 0 */

    int be = snd_info(SND_INFO_BACKEND);
    int have_mode = SI_HAS(&o, O_L) || SI_HAS(&o, O_P) || SI_HAS(&o, O_C) ||
                    SI_HAS(&o, O_J) || SI_HAS(&o, O_S) || SI_HAS(&o, O_W);
    if (!have_mode) o.seen |= (1u << O_L);    /* default action = -l */

    int rounds = SI_HAS(&o, O_W) ? -1 : 1;    /* -1 = until keypress */
    for (int r = 0; rounds < 0 || r < rounds; r++) {
        if (SI_HAS(&o, O_W)) {
            vga_write("[watch] refresh "); print_int(r + 1);
            si_line(" - any key stops");
        } else if (r) vga_write("\n");
        if (SI_HAS(&o, O_J))      si_json_block(be);
        else if (SI_HAS(&o, O_S)) si_short_block(be);
        else {
            if (SI_HAS(&o, O_L)) { si_list_block(be); vga_write("\n"); }
            if (SI_HAS(&o, O_P)) { si_pcm_block(be, 0); si_pcm_block(be, 1); }
            if (SI_HAS(&o, O_C)) si_ctrl_block(be);
        }
        if (rounds >= 0) break;
        uint32_t until = task_uptime_ms() + 800u;
        while (task_uptime_ms() < until) {
            if (kb_peek()) { (void)kb_getkey(); rounds = r + 1; break; }
            __asm__ volatile ("hlt");
        }
    }
    if (SI_HAS(&o, O_T)) {
        int rc = snd_tone(880u, 120u);
        if (rc < 0) { si_line("tone: playback failed"); g_last_rc = 4; return; }
        uint32_t until = task_uptime_ms() + 1000u;
        while (snd_info(SND_INFO_PLAYING) && task_uptime_ms() < until)
            __asm__ volatile ("hlt");
        si_line("[tone] 880 Hz / 120 ms played");
    }
}

/* v2.47: pending-guess one-shot confirm.  Holds the last command the shell
 * itself suggested after an unknown-command error on the MAIN prompt.  A
 * bare y/Y at the next main-prompt line replays it; any other line consumes
 * the offer.  In-command y/n prompts (PE->CE offer, format gate, gfx
 * consent, useradd wizard) read through their own kb_readline/kb_getkey and
 * never consult this state, so their answers follow the original path. */
static char pend_cmd[FS_NAME_MAX];
static char pend_arg[128];
static int  pend_valid;

static void pend_clear(void) {
    pend_valid = 0;
    pend_cmd[0] = 0;
    pend_arg[0] = 0;
}

static void pend_arm(const char *cmd, const char *arg) {
    if (g_script_mode) return;      /* no one to answer in scripts */
    pend_valid = 1;
    pend_cmd[0] = 0;
    strcat_b(pend_cmd, sizeof(pend_cmd), cmd);
    pend_arg[0] = 0;
    strcat_b(pend_arg, sizeof(pend_arg), arg ? arg : "");
}

/* v2.23 environment variables (defined near the shell loop below) */
static const char *env_get_(const char *key);

/* run "cmd" as a CE program: current directory first, then /bin (PATH).
 * Returns 0 when a program was found (spawn attempted), -1 if not found.
 * A command that already carries an extension (e.g. "hello.elf") is tried
 * verbatim -- ".ce" is appended only when the last path component has no
 * dot. */
static int cmd_has_ext(const char *cmd) {
    const char *p = cmd, *base = cmd;
    while (*p) {
        if (*p == '/') base = p + 1;
        else if (*p == '.') return 1;
        p++;
    }
    (void)base;
    return 0;
}

static int run_from_path(const char *cmd, char **argv, int argc, int bg, int uid) {
    if (!cmd || !*cmd) return -1;
    if (!fs_fs_ready()) return -1;
    char p[FS_PATH_MAX];
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    uint32_t c = 0, s = 0;
    const char *ext = cmd_has_ext(cmd) ? "" : ".ce";

    /* 1) current directory (bounded: never overflow like the old code) */
    p[0] = 0;
    /* v2.67.1: don't emit "//name" when cwd is "/" -- the extra slash is
     * harmless to FAT but ugly in every diagnostic and audit line. */
    if (!(pwd[0] == '/' && pwd[1] == 0)) {
        if (strcat_b(p, sizeof(p), pwd) != 0) return -1;
        if (strcat_b(p, sizeof(p), "/") != 0) return -1;
    } else {
        strcat_b(p, sizeof(p), "/");
    }
    if (strcat_b(p, sizeof(p), cmd) != 0) return -1;
    if (strcat_b(p, sizeof(p), ext) != 0) return -1;
    if (fs_resolve_file(p, &c, &s) == 0) {
        char why[56];
        if (sec_exec_check(p, uid, why, sizeof(why)) != 0) {
            vga_write(trs("exec: refused '", "exec: 被拒绝 '")); vga_write(p); vga_write("': ");
            vga_write(why); vga_write("\n");
            sec_audit(uid, "deny-exec", p, 13);
            return -2;
        }
        sec_audit(uid, "exec-ce", p, 0);
        spawn_and_maybe_wait(p, argc, argv, bg, uid);
        return 0;
    }
    /* 2) $PATH lookup (v2.23; default "/bin", colon-separated, in order) */
    const char *pathv = env_get_("PATH");
    if (!pathv || !*pathv) pathv = "/bin";
    const char *seg = pathv;
    while (*seg) {
        char dir[64];
        int di = 0;
        while (*seg && *seg != ':' && di < (int)sizeof(dir) - 1)
            dir[di++] = *seg++;
        dir[di] = 0;
        if (*seg == ':') seg++;
        if (!di) continue;
        p[0] = 0;
        if (strcat_b(p, sizeof(p), dir) != 0) continue;
        if (dir[di - 1] != '/' && strcat_b(p, sizeof(p), "/") != 0) continue;
        if (strcat_b(p, sizeof(p), cmd) != 0) continue;
        if (strcat_b(p, sizeof(p), ext) != 0) continue;
        if (fs_resolve_file(p, &c, &s) == 0) {
            char why[56];
            if (sec_exec_check(p, uid, why, sizeof(why)) != 0) {
                vga_write(trs("exec: refused '", "exec: 被拒绝 '")); vga_write(p); vga_write("': ");
                vga_write(why); vga_write("\n");
                sec_audit(uid, "deny-exec", p, 13);
                return -2;
            }
            sec_audit(uid, "exec-ce", p, 0);
            spawn_and_maybe_wait(p, argc, argv, bg, uid);
            return 0;
        }
    }
    return -1;
}

/* spawn a .ce as a ring3 task; foreground runs wait for it to exit,
 * background ('&') returns to the shell immediately */
void spawn_and_maybe_wait(const char *path, int argc, char **argv, int bg, int uid) {
    int tid = task_spawn_ce(path, argc, argv, uid);
    if (tid < 0) {
        vga_write(trs("run: cannot load '", "run: 无法加载 '"));
        vga_write(path);
        vga_write("' (");
        print_int(tid);
        vga_write(")\n");
        return;
    }
    if (bg) {
        task_t *bt = task_by_tid(tid);
        if (bt) bt->bg_mode = 1;      /* #59: capture stdout into the ring */
        g_last_bg_tid = tid;          /* v2.52: fg/bg/stop no-arg default */
        g_fg_tid = -1;                /* done with previous fg task */
        vga_write("[bg tid "); print_int(tid); vga_write("] ");
        vga_write(path); vga_write("\n");
        return;
    }
    g_fg_tid = tid;                 /* v2.60: track fg for ^C/^Z handling */
    task_wait_child(task_by_tid(tid));
    g_fg_tid = -1;                  /* v2.60: fg task is done */
}

/* ---- v2.5: shell redirection (| / > / >>) ----------------------------- */

/* split "cmd args [> F | >> F | | CMD]" into out_redir / pipe_to and
 * truncate arg at the operator.  Returns 1 when a redirection exists. */
static int parse_redir(char *arg, char **out_redir, int *append, char **pipe_to) {
    *out_redir = 0; *append = 0; *pipe_to = 0;
    for (char *q = arg; *q; q++) {
        if (*q == '|') { *pipe_to = q + 1; *q = 0; break; }
        if (*q == '>') {
            if (*(q + 1) == '>') { *append = 1; *out_redir = q + 2; *(q + 1) = 0; }
            else                 { *out_redir = q + 1; }
            *q = 0;
            break;
        }
    }
    if (*out_redir) while (**out_redir == ' ') (*out_redir)++;
    if (*pipe_to)   while (**pipe_to == ' ')   (*pipe_to)++;
    return (*out_redir || *pipe_to) ? 1 : 0;
}

/* make a redirection target absolute against the current directory */
static int redir_abspath(const char *name, char *out, int outlen) {
    if (!name || !*name) return -1;
    out[0] = 0;
    if (name[0] == '/') return strcat_b(out, outlen, name) == 0 ? 0 : -1;
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    if (pwd[0] == '/' && pwd[1] == 0) {
        if (strcat_b(out, outlen, "/") != 0) return -1;
    } else {
        if (strcat_b(out, outlen, pwd) != 0) return -1;
        if (strcat_b(out, outlen, "/") != 0) return -1;
    }
    return strcat_b(out, outlen, name) == 0 ? 0 : -1;
}

/* spawn a .ce and wire optional redirection: out_file (append flag) and/or
 * pipe ends.  fg = 1 waits for exit (and the shell then reaps the task). */
static void spawn_ce_redirect(const char *path, int argc, char **argv,
                              int fg, int uid,
                              const char *out_file, int append,
                              int out_pipe, int in_pipe) {
    int tid = task_spawn_ce(path, argc, argv, uid);
    if (tid < 0) {
        vga_write(trs("run: cannot load '", "run: 无法加载 '"));
        vga_write(path);
        vga_write("' (err ");
        print_int(tid);
        vga_write(")\n");
        return;
    }
    task_t *tt = task_by_tid(tid);
    if (out_file && out_file[0]) {
        char ab[FS_PATH_MAX];
        if (redir_abspath(out_file, ab, sizeof(ab)) != 0) {
            vga_write(trs("run: bad redirect path\n", "run: 重定向路径错误\n"));
            task_kill(tid, UID_ROOT);
            return;
        }
        /* #167: the pre-create/truncate below runs in ring0 and used to
         * skip fs_perm_check entirely; a non-root session could truncate or
         * create a protected file before the child's own SYS_WRITE checks
         * ran.  Re-check the target with the session identity up front, and
         * refuse without touching the file at all. */
        if (fs_perm_check(ab, g_uid, g_gid, PERM_WRITE) != 0) {
            vga_write(trs("run: permission denied: '", "run: 权限不足: '"));
            vga_write(ab);
            vga_write("'\n");
            task_kill(tid, UID_ROOT);
            return;
        }
        int i;
        for (i = 0; ab[i] && i < (int)sizeof(tt->out_file) - 1; i++)
            tt->out_file[i] = ab[i];
        tt->out_file[i] = 0;
        /* v2.5.1 (FAIL-5-1 fix): `>` truncates once up front, then every
         * SYS_WRITE must append so multi-line output is not clobbered. */
        tt->out_append = 1;
        if (!append)
            fs_write_raw_mode(ab, "", 0, 0);      /* pre-create/truncate; #49: 无条件截断 */
    }
    if (out_pipe >= 0) kpipe_assign_out(tid, out_pipe);
    if (in_pipe  >= 0) kpipe_assign_in(tid, in_pipe);
    if (!fg) {
        tt->bg_mode = 1;              /* #59: capture stdout into the ring */
        g_last_bg_tid = tid;          /* v2.52: fg/bg/stop no-arg default */
        g_fg_tid = -1;                /* v2.60: done with previous fg task */
        vga_write("[bg tid "); print_int(tid); vga_write("] ");
        vga_write(path); vga_write("\n");
        return;
    }
    g_fg_tid = tid;                 /* v2.60: track fg for ^C/^Z handling */
    task_wait_child(tt);
    g_fg_tid = -1;                  /* v2.60: fg task is done */
}

static void vga_clear_disabled(void) { }
static void cmd_run(char *arg, int uid) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write(trs("usage: run FILE.ce [args...] [&] [> F] [>> F] [| CMD]\n", "用法：run FILE.ce [args...] [&] [> F] [>> F] [| CMD]\n"));
        return;
    }
    if (!fs_fs_ready()) { vga_write(trs("run: no FAT32 volume.\n", "run: 没有 FAT32 卷.\n")); return; }

    char *out_redir = 0, *pipe_to = 0; int append = 0;
    parse_redir(arg, &out_redir, &append, &pipe_to);

    /* split FILE.ce from its arguments */
    char *file = arg;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; }
    char *argv[16];
    int argc = 1;
    argv[0] = file;
    argc += split_words(arg, argv + 1, 15);
    int bg = 0;
    if (argc > 1 && argv[argc - 1][0] == '&' && argv[argc - 1][1] == 0) {
        bg = 1;
        argc--;
    }

    /* P1: PE32/MZ detection.  Offer a one-shot conversion to CE01 and save
     * it on disk.  In non-interactive contexts (pipe, redirect, background,
     * autostart script) the offer is skipped: there is no one to answer. */
    {
        uint32_t pclus = 0, psize = 0;
        if (fs_resolve_file(file, &pclus, &psize) == 0 && psize >= 2) {
            char mz[2];
            if (fs_read_at(file, mz, 0, 2) == 2 &&
                mz[0] == 'M' && mz[1] == 'Z') {
                int noninter = pipe_to || out_redir || bg || g_script_mode;
                if (noninter) {
                    vga_write("run: '"); vga_write(file);
                    vga_write("' is a PE32/MZ binary; skipping the conversion "
                              "offer in a non-interactive session.\n");
                } else {
                vga_write("'"); vga_write(file);
                vga_write("' starts with MZ (PE32 binary). Convert to CE01 "
                          "and save? (y/n) ");
                char ans[16];
                kb_readline(ans, sizeof(ans));
                if (ans[0] != 'y' && ans[0] != 'Y') {
                    vga_write("PE conversion skipped; trying direct load.\n");
                } else {
                    /* default output: same directory, base name + .ce */
                    char def[FS_PATH_MAX];
                    int fi = 0; while (file[fi]) fi++;
                    int dot = fi, slash = -1;
                    for (int k = 0; k < fi; k++) {
                        if (file[k] == '/') slash = k;
                        if (file[k] == '.') dot = k;
                    }
                    int base = slash + 1, di = 0;
                    if (slash >= 0) {
                        for (int k = 0; k <= slash; k++) def[di++] = file[k];
                    }
                    for (int k = base; k < dot; k++) def[di++] = file[k];
                    def[di++] = '.'; def[di++] = 'c'; def[di++] = 'e';
                    def[di] = 0;

                    vga_write("Save CE as ["); vga_write(def); vga_write("]: ");
                    char name[FS_PATH_MAX];
                    kb_readline(name, sizeof(name));
                    const char *dst = name[0] ? name : def;
                    char cerr[64];
                    int cr = pe_to_ce_file(file, dst, g_uid, g_gid,
                                           cerr, sizeof(cerr));
                    if (cr == 0) {
                        vga_write("converted to "); vga_write(dst); vga_write("\n");
                    } else {
                        vga_write(trs("convert failed: ", "转换失败: ")); vga_write(cerr); vga_write("\n");
                    }
                    return;
                }
                }
            }
        }
    }

    if (pipe_to && *pipe_to) {
        /* a | b : producer (background, writes the pipe) then consumer
         * (foreground, reads the pipe until EOF when the writer exits) */
        char *file2 = pipe_to;
        char *sp = pipe_to;
        while (*sp && *sp != ' ') sp++;
        if (*sp) { *sp = 0; sp++; }
        char *argv2[16];
        int argc2 = 1;
        argv2[0] = file2;
        argc2 += split_words(sp, argv2 + 1, 15);
        int h = kpipe_create();
        if (h < 0) { vga_write(trs("run: no pipe slots\n", "run: 没有空闲管道\n")); return; }
        vga_clear();
        spawn_ce_redirect(file, argc, argv, 0, uid, 0, 0, h, -1);
        spawn_ce_redirect(file2, argc2, argv2, 1, uid, 0, 0, -1, h);
        return;
    }
    vga_clear_disabled();
    spawn_ce_redirect(file, argc, argv, !bg, uid, out_redir, append, -1, -1);
}

/* ---------- netcfg: view/change IP config (VMware NAT etc.) ---------- */
static int ip4_parse_(const char *s, uint8_t out[4]) {
    int v = 0, n = 0;
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (const char *q = s; *q; q++) {
        if (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); if (v > 255) return 0; }
        else if (*q == '.') { if (n >= 3) return 0; out[n++] = (uint8_t)v; v = 0; }
        else return 0;
    }
    out[n++] = (uint8_t)v;
    return n == 4;
}

static void cmd_netcfg(char *arg) {
    extern uint8_t net_our_ip[4];
    extern uint8_t net_gw_ip[4];
    extern void net_dns_server_set(const uint8_t ip[4]);
    extern const uint8_t *net_dns_server_get(void);
    extern void net_arp_flush(void);
    extern int  net_dhcp_once(void);
    extern void net_dhcp_retry(void);
    extern void net_cfg_user_set(void);
    extern int  net_dhcp_ok(void);

    if (arg && !strcmp_(arg, "dhcp")) {       /* v2.66: DHCP discover */
        vga_write("netcfg: DHCP discover ...\n");
        net_dhcp_retry();
        if (net_dhcp_ok()) {
            vga_write("netcfg: dhcp ok  ip ");
            for (int i = 0; i < 4; i++) { print_int(net_our_ip[i]); if (i < 3) vga_write("."); }
            vga_write("  gw ");
            for (int i = 0; i < 4; i++) { print_int(net_gw_ip[i]); if (i < 3) vga_write("."); }
            vga_write("  dns ");
            const uint8_t *d = net_dns_server_get();
            for (int i = 0; i < 4; i++) { print_int(d[i]); if (i < 3) vga_write("."); }
            vga_write("\n");
        } else {
            vga_write(trs("netcfg: no DHCP reply (QEMU static defaults kept)\n", "netcfg: 未收到 DHCP 应答（保留 QEMU 静态默认值）\n"));
        }
        return;
    }
    if (!*arg) {
        vga_write("ip  : ");
        for (int i = 0; i < 4; i++) { print_int(net_our_ip[i]); if (i < 3) vga_write("."); }
        vga_write("  gw: ");
        for (int i = 0; i < 4; i++) { print_int(net_gw_ip[i]); if (i < 3) vga_write("."); }
        vga_write("  dns: ");
        const uint8_t *d = net_dns_server_get();
        for (int i = 0; i < 4; i++) { print_int(d[i]); if (i < 3) vga_write("."); }
        vga_write(net_dhcp_ok() ? "  [dhcp]\n" : "  [static]\n");
        vga_write(trs("usage: netcfg IP GATEWAY [DNS]", "用法：netcfg IP GATEWAY [DNS]"));
        vga_write("   e.g. netcfg 192.168.152.8 192.168.152.1 192.168.152.2\n");
        vga_write("       netcfg dhcp   (auto-configure via DHCP)\n");
        return;
    }
    uint8_t ip[4], gw[4], dns[4];
    char *w[4];
    int n = 0;
    w[n++] = arg;
    for (char *q = arg; *q && n < 4; q++)
        if (*q == ' ') { *q = 0; while (q[1] == ' ') q++; w[n++] = q + 1; }
    if (n < 2 || !ip4_parse_(w[0], ip) || !ip4_parse_(w[1], gw)) {
        vga_write(trs("netcfg: bad arguments\n", "netcfg: 参数错误\n"));
        return;
    }
    for (int i = 0; i < 4; i++) net_our_ip[i] = ip[i];
    for (int i = 0; i < 4; i++) net_gw_ip[i] = gw[i];
    if (n >= 3 && ip4_parse_(w[2], dns)) net_dns_server_set(dns);
    net_arp_flush();
    net_cfg_user_set();                    /* v2.66: static wins over DHCP */
    vga_write("netcfg: ip/gw/dns updated, ARP cache cleared\n");
}

/* ---------- hwinfo: full hardware-properties dump ---------- */
static void cmd_hwinfo(void) {
    static char buf[3072];
    int n = hw_summary(buf, sizeof(buf));
    if (n <= 0) { vga_write(trs("hwinfo: nothing\n", "hwinfo：无信息\n")); return; }
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    if (lang_get() != LANG_ZH) { vga_write(buf); return; }
    static const struct { const char *en, *zh; } HZ[] = {
        {"== CatOS hardware properties ==", "== CatOS 硬件属性 =="},
        {"cpu.brand :", "CPU 型号   :"}, {"cpu.vendor:", "CPU 厂商   :"},
        {"cpu.flags :", "CPU 特性   :"}, {"cpu.mhz   :", "CPU 频率   :"},
        {"  cores ", "  核心数 "}, {"mem.total :", "内存总量   :"},
        {"mem.frames: used ", "内存帧   ：已用 "}, {" / free ", " / 空闲 "},
        {" (4K pages)", "（4K 页）"}, {" KB  kernel ", " KB  内核 "},
        {"disk.model:", "磁盘型号   :"}, {"disk.s/n  :", "磁盘序列号 :"},
        {" MB  lba48 ", " MB  LBA48 "}, {"  fw ", "  固件 "},
        {"nic.model :", "网卡型号   :"}, {"  mac ", "  MAC "}, {"  ip ", "  IP "},
        {"  sig fam ", "  签名 系列 "}, {" mod ", " 型号 "}, {" step ", " 步进 "}
    };
    static char out[4096];
    int o = 0;
    const char *p = buf;
    while (*p && o < (int)sizeof(out) - 2) {
        int hit = -1;
        for (unsigned i = 0; i < sizeof(HZ)/sizeof(HZ[0]); i++) {
            int k = 0;
            while (HZ[i].en[k] && p[k] == HZ[i].en[k]) k++;
            if (HZ[i].en[k] == 0) { hit = (int)i; break; }
        }
        if (hit >= 0) {
            for (const char *q = HZ[hit].zh; *q && o < (int)sizeof(out) - 2; q++) out[o++] = *q;
            p += slen_(HZ[hit].en);
        } else out[o++] = *p++;
    }
    out[o] = 0;
    vga_write(out);
}

/* ---------- top: task monitor with live CPU% ---------- */
static void top_draw(uint32_t win_ticks) {
    struct toprow {
        int tid, uid, ring, state;
        uint32_t dt, pages;
        char name[16];
    } rows[MAX_TASKS];
    int nr = 0;
    uint32_t idle_now = task_idle_ticks();

    static uint32_t prev_ticks[MAX_TASKS], prev_idle;
    uint32_t now[MAX_TASKS];

    for (int slot = 0; slot < MAX_TASKS; slot++) {
        int tid; uint32_t ck, pg; int st, uid, ring; char nm[16];
        now[slot] = 0;
        if (!task_stat(slot, &tid, &ck, &pg, &st, &uid, &ring, nm, sizeof(nm)))
            continue;
        now[slot] = ck;
        rows[nr].tid = tid; rows[nr].uid = uid; rows[nr].ring = ring;
        rows[nr].state = st; rows[nr].pages = pg;
        rows[nr].dt = (ck >= prev_ticks[slot]) ? ck - prev_ticks[slot] : 0;
        {
            int i;
            for (i = 0; i < 15 && nm[i]; i++) rows[nr].name[i] = nm[i];
            rows[nr].name[i] = 0;
        }
        nr++;
    }

    /* insertion sort by delta desc */
    for (int i = 1; i < nr; i++) {
        struct toprow r = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].dt < r.dt) { rows[j + 1] = rows[j]; j--; }
        rows[j + 1] = r;
    }

    vga_clear();
    vga_write("CatOS top - ");
    print_int((int)(task_uptime_ms() / 1000));
    vga_write("s up, ");
    print_int(hw_num(HW_CPU_MHZ));
    vga_write(" MHz, tasks ");
    print_int(hw_num(HW_TASKS));
    vga_write("\nmem ");
    print_int(hw_num(HW_MEM_USED_KB));
    vga_write("K used / ");
    print_int(hw_num(HW_MEM_FREE_KB));
    vga_write("K free / ");
    print_int(hw_num(HW_MEM_TOTAL_KB));
    vga_write("K total\n");

    if (win_ticks) {
        uint32_t idle_d = (idle_now >= prev_idle) ? idle_now - prev_idle : 0;
        int busy = 100 - (int)(idle_d * 100 / win_ticks);
        if (busy < 0) busy = 0;
        if (busy > 100) busy = 100;
        vga_write("cpu: ");
        print_int(busy);
        vga_write("% busy (");
        print_int((int)idle_d);
        vga_write("/");
        print_int((int)win_ticks);
        vga_write(" idle ticks)\n");
    } else {
        vga_write("cpu: (sampling, next frame in 2s)\n");
    }
    vga_write("TID  UID   RING  STATE   CPU%  PAGES  NAME\n");

    static const char *stn[] = {
        "UNUSED", "READY ", "RUN   ", "SLEEP ", "KB    ", "CHILD ", "ZOMBIE",
    };
    for (int i = 0; i < nr; i++) {
        print_int(rows[i].tid); vga_write(rows[i].tid < 10 ? "    " : "   ");
        print_int(rows[i].uid); vga_write(rows[i].uid < 10 ? "    " : "   ");
        vga_write(rows[i].ring == 0 ? "0    " : "3    ");
        vga_write(stn[rows[i].state <= 6 ? rows[i].state : 0]);
        vga_write(" ");
        if (win_ticks) {
            int pct = (int)(rows[i].dt * 100 / win_ticks);
            if (pct > 100) pct = 100;
            print_pad(pct, 3);
        } else {
            vga_write("  .");
        }
        vga_write("%  ");
        print_pad((int)rows[i].pages, 4);
        vga_write("  ");
        vga_write(rows[i].name);
        vga_write("\n");
    }
    vga_write("-- any key quits, refresh 2s --\n");

    for (int slot = 0; slot < MAX_TASKS; slot++) prev_ticks[slot] = now[slot];
    prev_idle = idle_now;
}

static void cmd_top(void) {
    top_draw(0);                             /* first frame: totals only */
    for (;;) {
        /* wait 2 s (20 ticks) or until a REAL key arrives; stale break
         * codes left in the queue by the launching readline are decoded
         * away and never count as the quit key */
        int k = kb_getkey_timed(20);
        if (k >= 0) break;
        top_draw(20);                        /* 20-tick sampling window */
    }
    vga_clear();
}

/* ---------- #59: jobs / log -- background job output ---------- */
static const char *job_state_name(int st) {
    switch (st) {
    case T_READY:      return "ready";
    case T_RUNNING:    return "run";
    case T_SLEEPING:   return "sleep";
    case T_WAIT_KB:    return "waitkey";
    case T_WAIT_CHILD: return "wait";
    case T_ZOMBIE:     return "done";
    case T_STOPPED:    return "stopped";
    default:           return "?";
    }
}

/* v2.52: parse an optional leading '%' then a TID.  Returns tid >= 0 when
 * the user named one, -1 when the argument is empty (caller falls back to
 * the most recent background job), -2 on garbage. */
static int parse_job_tid(const char *arg) {
    while (*arg == ' ') arg++;
    if (*arg == '%') arg++;
    if (*arg == 0) return -1;
    if (*arg < '0' || *arg > '9') return -2;
    int tid = 0;
    while (*arg >= '0' && *arg <= '9') tid = tid * 10 + (*arg++ - '0');
    return tid;
}

/* resolve the fg/bg/stop target: explicit TID, else the most recent '&'
 * job.  Rejects the shell itself (0) -- signaling the shell from its own
 * prompt would stop the shell. */
static task_t *job_lookup(char *arg, const char *who) {
    int tid = parse_job_tid(arg);
    if (tid == -1) tid = g_last_bg_tid;
    if (tid < 0) {
        vga_write(who); vga_write(trs(": no current job\n", ": 没有当前任务\n"));
        return 0;
    }
    task_t *t = task_by_tid(tid);
    if (!t || t->state == T_UNUSED || t == task_current()) {
        vga_write(who); vga_write(trs(": no such job\n", ": 找不到任务\n"));
        return 0;
    }
    if (t->parent != task_current()) {
        vga_write(who); vga_write(trs(": not your job\n", "：不属于你的任务\n"));
        return 0;
    }
    if (t->state == T_ZOMBIE) {
        vga_write(who); vga_write(": job already exited\n");
        return 0;
    }
    return t;
}

/* fg [%]TID : resume a stopped/background job in the foreground and wait
 *             for it to exit or stop again.
 * bg [%]TID : resume a stopped job in the background.
 * stop [%]TID: suspend a running job (shell-side SIGTSTP). */
static void cmd_fg(char *arg) {
    task_t *t = job_lookup(arg, "fg");
    if (!t) return;
    task_signal(t->tid, SIGCONT);
    t->bg_mode = 0;                 /* foreground: output goes to the console */
    int stopped = task_wait_child_or_stop(t);
    vga_write(stopped ? "\n[stopped] " : "\n[done] ");
    print_int(t->tid); vga_write(" "); vga_write(t->name); vga_write("\n");
}

static void cmd_bg(char *arg) {
    task_t *t = job_lookup(arg, "bg");
    if (!t) return;
    if (t->state != T_STOPPED) {
        vga_write(trs("bg: job is not stopped\n", "bg: 任务未停止\n"));
        return;
    }
    task_signal(t->tid, SIGCONT);
    t->bg_mode = 1;                 /* background: capture stdout to the ring */
    vga_write("[bg tid "); print_int(t->tid); vga_write("] ");
    vga_write(t->name); vga_write("\n");
}

static void cmd_stop(char *arg) {
    task_t *t = job_lookup(arg, "stop");
    if (!t) return;
    if (!t->bg_mode && t->state != T_STOPPED) {
        vga_write(trs("stop: not a background job\n", "stop: 不是后台任务\n"));
        return;
    }
    int r = task_signal(t->tid, SIGTSTP);
    if (r == -3) vga_write("stop: job is blocked on I/O; try again later\n");
    else if (r != 0) vga_write(trs("stop: signal failed\n", "stop: signal 失败\n"));
}

/* daemon CMD [args...] : spawn a detached service (v2.54).  Like a
 * background job it returns to the shell at once and captures stdout to its
 * log ring, but it is flagged as a daemon: it has no controlling interactive
 * session and is listed with a `D` marker in `jobs`.  The event loop of a
 * daemon sleeps on the existing timer/child/key wake sources; no POSIX
 * session model or signal table is introduced here. */
static void cmd_daemon(char *arg) {
    char *argv[16];
    int argc = split_words(arg, argv, 16);
    if (argc < 1) {
        vga_write(trs("usage: daemon CMD [args...]\n", "用法：daemon CMD [args...]\n"));
        return;
    }
    if (run_from_path(argv[0], argv, argc, 1, g_uid) != 0) {
        vga_write(trs("daemon: unknown command: ", "daemon: 未知命令: ")); vga_write(argv[0]); vga_write("\n");
        return;
    }
    task_t *t = task_by_tid(g_last_bg_tid);
    if (t) t->daemon = 1;
    vga_write("[daemon tid "); print_int(g_last_bg_tid);
    vga_write("] "); vga_write(argv[0]); vga_write("\n");
}

static void cmd_jobs(void) {
    int tid, state, uid, ring, found = 0;
    uint32_t cpu, pages;
    char nm[16];
    vga_write("TID  STATE    NAME             LOG\n");
    for (int slot = 1; slot < MAX_TASKS; slot++) {
        if (!task_stat(slot, &tid, &cpu, &pages, &state, &uid, &ring,
                       nm, sizeof(nm))) continue;
        task_t *t = task_by_tid(tid);
        /* background jobs, plus stopped foreground jobs held by job control */
        if (!t || (!t->bg_mode && t->state != T_STOPPED)) continue;
        found = 1;
        vga_write("  "); print_int(tid);
        vga_write("   "); vga_write(job_state_name(state));
        vga_write(t->daemon ? " D  " : "    ");
        vga_write(nm);
        vga_write("  "); print_int(task_log_pending(t));
        vga_write("B\n");
    }
    if (!found) vga_write(tr(L_JOBS_NONE));
}

/* log TID      : dump everything the job has printed since the last drain
 * log -f TID   : keep following until the job exits (like tail -f)
 * Both drain the per-task ring, so a second `log TID` shows only new text. */
static void cmd_log(char *arg) {
    while (*arg == ' ') arg++;
    int follow = 0;
    if (arg[0] == '-' && arg[1] == 'f' && (arg[2] == ' ' || arg[2] == 0)) {
        follow = 1; arg += 2;
        while (*arg == ' ') arg++;
    }
    if (*arg < '0' || *arg > '9') {
        vga_write(trs("usage: log [-f] TID   (see `jobs` for TIDs)\n", "用法：log [-f] TID   (see `jobs` for TIDs)\n"));
        return;
    }
    int tid = 0;
    while (*arg >= '0' && *arg <= '9') tid = tid * 10 + (*arg++ - '0');
    task_t *t = task_by_tid(tid);
    if (!t || t->state == T_UNUSED) { vga_write(tr(L_LOG_NOJOB)); return; }
    if (!t->bg_mode) {
        vga_write("log: task "); print_int(tid);
        vga_write(trs(" is not a background job (its output went to the console)\n", " is 不是后台任务 (its output went to the console)\n"));
        return;
    }
    char buf[161];
    int idle = 0, printed = 0;
    for (;;) {
        uint32_t lost = 0;
        int n = task_log_drain(t, buf, 160, &lost);
        if (n > 0) {
            vga_write_n(buf, n, 1);          /* keeps UTF-8 state across chunks */
            printed += n;
            idle = 0;
            continue;
        }
        if (lost) {
            vga_write("\n[log: "); print_int(lost);
            vga_write(" bytes dropped (ring overflow)]\n");
            lost = 0;
        }
        if (!follow) break;
        t = task_by_tid(tid);
        if (!t || t->state == T_UNUSED || t->state == T_ZOMBIE) break;
        if (idle++ > 600) break;             /* ~60 s safety stop */
        task_sleep_ticks(1);                 /* 100 ms */
    }
    if (printed) vga_putc('\n');
}
extern uint8_t net_mac[6];
extern int net_ready;
extern int netdbg_tx_calls, netdbg_tx_last, netdbg_rx_frames, netdbg_io_base;
extern int netdbg_io_width;
extern int      netdbg_pci_count;
extern uint16_t netdbg_pci_vend[8];
extern uint16_t netdbg_pci_dev[8];
extern uint16_t netdbg_csr0;
extern uint32_t netdbg_tx_word0, netdbg_rx_word0;
extern uint32_t netdbg_desc_tx0(void), netdbg_desc_rx0(void);
extern uint32_t netdbg_init_addr(void), netdbg_init_rxring(void), netdbg_init_txring(void);
extern uint16_t netdbg_csr(int i);
extern int net_fail_code;
extern int net_env_vmware(void);
static void cmd_netdiag(const char *arg) {
    (void)arg;
    vga_write("PCI bus 0: ");
    if (netdbg_pci_count == 0) vga_write(trs("no devices\n", "无设备\n"));
    for (int i = 0; i < netdbg_pci_count; i++) {
        vga_write("vend="); print_hex(netdbg_pci_vend[i]);
        vga_write(" dev="); print_hex(netdbg_pci_dev[i]);
        if (i < netdbg_pci_count - 1) vga_write("  ");
    }
    vga_write("\n");
    vga_write("net_ready="); print_int(net_ready); vga_write("  io_base=0x");
    print_hex((uint32_t)netdbg_io_base);
    vga_write("  width="); print_int(netdbg_io_width); vga_write("-bit\n");
    vga_write("backend="); vga_write(net_backend_name());
    if (e1000_up()) {
        vga_write("  e1000 up="); print_int(e1000_up());
        vga_write("  tx="); print_int(e1000_info(E1000_INFO_TX_FRAMES));
        vga_write("  rx="); print_int(e1000_info(E1000_INFO_RX_FRAMES));
        vga_write("  irq="); print_int(e1000_irq_line());
        vga_write("\n");
    } else {
        vga_write("\n");
    }
    vga_write("fail="); print_int(net_fail_code);
    vga_write(" (0=ok 1=no-dev 2=bus-FF 3=mac 4=init)  env=");
    vga_write(net_env_vmware() ? "VMware" : "QEMU/PC");
    vga_write("  mac_state=");
    if (net_mac[0]==0xFF && net_mac[1]==0xFF && net_mac[2]==0xFF &&
        net_mac[3]==0xFF && net_mac[4]==0xFF && net_mac[5]==0xFF)
        vga_write("OPEN-BUS");
    else vga_write("valid");
    vga_write("\n");
    vga_write("csr0=0x"); print_hex(netdbg_csr0); vga_write("\n");
    vga_write("mac="); print_hex((net_mac[0]<<8)|net_mac[1]); vga_write(":");
    print_hex((net_mac[2]<<8)|net_mac[3]); vga_write(":");
    print_hex((net_mac[4]<<8)|net_mac[5]); vga_write("\n");
    vga_write("tx_calls="); print_int(netdbg_tx_calls);
    vga_write(" tx_last="); print_int(netdbg_tx_last);
    vga_write(" rx_frames="); print_int(netdbg_rx_frames); vga_write("\n");
    vga_write("tx_word0=0x"); print_hex(netdbg_tx_word0);
    vga_write(" rx_word0=0x"); print_hex(netdbg_rx_word0); vga_write("\n");
    vga_write("desc_tx0=0x"); print_hex(netdbg_desc_tx0());
    vga_write(" desc_rx0=0x"); print_hex(netdbg_desc_rx0()); vga_write("\n");
    vga_write("init@0x"); print_hex(netdbg_init_addr());
    vga_write(" rxr=0x"); print_hex(netdbg_init_rxring());
    vga_write(" txr=0x"); print_hex(netdbg_init_txring()); vga_write("\n");
    vga_write("XMTRC="); print_int(netdbg_csr(40));
    vga_write(" CXDA=0x"); print_hex(netdbg_csr(34) | ((uint32_t)netdbg_csr(35) << 16));
    vga_write(" RL/TL="); print_int(netdbg_csr(76)); vga_write("/"); print_int(netdbg_csr(78)); vga_write("\n");
    vga_write("csr0_now=0x"); print_hex(netdbg_csr(0));
    vga_write(" csr15(mode)="); print_hex(netdbg_csr(15));
    vga_write(" POLL="); print_int(netdbg_csr(46)); vga_write("\n");
    vga_write("IADR=0x"); print_hex(netdbg_csr(1) | ((uint32_t)netdbg_csr(2) << 16));
    vga_write(" csr6="); print_hex(netdbg_csr(6));
    vga_write(" XMTRC="); print_int(netdbg_csr(74)); vga_write("\n");
    vga_write("init_rcvrl="); print_int(netdbg_init_rcvrl);
    vga_write(" init_xmtrl="); print_int(netdbg_init_xmtrl); vga_write("\n");
}

/* explain why the network is down, appended to "network not ready" */
static void net_why_down(void) {
    extern int netdbg_pci_count;
    extern uint16_t netdbg_pci_vend[8];
    extern uint16_t netdbg_pci_dev[8];
    extern int net_fail_code;
    extern int net_env_vmware(void);
    if (netdbg_pci_count == 0) {
        vga_write(" (no PCI devices at all)");
    } else {
        switch (net_fail_code) {
        case 1: vga_write(" (no PCnet 1022:2000 on any bus)"); break;
        case 2: vga_write(" (NIC registers read open-bus FF/00)"); break;
        case 3: vga_write(" (MAC unreadable: device not answering)"); break;
        case 4: vga_write(" (INIT/START did not latch)"); break;
        default: vga_write(" (driver not initialised)"); break;
        }
    }
    if (net_env_vmware())
        vga_write("; VMware guest -> set the VM network adapter to Intel E1000\n");
    else
        vga_write("; QEMU -> -device e1000,netdev=n0 or -device pcnet,netdev=n0\n");
}

/* ---------- curl: one-shot HTTP GET ---------- */
static void cmd_curl(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write(trs("usage: curl [http://|https://]HOST[:port][/path]\n", "用法：curl [http://|https://]HOST[:port][/路径]\n"));
        vga_write(trs("  the scheme is optional; http and https (TLS 1.2) are supported\n", "  协议头可省略；支持 http 与 https（TLS 1.2）\n"));
        vga_write("e.g.   curl example.com/      curl https://10.0.2.2:8443/   curl 10.0.2.2:8080/API.txt\n");
        return;
    }
    if (!net_up()) { vga_write(trs("curl: network not ready", "curl: 网络未就绪")); net_why_down(); return; }
    static char buf[8192];
    int n = net_http_get(arg, buf, sizeof(buf));
    if (n < 0) {
        vga_write(trs("curl: error ", "curl: 错误 "));
        print_int(n);
        vga_write(" (1=NIC 2=host 3=connect 4=http 5=resp 6=send 7=https 8=redirects)\n");
        int st = net_http_last_status();
        const char *lo = net_http_last_location();
        if (st) {
            vga_write(trs("curl: server answered HTTP ", "curl: 服务器返回 HTTP "));
            print_int(st);
            vga_write("\n");
        }
        if (lo && *lo) {
            vga_write(trs("curl: Location: ", "curl: 跳转到 "));
            vga_write(lo);
            vga_write("\n");
            if (n == -7)
                vga_write(trs("curl: redirected to https -- cross-scheme https redirects are not supported yet; request that https URL directly\n", "curl: 该跳转指向 https —— 暂不支持跨协议跳转到 https，请直接请求该 https 地址\n"));
        }
        return;
    }
    vga_write("--- ");
    int st = net_http_last_status();
    vga_write(arg);
    if (st) { vga_write(" (HTTP "); print_int(st); vga_write(")"); }
    vga_write(" ---\n");
    vga_write(buf);
    if (n > 0 && buf[n-1] != '\n') vga_write("\n");
    vga_write("--- "); print_int(n); vga_write(" bytes ---\n");
}

/* ---------- ping: ICMP echo ---------- */
static void cmd_ping(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write(trs("usage: ping IP_or_host\n", "用法：ping IP_or_host\n"));
        return;
    }
    if (!net_up()) { vga_write(trs("ping: network not ready", "ping: 网络未就绪")); net_why_down(); return; }
    for (int i = 0; i < 3; i++) {
        int ok = net_ping(arg);
        vga_write(ok == 0 ? "reply  " : "no-reply ");
        vga_write(arg);
        vga_write("\n");
        if (ok != 0) break;
        for (volatile long d = 0; d < 800000L; d++) { }  /* small gap between tries */
    }
}

/* ---------- v2.71 sshd: SSH-2 server on port 22 ---------------------- */

/* console capture for remote command execution: mirrors every character
 * the builtin prints (same mechanism as the `>` redirect tee) with \n
 * promoted to \r\n for the wire. */
#define SSH_CAP_BUF 8192
static char     ssh_cap_buf[SSH_CAP_BUF];
static uint32_t ssh_cap_len;
static int      ssh_cap_on;

static void ssh_cap_char(char c) {
    if (!ssh_cap_on) return;
    if (c == '\n') {
        if (ssh_cap_len < SSH_CAP_BUF - 1) ssh_cap_buf[ssh_cap_len++] = '\r';
    }
    if (ssh_cap_len < SSH_CAP_BUF - 1) ssh_cap_buf[ssh_cap_len++] = c;
}

/* Run one line as if typed at the prompt; capture builtin output into
 * `out` (NUL-terminated, \r\n line endings), report the exit status. */
int shell_ssh_exec_line(const char *line, char *out, int outmax, int *rc) {
    static char buf[256];
    int i;
    for (i = 0; line[i] && i < 255; i++) buf[i] = line[i];
    buf[i] = 0;
    env_expand(buf, (int)sizeof(buf));

    /* split first token (same convention as the main loop) */
    char *cmd = buf;
    while (*cmd == ' ') cmd++;
    char *arg = cmd;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }
    if (!*cmd) { if (rc) *rc = 0; if (out) out[0] = 0; return 0; }

    ssh_cap_len = 0;
    ssh_cap_on = 1;
    int saved_script = g_script_mode;
    g_script_mode = 1;                       /* pager streams, no -- More -- */
    vga_set_tee(ssh_cap_char);
    shell_exec(cmd, arg);
    vga_set_tee(0);
    g_script_mode = saved_script;
    ssh_cap_on = 0;
    ssh_cap_buf[ssh_cap_len] = 0;

    int n = (int)ssh_cap_len;
    if (n > outmax - 1) n = outmax - 1;
    for (i = 0; i < n; i++) out[i] = ssh_cap_buf[i];
    out[n] = 0;
    if (rc) *rc = g_last_rc;
    return 0;
}

static void cmd_sshd(char *arg) {
    while (*arg == ' ') arg++;
    /* v2.72: -d spawns the detached ring0 daemon (survives the shell,
     * also used by the /etc/autoexec "@sshd" boot line) */
    if (arg[0] == '-' && arg[1] == 'd') {
        int tid = task_spawn_r0(sshd_daemon_main, "sshd", 1);
        if (tid > 0) {
            vga_write("[daemon tid "); print_int(tid);
            vga_write("] sshd (waits for the NIC, then :22)\n");
        } else {
            vga_write(trs("sshd: no free task slot\n", "sshd: 没有空闲任务槽\n"));
        }
        return;
    }
    if (!net_up()) {
        vga_write(trs("sshd: network not ready", "sshd: 网络未就绪"));
        net_why_down();
        return;
    }
    net_dhcp_ensure();
    net_tcp_listen(22);
    vga_write("sshd: listening on port 22 (ESC stops)\n");
    vga_write("sshd: try from the host:\n");
    vga_write("  ssh -p 2222 -o StrictHostKeyChecking=no \\\n");
    vga_write("       -o KexAlgorithms=curve25519-sha256 \\\n");
    vga_write("       -o HostKeyAlgorithms=ssh-ed25519 -o Ciphers=aes128-ctr \\\n");
    vga_write("       -o MACs=hmac-sha2-256 user@localhost\n");
    int sessions = 0;
    for (;;) {
        if (net_tcp_saccept() == 0) {
            vga_write("sshd: session opened\n");
            ssh_serve_session();
            sessions++;
            vga_write("sshd: session closed\n");
            continue;
        }
        /* idle cycle: ESC stops the server.  kb_getkey() would BLOCK
         * forever on a stray modifier/break scancode (kb_next_scancode
         * waits for a "real key"), so use the bounded timed variant. */
        if (kb_peek()) {
            int k = kb_getkey_timed(2);
            if (k == 0x1B) break;            /* ESC */
        }
    }
    net_tcp_sclose();
    vga_write("sshd: stopped, ");
    print_int(sessions);
    vga_write(" session(s) served\n");
}

/* ---------- dns: resolve a hostname ---------- */
static void cmd_dns(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { vga_write(trs("usage: dns hostname\n", "用法：dns hostname\n")); return; }
    if (!net_up()) { vga_write(trs("dns: network not ready", "dns: 网络未就绪")); net_why_down(); return; }
    uint8_t ip[4];
    if (net_dns_lookup(arg, ip) != 0) {
        vga_write(trs("dns: could not resolve '", "dns: 无法解析 '"));
        vga_write(arg);
        vga_write("'\n");
        return;
    }
    vga_write(arg);
    vga_write(" -> ");
    char t[4];
    for (int i = 0; i < 4; i++) {
        int d = ip[i], ti = 0;
        if (d == 0) { vga_putc('0'); }
        while (d > 0) { t[ti++] = (char)('0' + d % 10); d /= 10; }
        while (ti > 0) vga_putc(t[--ti]);
        if (i < 3) vga_putc('.');
    }
    vga_write("\n");
}

/* ---------- udp: send a datagram, then wait for one reply ----------
 * works with a host echo server, e.g.
 *   python -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('',9999));
 *              while 1: d,a=s.recvfrom(2048); s.sendto(d,a)"
 */
static void cmd_udp(const char *arg) {
    while (*arg == ' ') arg++;
    char ip[64];
    int pi = 0;
    while (*arg && *arg != ' ' && pi < 63) ip[pi++] = *arg++;
    ip[pi] = 0;
    while (*arg == ' ') arg++;
    int port = 0;
    while (*arg >= '0' && *arg <= '9') { port = port * 10 + (*arg - '0'); arg++; }
    while (*arg == ' ') arg++;
    if (!*ip || port <= 0 || port > 65535 || !*arg) {
        vga_write(trs("usage: udp IP port text\n", "用法：udp IP port text\n"));
        return;
    }
    if (!net_up()) { vga_write(trs("udp: network not ready", "udp: 网络未就绪")); net_why_down(); return; }
    uint32_t tlen = 0;
    while (arg[tlen]) tlen++;
    if (net_udp_send(ip, (uint16_t)port, (const uint8_t *)arg, tlen) != 0) {
        vga_write(trs("udp: send failed\n", "udp: send 失败\n"));
        return;
    }
    vga_write("sent, waiting for reply...\n");
    static char rbuf[2048];
    char rip[24];
    int rport = 0;
    /* poll the NIC while waiting (the old loop never did -> "no reply") */
    extern volatile uint32_t timer_ticks;
    extern void net_poll(void);
    uint32_t deadline = timer_ticks + 200;
    for (;;) {
        net_poll();
        int n = net_udp_recv(rip, &rport, (uint8_t *)rbuf, sizeof(rbuf) - 1);
        if (n > 0) {
            vga_write("reply from ");
            vga_write(rip);
            vga_write(":");
            print_int(rport);
            vga_write(" (");
            print_int(n);
            vga_write(" bytes): ");
            rbuf[n] = 0;
            vga_write(rbuf);
            vga_write("\n");
            return;
        }
        if ((int32_t)(timer_ticks - deadline) >= 0) break;
        task_maybe_yield();
        task_sleep_ticks(1);
    }
    vga_write(trs("udp: no reply\n", "udp: 无应答\n"));
}

static void cmd_format(void) {    vga_write(tr(L_FORMAT_CONFIRM));
    vga_write("Disk: ");
    uint32_t tot = 0;
    ata_identify(&tot);
    if (tot == 0) tot = 131072;
    print_int((int)(tot / 2048));            /* MiB */
    vga_write(" MiB IDE, FAT32, 512B/sector ");
    uint32_t cs = fat_cluster_size();
    print_int((int)(cs ? cs / 512 : 8));
    vga_write(" sectors/cluster.\n");
    vga_write("Proceed? (y/n) ");
    char ans[16];
    kb_readline(ans, sizeof(ans));
    if (ans[0] == 'y' || ans[0] == 'Y') {
        if (fs_format() != 0) {
            vga_write(trs("format: no IDE disk attached\n", "format: 没有连接 IDE 磁盘\n"));
            return;
        }
        vga_write(tr(L_FORMAT_DONE));
        vga_write("Top-level dirs: /dev /etc /bin /sbin /lib /usr /home\n");
        vga_write("                /root /tmp /var /proc /sys /mnt /boot\n");
        ce_install_builtins();      /* put /bin apps + /appdevelop docs on disk */
        ce_install_pkg_repo();      /* NIGHT-P4: seed /pkg/ local repo after format */
    } else {
        vga_write(tr(L_FORMAT_CANCEL));
    }
}

/* ---------- nano-style full-screen editor ---------- */
#define ED_MAX_LINES 64
#define ED_MAX_COLS  99          /* widest terminal (VBE) is 100 columns */
#define ED_MAX_VIEW  60

static char ed_lines[ED_MAX_LINES][ED_MAX_COLS + 1];
static int  ed_nlines;
static int  ed_modified;
static int  ed_top;              /* first visible content line */
static int  ed_cx, ed_cy;        /* cursor column / content line */
static char ed_fname[FS_NAME_MAX];

static int ed_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* v2.9: the editor follows the ACTIVE back-end's grid instead of being
 * hard-wired to 80x25.  Row 0 is the title, row con_rows()-1 the bars. */
static int ed_view_rows(void) {
    int r = con_rows() - 2;
    if (r < 1) r = 1;
    if (r > ED_MAX_VIEW) r = ED_MAX_VIEW;
    return r;
}
static int ed_max_cols(void) {
    int c = con_cols() - 1;
    if (c < 1) c = 1;
    if (c > ED_MAX_COLS) c = ED_MAX_COLS;
    return c;
}

static void ed_status_msg(const char *m) {
    int row = con_rows() - 1;
    if (row < 1) row = 1;
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_SELECTION), 0);
    for (int x = 0; x < con_cols(); x++) vga_putc_at((uint8_t)x, (uint8_t)row, ' ');
    vga_write_at_utf8(0, (uint8_t)row, m);
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
}

static void ed_load(fs_entry_t *f) {
    ed_nlines = 0;
    int n = fs_line_count(f);
    char lb[ED_MAX_COLS + 2];
    for (int i = 0; i < n && i < ED_MAX_LINES; i++) {
        if (fs_get_line(f, i, lb, sizeof(lb)) == 0) {
            int l = ed_strlen(lb);
            if (l > ED_MAX_COLS) l = ED_MAX_COLS;
            for (int j = 0; j < l; j++) ed_lines[ed_nlines][j] = lb[j];
            ed_lines[ed_nlines][l] = 0;
            ed_nlines++;
        }
    }
    if (ed_nlines == 0) { ed_lines[0][0] = 0; ed_nlines = 1; }
    ed_modified = 0;
    ed_top = 0; ed_cx = 0; ed_cy = 0;
}

static int ed_save(fs_entry_t *f) {
    /* #182: re-check write permission at save time -- fs_save is a ring0
     * write and must not hand a protected path back to the session. */
    if (fs_perm_check(ed_fname, g_uid, g_gid, PERM_WRITE) != 0) return -1;
    int total = 0;
    for (int i = 0; i < ed_nlines; i++) total += ed_strlen(ed_lines[i]) + 1;
    if (total > FS_DATA_MAX) return -1;
    fs_clear_file(f);
    for (int i = 0; i < ed_nlines; i++)
        if (fs_append_line(f, ed_lines[i]) != 0) return -1;
    if (fs_save(f) != 0) return -1;    /* write back to the FAT32 volume */
    ed_modified = 0;
    return 0;
}

static void ed_redraw(void) {
    int sc = con_cols();
    int last = con_rows() - 1;
    if (last < 2) last = 2;
    int view = ed_view_rows();

    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
    for (int y = 0; y <= last; y++)
        for (int x = 0; x < sc; x++)
            vga_putc_at((uint8_t)x, (uint8_t)y, ' ');

    /* title row */
    con_set_style_rgb(pal_rgb(PAL_BG), pal_rgb(PAL_SELECTION), 0);
    int sx = 0;
    vga_write_at_utf8((uint8_t)sx, 0, "CatOS nano  "); sx += 12;
    vga_write_at_utf8((uint8_t)sx, 0, ed_fname); sx += ed_strlen(ed_fname);
    vga_write_at_utf8((uint8_t)sx, 0, "  "); sx += 2;
    char tmp[24];
    itoa(ed_nlines, tmp, 0);
    vga_write_at_utf8((uint8_t)sx, 0, tmp); sx += ed_strlen(tmp);
    vga_write_at_utf8((uint8_t)sx, 0, " lines"); sx += 6;
    if (ed_modified) vga_write_at_utf8((uint8_t)sx, 0, "  [modified]");

    /* content rows (UTF-8 aware, so Chinese file contents display) */
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
    for (int r = 0; r < view && r + 1 < last; r++) {
        int ln = ed_top + r;
        if (ln >= ed_nlines) break;
        vga_write_at_utf8(0, (uint8_t)(1 + r), ed_lines[ln]);
    }

    /* bottom bar */
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_SELECTION), 0);
    vga_write_at_utf8(0, (uint8_t)last,
        "^G Help  ^O Save  ^X Exit  ^K Del Line  ^U Clear  F2 IME");
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);

    int scr_y = 1 + (ed_cy - ed_top);
    if (scr_y < 1) scr_y = 1;
    if (scr_y > last - 1) scr_y = last - 1;
    /* v2.58: active IME composition follows the editor cursor inline. */
    const char *comp = ime_composition();
    int comp_w = 0;
    if (comp && *comp) {
        int px = disp_width_n(ed_lines[ed_cy], ed_cx);
        vga_write_at_utf8((uint8_t)px, (uint8_t)scr_y, comp);
        comp_w = disp_width(comp);
    }
    int cur_col = ed_cx + comp_w;
    vga_set_cursor((uint8_t)cur_col, (uint8_t)scr_y);
}

static void ed_insert(char c) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    int lim = ed_max_cols();
    if (l >= lim) { ed_status_msg("Line is full for this terminal width"); return; }
    for (int i = l; i > ed_cx; i--) ln[i] = ln[i - 1];
    ln[ed_cx] = c;
    ln[l + 1] = 0;
    ed_cx++;
}

/* Insert a whole UTF-8 sequence (from the pinyin IME) as one unit. */
static void ed_insert_utf8(const char *s) {
    while (*s) ed_insert(*s++);
}

static void ed_backspace(void) {
    char *ln = ed_lines[ed_cy];
    if (ed_cx > 0) {
        int l = ed_strlen(ln);
        for (int i = ed_cx - 1; i < l; i++) ln[i] = ln[i + 1];
        ed_cx--;
    } else if (ed_cy > 0) {
        char *prev = ed_lines[ed_cy - 1];
        int pl = ed_strlen(prev);
        char *cur = ed_lines[ed_cy];
        int cl = ed_strlen(cur);
        if (pl + cl > ED_MAX_COLS) cl = ED_MAX_COLS - pl;
        for (int i = 0; i < cl; i++) prev[pl + i] = cur[i];
        prev[pl + cl] = 0;
        for (int i = ed_cy; i < ed_nlines - 1; i++)
            for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
        ed_nlines--;
        ed_cy--;
        ed_cx = pl;
    }
}

static void ed_delete(void) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    if (ed_cx < l) {
        for (int i = ed_cx; i < l; i++) ln[i] = ln[i + 1];
    } else if (ed_cy < ed_nlines - 1) {
        char *next = ed_lines[ed_cy + 1];
        int nl = ed_strlen(next);
        if (l + nl > ED_MAX_COLS) nl = ED_MAX_COLS - l;
        for (int i = 0; i < nl; i++) ln[l + i] = next[i];
        ln[l + nl] = 0;
        for (int i = ed_cy + 1; i < ed_nlines - 1; i++)
            for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
        ed_nlines--;
    }
}

static void ed_enter(void) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    if (ed_nlines >= ED_MAX_LINES) { ed_status_msg("Too many lines (max 64)"); return; }
    for (int i = ed_nlines; i > ed_cy + 1; i--)
        for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i - 1][j];
    int start = ed_cx;
    if (start > l) start = l;
    char *newln = ed_lines[ed_cy + 1];
    int nl = 0;
    for (int i = start; i < l; i++) {
        newln[nl++] = ln[i];
        if (nl > ED_MAX_COLS) break;
    }
    newln[nl] = 0;
    ln[start] = 0;
    ed_nlines++;
    ed_cy++;
    ed_cx = 0;
}

static void ed_del_line(void) {
    for (int i = ed_cy; i < ed_nlines - 1; i++)
        for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
    ed_nlines--;
    if (ed_nlines == 0) { ed_lines[0][0] = 0; ed_nlines = 1; }
    if (ed_cy > ed_nlines - 1) ed_cy = ed_nlines - 1;
    if (ed_cx > ed_strlen(ed_lines[ed_cy])) ed_cx = ed_strlen(ed_lines[ed_cy]);
}

static void cmd_edit(const char *arg) {
    if (!*arg) {
        vga_write(trs("usage: edit [-c|--cat] [+N|-N] FILE\n", "用法：edit [-c|--cat] [+N|-N] 文件\n"));
        return;
    }
    /* v2.68: cat/head/tail merged into edit.  -c prints the whole file,
     * -c N prints the first N lines, -c -N the last N lines. */
    {
        char ebuf[FS_PATH_MAX + 64];
        int k = 0;
        while (arg[k] && k < (int)sizeof(ebuf) - 1) { ebuf[k] = arg[k]; k++; }
        ebuf[k] = 0;
        char *tok[4]; int nt = 0;
        char *q = ebuf;
        while (*q && nt < 4) {
            while (*q == ' ') q++;
            if (!*q) break;
            tok[nt++] = q;
            while (*q && *q != ' ') q++;
            if (*q) *q++ = 0;
        }
        int catmode = 0, nlines = 0;
        const char *path = 0;
        for (int t = 0; t < nt; t++) {
            if (!strcmp_(tok[t], "-c") || !strcmp_(tok[t], "--cat")) catmode = 1;
            else if (tok[t][0] == '+' || (tok[t][0] == '-' && tok[t][1]) ||
                     (tok[t][0] >= '0' && tok[t][0] <= '9')) nlines = atoi_(tok[t]);
            else path = tok[t];
        }
        if (catmode) {
            if (!path) { vga_write(trs("edit -c: missing FILE\n", "edit -c：缺少文件名\n")); return; }
            if (nlines != 0) {
                char sub[FS_PATH_MAX + 32];
                int o = 0;
                const char *h = "-n ";
                while (*h && o < (int)sizeof(sub) - 1) sub[o++] = *h++;
                o += ls_u2a((uint32_t)(nlines > 0 ? nlines : -nlines), sub + o);
                sub[o++] = ' ';
                for (const char *z = path; *z && o < (int)sizeof(sub) - 1; z++) sub[o++] = *z;
                sub[o] = 0;
                if (nlines > 0) cmd_head(sub); else cmd_tail(sub);
            } else cmd_cat(path);
            return;
        }
    }
    /* refuse files exceeding the editor buffer: loading used
     * to silently truncate them and ^O then overwrote the file */
    uint32_t cl = 0, sz = 0;
    int existed = fs_resolve_file(arg, &cl, &sz) == 0;
    if (existed && sz >= FS_DATA_MAX) {
        vga_write(trs("edit: file too large for the editor;", "edit: 文件 过大 for the editor;"));
        vga_write(trs(" refusing (a save would truncate it)\n", " 拒绝 (a save would truncate it)\n"));
        return;
    }
    /* #182: gate open/create on write permission.  An existing file needs
     * PERM_WRITE on the entry itself; a new one needs write on its parent
     * directory (the same rule touch/mkdir/cp apply). */
    if (existed) {
        if (fs_perm_check(arg, g_uid, g_gid, PERM_WRITE) != 0) {
            vga_write(trs("edit: permission denied\n", "edit: 权限不足\n")); return;
        }
    } else {
        char parent[FS_PATH_MAX];
        if (perm_write_on_parent(arg, parent, sizeof(parent)) == -13) {
            vga_write(trs("edit: permission denied\n", "edit: 权限不足\n")); return;
        }
    }
    fs_entry_t *f = fs_create_file(arg);
    if (!f) {
        con_set_fg_rgb(pal_rgb(PAL_ERROR));
        vga_write(trs("edit: cannot create/open file\n", "edit: 无法create/打开 文件\n"));
        con_set_fg_rgb(pal_rgb(PAL_FG));
        return;
    }

    int fi = 0;
    while (arg[fi] && fi < FS_NAME_MAX - 1) { ed_fname[fi] = arg[fi]; fi++; }
    ed_fname[fi] = 0;

    ed_load(f);
    ed_redraw();

    for (;;) {
        int k = kb_getkey();

        /* v2.9: F2 pinyin IME also works inside the editor */
        {
            char han[IME_OUT_MAX];
            int ir = ime_feed(k, han);
            if (ir == IME_CONSUME) { ed_redraw(); ime_refresh(); continue; }
            if (ir == IME_COMMIT) {
                ed_insert_utf8(han);
                ed_modified = 1;
                ed_redraw(); ime_refresh();
                continue;
            }
        }

        if (k == 0x18) {                    /* ^X exit */
            int do_exit = 1;
            if (ed_modified) {
                ed_status_msg("Save?  ^O=yes  ^Y=no  any other=cancel");
                for (;;) {
                    k = kb_getkey();
                    if (k == 0x0F) { ed_save(f); break; }   /* ^O */
                    if (k == 0x19) { break; }                /* ^Y */
                    ed_redraw();
                    do_exit = 0;
                    break;
                }
                if (!do_exit) continue;
            }
            con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
            ime_off();
            vga_clear();
            vga_write("edit: saved ");
            print_int(fs_file_size(f));
            vga_write(" bytes.\n");
            return;
        }
        if (k == 0x0F || k == 0x13) {       /* ^O / ^S save */
            if (ed_save(f) != 0)
                ed_status_msg("SAVE FAILED: exceeds editor buffer limit");
            else
                ed_status_msg("Saved.");
            continue;
        }
        if (k == 0x07) {                    /* ^G help */
            ed_status_msg("^O/^S save  ^X exit  ^K del line  ^U clear  arrows move  Del delete");
            continue;
        }
        if (k == 0x0B) { ed_del_line(); ed_modified = 1; ed_redraw(); continue; }  /* ^K */
        if (k == 0x15) {                    /* ^U clear all */
            ed_nlines = 1; ed_lines[0][0] = 0;
            ed_cy = 0; ed_cx = 0; ed_top = 0; ed_modified = 1; ed_redraw(); continue;
        }
        if (k == KB_UP)          { if (ed_cy > 0) ed_cy--; }
        else if (k == KB_DOWN)   { if (ed_cy < ed_nlines - 1) ed_cy++; }
        else if (k == KB_LEFT)   { if (ed_cx > 0) ed_cx--; }
        else if (k == KB_RIGHT)  { int l = ed_strlen(ed_lines[ed_cy]); if (ed_cx < l) ed_cx++; }
        else if (k == KB_HOME)   { ed_cx = 0; }
        else if (k == KB_END)    { ed_cx = ed_strlen(ed_lines[ed_cy]); }
        else if (k == KB_PGUP)   { ed_cy -= ed_view_rows(); if (ed_cy < 0) ed_cy = 0; }
        else if (k == KB_PGDN)   { ed_cy += ed_view_rows(); if (ed_cy > ed_nlines - 1) ed_cy = ed_nlines - 1; }
        else if (k == '\b')      { ed_backspace(); ed_modified = 1; }
        else if (k == KB_DEL)    { ed_delete(); ed_modified = 1; }
        else if (k == '\n')      { ed_enter(); ed_modified = 1; }
        else if (k >= 32 && k < 256) { ed_insert((char)k); ed_modified = 1; }

        int ll = ed_strlen(ed_lines[ed_cy]);
        if (ed_cx > ll) ed_cx = ll;
        if (ed_cx > ed_max_cols()) ed_cx = ed_max_cols();
        if (ed_cy < ed_top) ed_top = ed_cy;
        if (ed_cy >= ed_top + ed_view_rows())
            ed_top = ed_cy - ed_view_rows() + 1;
        ed_redraw();
        ime_refresh();
    }
}

/* ---------- session user & capability commands (v2.13) --------------------
 * The session starts as root (the initial user).  `login`/`su` switch it;
 * `useradd`/`userdel`/`passwd`/`chcaps` edit the persistent /etc/passwd;
 * `chmod`/`chown` edit the persistent /etc/acl.  Everything here runs in
 * ring0 and talks to user.c directly -- no new syscall slots. */
static int atoi_(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static uint32_t hex32_(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * 16u + (uint32_t)d;
        s++;
    }
    return v;
}
static uint32_t octal32_(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '7') { v = v * 8u + (uint32_t)(*s++ - '0'); }
    return v;
}
/* resolve a possibly-relative path against the cwd (fs_pwd gives cwd) */
static void abs_path_(const char *in, char *out, int outlen) {
    if (!in || !*in || in[0] == '/') {
        int i = 0;
        for (; in && in[i] && i < outlen - 1; i++) out[i] = in[i];
        out[i] = 0;
        return;
    }
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    int o = 0;
    for (int i = 0; pwd[i] && o < outlen - 1; i++) out[o++] = pwd[i];
    if (o > 0 && out[o - 1] != '/' && o < outlen - 1) out[o++] = '/';
    for (int i = 0; in[i] && o < outlen - 1; i++) out[o++] = in[i];
    out[o] = 0;
}

static int      g_uid  = UID_ROOT;
static int      g_gid  = 0;
static uint32_t g_caps = CAP_ALL;

static void session_apply(void) {
    user_rec_t *u = user_by_uid(g_uid);
    if (u) { g_gid = u->gid; g_caps = u->caps; }
}

static void print_caps(uint32_t caps) {
    if (caps == CAP_ALL) { vga_write("all"); return; }
    static const struct { uint32_t bit; const char *nm; } T[] = {
        { CAP_NET, "net" },      { CAP_KILL, "kill" },
        { CAP_FSADMIN, "fsadmin" }, { CAP_AUDIO, "audio" },
        { CAP_USERADM, "useradm" }, { CAP_DEVICE, "device" },
        { CAP_SYS, "sys" },
        { CAP_FORMAT, "format" }, { CAP_FSCK, "fsck" },
        { CAP_RAWIO, "rawio" },   { CAP_HWINFO, "hwinfo" },
        { CAP_REBOOT, "reboot" }, { CAP_DIAG, "diag" },
    };
    int first = 1;
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (caps & T[i].bit) {
            if (!first) vga_write(",");
            vga_write(T[i].nm);
            first = 0;
        }
    if (first) vga_write("none");
}

static int session_is_admin(void) {
    return g_uid == UID_ROOT || (g_caps & CAP_USERADM);
}
static int session_is_fsadmin(void) {
    return g_uid == UID_ROOT || (g_caps & CAP_FSADMIN);
}

/* v2.59: render the full-screen crash report on demand.
 *   bsod [--no-reboot]   --no-reboot: return to the shell after a keypress
 *                           (test hook); otherwise reboot/halt per the
 *                           cached /etc/panicmode setting. */
static void cmd_bsod(char *arg) {
    int soft = 0;
    while (*arg == ' ') arg++;
    if (!strcmp_(arg, "--no-reboot") || !strcmp_(arg, "--soft")) soft = 1;
    if (!soft && g_uid != UID_ROOT && !(g_caps & (CAP_SYS | CAP_REBOOT))) {
        vga_write(trs("bsod: permission denied (need CAP_SYS or CAP_REBOOT)\n", "bsod: 权限不足 (need CAP_SYS or CAP_REBOOT)\n"));
        return;
    }
    bsod_enter("manual BSOD triggered by `bsod` command", 0, soft);
}

/* v2.59: read / set the post-crash action, persisted to /etc/panicmode.
 *   panicmode            -> show current setting
 *   panicmode reboot|halt */
/* v2.59-crashscreen: read the fixed bss panic record saved on the last
 * crash.  No FS read: the record lives in kernel bss from the crash path.
 *   crashlog          -> symbolized summary (EIP/frames resolved by the
 *                         same built-in symbol table the BSOD uses)
 *   crashlog --full   -> raw fields, with a sensitive-data policy note */
static void cmd_crashlog(char *arg) {
    while (*arg == ' ') arg++;
    const panic_record_t *r = panic_record_get();
    if (!r || r->magic != PANIC_RECORD_MAGIC) {
        vga_write(trs("crashlog: no crash record\n", "crashlog: 没有崩溃记录\n"));
        return;
    }
    int full = !strcmp_(arg, "--full") || !strcmp_(arg, "-f");

    vga_write("crashlog: saved panic record\n");
    vga_write("version: "); vga_write(r->version); vga_write("\n");
    vga_write("tick=");
    { static const char *h = "0123456789abcdef";
      vga_write("0x");
      for (int i = 7; i >= 0; i--) vga_putc(h[(r->ticks >> (i * 4)) & 0xF]); }
    vga_write(" vector=");
    { static const char *h = "0123456789abcdef";
      vga_write("0x");
      for (int i = 7; i >= 0; i--) vga_putc(h[(r->intno >> (i * 4)) & 0xF]); }
    vga_write("\n");

    if (full) {
        vga_write("[sensitive-data note: raw fields may contain pointers / "
                  "stack residue; share with care]\n");
        vga_write("EIP=");
        { static const char *h = "0123456789abcdef";
          vga_write("0x");
          for (int i = 7; i >= 0; i--) vga_putc(h[(r->eip >> (i * 4)) & 0xF]); }
        vga_write(" CR2=");
        { static const char *h = "0123456789abcdef";
          vga_write("0x");
          for (int i = 7; i >= 0; i--) vga_putc(h[(r->cr2 >> (i * 4)) & 0xF]); }
        vga_write(" ERR=");
        { static const char *h = "0123456789abcdef";
          vga_write("0x");
          for (int i = 7; i >= 0; i--) vga_putc(h[(r->err >> (i * 4)) & 0xF]); }
        vga_write("\nEFLAGS=");
        { static const char *h = "0123456789abcdef";
          vga_write("0x");
          for (int i = 7; i >= 0; i--) vga_putc(h[(r->eflags >> (i * 4)) & 0xF]); }
        vga_write(" ESP=");
        { static const char *h = "0123456789abcdef";
          vga_write("0x");
          for (int i = 7; i >= 0; i--) vga_putc(h[(r->esp_k >> (i * 4)) & 0xF]); }
        vga_write("\n");
        for (int i = 0; i < r->nframes; i++) {
            vga_write("frame#");
            { char b[4]; b[0] = (char)('0' + i); b[1] = 0; vga_write(b); }
            vga_write("=");
            { static const char *h = "0123456789abcdef";
              vga_write("0x");
              for (int d = 7; d >= 0; d--) vga_putc(h[(r->frames[i] >> (d * 4)) & 0xF]); }
            vga_write("\n");
        }
    } else {
        /* summary: EIP resolved to nearest symbol+offset via the same
         * static .rodata symbol table the BSOD uses. */
        char sym[44];
        bsod_sym_fmt(sym, sizeof(sym), r->eip);
        vga_write("EIP="); vga_write(sym);
        vga_write(" CR2=");
        { static const char *h = "0123456789abcdef";
          vga_write("0x");
          for (int i = 7; i >= 0; i--) vga_putc(h[(r->cr2 >> (i * 4)) & 0xF]); }
        vga_write("\nreason: "); vga_write(r->msg); vga_write("\n");
        for (int i = 0; i < r->nframes && i < 3; i++) {
            char fsym[44];
            bsod_sym_fmt(fsym, sizeof(fsym), r->frames[i]);
            vga_write("  #"); vga_write(fsym); vga_write("\n");
        }
    }
}

static void cmd_panicmode(char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        int m = bsod_mode();
        vga_write("panicmode: ");
        vga_write(m == BSOD_MODE_HALT ? "halt" :
                  m == BSOD_MODE_WAIT ? "wait" : "reboot");
        vga_write("\n");
        return;
    }
    if (g_uid != UID_ROOT && !(g_caps & (CAP_SYS | CAP_REBOOT))) {
        vga_write(trs("panicmode: permission denied (need CAP_SYS or CAP_REBOOT)\n", "panicmode: 权限不足 (need CAP_SYS or CAP_REBOOT)\n"));
        return;
    }
    if (!strcmp_(arg, "halt")) {
        bsod_set_mode(BSOD_MODE_HALT);
        hive_set_sz("HKLM\\System\\Crash\\Mode", "halt"); hive_save();
        fs_write_text("/etc/panicmode", "halt\n", 5);   /* legacy mirror */
    } else if (!strcmp_(arg, "wait")) {
        bsod_set_wait();
        hive_set_sz("HKLM\\System\\Crash\\Mode", "wait"); hive_save();
        fs_write_text("/etc/panicmode", "wait\n", 5);
    } else if (!strcmp_(arg, "reboot")) {
        bsod_set_mode(BSOD_MODE_REBOOT);
        hive_set_sz("HKLM\\System\\Crash\\Mode", "reboot"); hive_save();
        fs_write_text("/etc/panicmode", "reboot\n", 7);
    } else {
        vga_write(trs("panicmode: usage: panicmode [reboot|halt|wait] [reboot:Ns]\n", "panicmode: 用法：panicmode [reboot|halt|wait] [reboot:Ns]\n"));
        return;
    }
    vga_write("panicmode: ");
    vga_write(arg);
    vga_write(" (persisted to /etc/panicmode)\n");
}

static void cmd_whoami(void) {
    user_rec_t *u = user_by_uid(g_uid);
    vga_write(u ? u->name : "?");
    vga_write("\n");
}

static void cmd_id(const char *name) {
    user_rec_t *u = (name && *name) ? user_by_name(name) : user_by_uid(g_uid);
    if (!u) { vga_write(trs("id: no such user\n", "id: 找不到用户\n")); return; }
    vga_write("uid=");    print_int(u->uid);
    vga_write(" gid=");   print_int(u->gid);
    vga_write(" name=");  vga_write(u->name);
    vga_write(" home=");  vga_write(u->home);
    vga_write("\ncaps=0x"); print_hex(u->caps);
    vga_write(" ("); print_caps(u->caps); vga_write(")\n");
    vga_write("pw="); vga_write(user_pw_is_set(u) ? "set" : "none");
    vga_write(" shell="); vga_write(u->shell); vga_write("\n");
}

static void print_hex8(uint32_t v) {
    char b[9];
    for (int i = 7; i >= 0; i--) { b[i] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    b[8] = 0;
    vga_write(b);
}

static void cmd_lsuser(void) {
    vga_write("UID  GID  PW   CAPS      NAME\n");
    for (int i = 0; i < MAX_USERS; i++) {
        user_rec_t *u = user_at(i);
        if (!u || !u->used) continue;
        print_pad(u->uid, 4); vga_write(" ");
        print_pad(u->gid, 4); vga_write(" ");
        vga_write(user_pw_is_set(u) ? "yes  " : "no   ");
        print_hex8(u->caps);
        vga_write("  ");
        vga_write(u->name);
        vga_write("\n");
    }
}

static void cmd_caps_legend(void) {
    vga_write(trs("capability bits (per-user, /etc/passwd):\n", "能力位（每用户，/etc/passwd）：\n"));
    vga_write(trs("  0x0001 net      network: http/dns/udp/ping/ip\n", "  0x0001 net      网络：http/dns/udp/ping/ip\n"));
    vga_write(trs("  0x0002 kill     kill tasks not owned by self\n", "  0x0002 kill     终止不属于自己的任务\n"));
    vga_write(trs("  0x0004 fsadmin  format / fsck\n", "  0x0004 fsadmin  格式化 / 文件系统检查\n"));
    vga_write(trs("  0x0008 audio    SB16 PCM playback\n", "  0x0008 audio    SB16 PCM 播放\n"));
    vga_write(trs("  0x0010 useradm  useradd/userdel/passwd/chcaps/su\n", "  0x0010 useradm  用户管理命令\n"));
    vga_write(trs("  0x0020 device   raw device + low-level hw info\n", "  0x0020 设备   原始设备 + 底层硬件信息\n"));
    vga_write(trs("  0x0040 sys      reserved\n", "  0x0040 sys      保留\n"));
    vga_write(trs("  fine-grained (v2.25c splits of fsadmin/device):\n", "  细粒度（从 fsadmin/设备 拆分）：\n"));
    vga_write("  0x0080 format   0x0100 fsck    0x0200 rawio\n");
    vga_write("  0x0400 hwinfo   0x0800 reboot  0x1000 diag\n");
    vga_write("session: uid="); print_int(g_uid);
    vga_write(" caps=0x"); print_hex(g_caps);
    vga_write(" ("); print_caps(g_caps); vga_write(")\n");
}

/* login USER [PW]  /  su [USER [PW]] */
static void cmd_login(char *arg, int as_su) {
    char *argv[3];
    int argc = split_words(arg, argv, 3);
    const char *name = argc >= 1 ? argv[0] : "root";
    const char *pw   = argc >= 2 ? argv[1] : 0;
    user_rec_t *u = user_by_name(name);
    if (!u) { vga_write(trs("login: no such user\n", "login: 找不到用户\n")); return; }
    if (u->uid == g_uid) { vga_write("already session user\n"); return; }
    int need_pw = user_pw_is_set(u);
    if (as_su && g_uid == UID_ROOT) need_pw = 0;      /* root may su freely */
    /* T52: when no password was given on the command line, ask for it
     * interactively and echo '*' per keystroke instead of the characters. */
    char typed[64];
    if (need_pw && !pw) {
        vga_write(tr(L_PASSWD_PROMPT));
        kb_readline_masked(typed, sizeof(typed));
        pw = typed;
    }
    if (need_pw && user_check_pw(u, pw) != 0) {
        vga_write("login: incorrect password\n");
        return;
    }
    g_uid = u->uid;
    session_apply();
    vga_write("logged in as "); vga_write(u->name);
    vga_write(" (uid "); print_int(g_uid);
    vga_write(")\n");
}

/* useradd NAME [UID [GID [CAPS_HEX]]]
 * With no CAPS_HEX argument the capability set is built interactively: one
 * yes/no prompt per capability bit (T52).  `useradd NAME UID GID CAPS` keeps
 * working for scripts and for the regression suite. */
/* v2.53: groupadd G [MEMBER...] -- create /etc/group entry, append members */
static void cmd_groupadd(char *arg) {
    if (!session_is_admin()) { vga_write(trs("groupadd: permission denied\n", "groupadd: 权限不足\n")); return; }
    /* #186: count tokens before split_words mutates the line; words beyond
     * the argv capacity used to be silently dropped. */
    int words = 0;
    for (const char *q = arg; *q; ) {
        while (*q == ' ') q++;
        if (!*q) break;
        words++;
        while (*q && *q != ' ') q++;
    }
    char *argv[12];
    int argc = split_words(arg, argv, 12);
    if (argc < 1) {
        vga_write(trs("usage: groupadd GROUP [MEMBER USER...]\n", "用法：groupadd GROUP [MEMBER USER...]\n"));
        return;
    }
    if (words > argc)
        vga_write(trs("groupadd: too many arguments; trailing members ignored\n", "groupadd: 过多 参数; trailing members ignored\n"));
    int nmem = argc - 1;
    if (nmem > GROUP_MEMBERS) {
        vga_write("groupadd: only "); print_int(GROUP_MEMBERS);
        vga_write(" members supported; extra members ignored\n");
        nmem = GROUP_MEMBERS;
    }
    /* #186: validate members BEFORE touching the group table, so an unknown
     * user cannot leave an empty group slot that group_save would persist. */
    for (int i = 1; i < argc; i++) {
        if (!user_by_name(argv[i])) {
            vga_write(trs("groupadd: unknown user '", "groupadd: 未知 user '")); vga_write(argv[i]);
            vga_write("'\n");
            return;
        }
    }
    /* remember whether the group already existed, so a rollback below does
     * not wipe an existing entry's members. */
    int existed = 0;
    char gnm[16];
    for (int j = 0; group_info(j, gnm, 16, 0) >= 0; j++)
        if (!strcmp_(gnm, argv[0])) { existed = 1; break; }
    int gid = group_add(argv[0], 0);
    if (gid == -2) { vga_write("groupadd: group table full\n"); return; }
    if (gid < 0)   { vga_write(trs("groupadd: failed\n", "groupadd: 失败\n")); return; }
    int failed = 0;
    for (int i = 1; i <= nmem; i++) {
        int r = group_add(argv[0], argv[i]);
        if (r == -4) { vga_write("groupadd: member list full\n"); failed = 1; break; }
        if (r < 0)   { vga_write(trs("groupadd: failed to add member\n", "groupadd: 失败 to add member\n")); failed = 1; break; }
    }
    if (failed) {
        if (!existed) group_remove(argv[0]);   /* roll back the fresh slot */
        return;
    }
    if (group_save() != 0) { vga_write(trs("groupadd: WARNING: could not persist\n", "groupadd: 警告： could not persist\n")); return; }
    vga_write("group '"); vga_write(argv[0]); vga_write("' gid ");
    print_int(gid); vga_write("\n");
}

/* v2.53: groups -- list groups with member names */
static void cmd_groups(void) {
    char nm[16];
    int i = 0, found = 0;
    for (;;) {
        int nmem = group_info(i, nm, sizeof(nm), 0);
        if (nmem < 0) break;
        found = 1;
        vga_write(nm);
        int gid = 0;
        group_info(i, nm, sizeof(nm), &gid);
        vga_write(" (gid "); print_int(gid); vga_write("): ");
        /* members: re-read names from the db through user records */
        int printed = 0;
        for (int ui = 0; ui < MAX_USERS; ui++) {
            user_rec_t *u = user_at(ui);
            if (!u || !u->used) continue;
            if (user_in_group(u->uid, gid)) {
                if (printed) vga_write(" ");
                vga_write(u->name);
                printed++;
            }
        }
        vga_write("\n");
        i++;
    }
    if (!found) vga_write("no groups\n");
}

static void cmd_useradd(char *arg) {
    if (!session_is_admin()) { vga_write(trs("useradd: permission denied\n", "useradd: 权限不足\n")); return; }
    char *argv[5];
    int argc = split_words(arg, argv, 5);
    static char ua_name[32];
    if (argc < 1) {
        /* v2.68: interactive by default -- ask username, then the
         * existing usercaps wizard below. */
        vga_write(trs("username: ", "用户名："));
        kb_readline(ua_name, sizeof(ua_name));
        if (!*ua_name) { vga_write(trs("useradd: cancelled\n", "useradd：已取消\n")); return; }
        argv[0] = ua_name;
        argc = 1;
    }
    const char *name = argv[0];
    int uid  = argc >= 2 ? atoi_(argv[1]) : 101;
    int gid  = argc >= 3 ? atoi_(argv[2]) : uid;
    uint32_t caps;
    if (argc >= 4) {
        caps = hex32_(argv[3]);
    } else {
        /* T52 wizard, extended v2.25: presets first, then per-bit
         * questions including the fine-grained successors. */
        static const struct { uint32_t bit; const char *q; } W[] = {
            { CAP_NET,     "network (http/dns/udp/ping)" },
            { CAP_KILL,    "kill other users' tasks" },
            { CAP_FSADMIN, "filesystem admin (format+fsck, broad)" },
            { CAP_FORMAT,  "format disks (fine)" },
            { CAP_FSCK,    "fsck mirror repair (fine)" },
            { CAP_AUDIO,   "audio playback (SB16/ES1371)" },
            { CAP_USERADM, "user admin (useradd/passwd/su)" },
            { CAP_DEVICE,  "raw device access (broad)" },
            { CAP_RAWIO,   "raw device I/O (fine)" },
            { CAP_HWINFO,  "hardware info read (fine)" },
            { CAP_SYS,     "system (broad: reboot+introspection)" },
            { CAP_REBOOT,  "reboot (fine)" },
            { CAP_DIAG,    "kernel introspection/faultinfo (fine)" },
        };
        caps = 0;
        vga_write("Capabilities for '"); vga_write(name);
        vga_write("'\n  preset? admin/operator/developer/viewer/guest or ENTER for wizard: ");
        char pans[16];
        kb_readline(pans, sizeof(pans));
        if (pans[0] && user_preset_caps(pans, &caps) == 0) {
            vga_write("     preset '"); vga_write(pans);
            vga_write("' -> caps=0x"); print_hex(caps); vga_write("\n");
        } else {
        vga_write("(y/N per item)\n");
        char ans[8];
        for (int i = 0; i < (int)(sizeof(W) / sizeof(W[0])); i++) {
            char num[4]; num[0] = (char)('1' + i); num[1] = 0;
            vga_write("  "); vga_write(num); vga_write(". allow ");
            vga_write(W[i].q); vga_write("? [y/N] ");
            kb_readline(ans, sizeof(ans));
            if (ans[0] == 'y' || ans[0] == 'Y') {
                caps |= W[i].bit;
                vga_write("     -> granted\n");
            } else {
                vga_write(trs("     -> denied\n", "     -> 已拒绝\n"));
            }
        }
        vga_write("       caps=0x"); print_hex(caps); vga_write("\n");
        }
    }
    /* #181: newly created accounts are subject to the same subset rule:
     * a non-root issuer cannot mint an account stronger than themselves. */
    if (g_uid != UID_ROOT && (caps & ~g_caps) != 0) {
        vga_write(trs("useradd: cannot grant capabilities you do not hold\n", "useradd: 无法grant capabilities you do not hold\n"));
        return;
    }
    char home[48];
    int o = 0;
    for (const char *q = "/home/"; *q && o < 47; q++) home[o++] = *q;
    for (const char *q = name; *q && o < 47; q++) home[o++] = *q;
    home[o] = 0;
    int r = user_add(name, uid, gid, home, "/bin/shell.ce", caps);
    if (r == -5) { vga_write(trs("useradd: name or uid already exists\n", "useradd: 用户名或 uid 已存在\n")); return; }
    if (r != 0)  { vga_write(trs("useradd: failed (table full?)\n", "useradd: 失败 (table full?)\n")); return; }
    if (user_save() != 0) vga_write(trs("useradd: WARNING: could not persist /etc/passwd\n", "useradd: 警告： could not persist /etc/passwd\n"));
    else vga_write("user added (persisted to /etc/passwd)\n");
}

static void cmd_userdel(char *arg) {
    if (!session_is_admin()) { vga_write(trs("userdel: permission denied\n", "userdel: 权限不足\n")); return; }
    char *argv[2];
    if (split_words(arg, argv, 2) < 1) { vga_write(trs("usage: userdel NAME\n", "用法：userdel NAME\n")); return; }
    if (!strcmp_(argv[0], "root")) { vga_write("userdel: root is undeletable\n"); return; }
    if (atoi_(argv[0]) == 0 && argv[0][0] >= '0' && argv[0][0] <= '9') {
        /* numeric arg: refuse deleting root by number too */
    }
    int r = user_del(argv[0]);
    if (r == -1) { vga_write(trs("userdel: no such user\n", "userdel: 找不到用户\n")); return; }
    if (user_save() != 0) vga_write(trs("userdel: WARNING: could not persist\n", "userdel: 警告： could not persist\n"));
    else vga_write("user deleted\n");
}

static void cmd_passwd(char *arg) {
    char *argv[3];
    int argc = split_words(arg, argv, 3);
    const char *name = argc >= 1 ? argv[0] : (user_by_uid(g_uid) ? user_by_uid(g_uid)->name : 0);
    if (!name) { vga_write(trs("usage: passwd [NAME] [NEWPW]\n", "用法：passwd [NAME] [NEWPW]\n")); return; }
    user_rec_t *u = user_by_name(name);
    if (!u) { vga_write(trs("passwd: no such user\n", "passwd: 找不到用户\n")); return; }
    if (u->uid != g_uid && !session_is_admin()) {
        vga_write(trs("passwd: permission denied\n", "passwd: 权限不足\n"));
        return;
    }
    const char *pw;
    char p1[64], p2[64];
    if (argc >= 2) {
        pw = argv[1];                        /* non-interactive form */
    } else {
        /* T52: interactive, no echo -- '*' per keystroke, retype check */
        vga_write("New password for "); vga_write(u->name); vga_write(": ");
        kb_readline_masked(p1, sizeof(p1));
        vga_write("Retype new password: ");
        kb_readline_masked(p2, sizeof(p2));
        if (!p1[0]) { vga_write(trs("passwd: empty password rejected\n", "passwd: 空 password rejected\n")); return; }
        if (strcmp_(p1, p2) != 0) { vga_write(tr(L_PASSWD_MISMATCH)); return; }
        pw = p1;
    }
    user_set_pw(name, pw);
    if (user_save() != 0) vga_write(trs("passwd: WARNING: could not persist\n", "passwd: 警告： could not persist\n"));
    else vga_write("password set\n");
}

static void cmd_chcaps(char *arg) {
    if (!session_is_admin()) { vga_write(trs("chcaps: permission denied\n", "chcaps: 权限不足\n")); return; }
    char *argv[3];
    if (split_words(arg, argv, 3) < 2) { vga_write(trs("usage: chcaps CAPS_HEX NAME\n", "用法：chcaps CAPS_HEX NAME\n")); return; }
    uint32_t want = hex32_(argv[0]);
    /* #181: a non-root issuer may only confer bits they already hold, so
     * a CAP_USERADM account cannot self/other-grant CAP_ALL (CAP_KILL/
     * DEVICE/SYS/REBOOT ...).  Root is exempt. */
    if (g_uid != UID_ROOT && (want & ~g_caps) != 0) {
        vga_write(trs("chcaps: cannot grant capabilities you do not hold\n", "chcaps: 无法grant capabilities you do not hold\n"));
        return;
    }
    if (user_set_caps(argv[1], want) != 0) {
        vga_write(trs("chcaps: no such user\n", "chcaps: 找不到用户\n"));
        return;
    }
    if (user_save() != 0) vga_write(trs("chcaps: WARNING: could not persist\n", "chcaps: 警告： could not persist\n"));
    else vga_write("capabilities updated\n");
    user_rec_t *tu = user_by_name(argv[1]);
    if (tu && g_uid == tu->uid) session_apply();
}

/* chmod OCTAL PATH   (owner or root) */
static void cmd_chmod(char *arg) {
    char *argv[3];
    if (split_words(arg, argv, 3) < 2) { vga_write(trs("usage: chmod OCTAL PATH\n", "用法：chmod OCTAL PATH\n")); return; }
    int mode = (int)octal32_(argv[0]);
    char path[FS_PATH_MAX];
    abs_path_(argv[1], path, sizeof(path));
    /* #98: acl_lookup() leaves its out-params untouched on a miss.  All
     * three MUST be initialised here -- the old code passed uninitialised
     * fgid/fmode straight into acl_set() and persisted stack garbage. */
    int fuid = UID_ROOT, fgid = 0, fmode = 0644;
    if (acl_lookup(path, &fuid, &fgid, &fmode) != 0) {
        /* default entry: root-owned 0644 -- only root may chmod it */
        fuid = UID_ROOT; fgid = 0; fmode = 0644;
    }
    if (g_uid != UID_ROOT && g_uid != fuid) {
        vga_write(trs("chmod: not the owner\n", "chmod: 不是所有者\n"));
        return;
    }
    if (acl_set(path, fuid, fgid, mode) != 0) {
        vga_write("chmod: acl table full\n");
        return;
    }
    if (acl_save() != 0) vga_write(trs("chmod: WARNING: could not persist\n", "chmod: 警告： could not persist\n"));
    else vga_write("mode updated\n");
}

/* chown UID[:GID] PATH   (root or CAP_FSADMIN) */
static void cmd_chown(char *arg) {
    if (!session_is_fsadmin()) { vga_write(trs("chown: permission denied\n", "chown: 权限不足\n")); return; }
    char *argv[3];
    if (split_words(arg, argv, 3) < 2) { vga_write(trs("usage: chown UID[:GID] PATH\n", "用法：chown UID[:GID] PATH\n")); return; }
    int uid = 0, gid = -1;
    const char *spec = argv[0];
    int i = 0;
    for (; spec[i] && spec[i] != ':'; i++) ;
    if (spec[i] == ':') {
        char nb[12]; int j = 0;
        for (int k = 0; k < i && j < 11; k++) nb[j++] = spec[k];
        nb[j] = 0; uid = atoi_(nb);
        j = 0;
        for (int k = i + 1; spec[k] && j < 11; k++) nb[j++] = spec[k];
        nb[j] = 0;
        gid = spec[i + 1] ? atoi_(nb) : -1;
    } else uid = atoi_(spec);
    char path[FS_PATH_MAX];
    abs_path_(argv[1], path, sizeof(path));
    int fuid, fgid, fmode;
    if (acl_lookup(path, &fuid, &fgid, &fmode) != 0) { fuid = UID_ROOT; fgid = 0; fmode = 0644; }
    if (gid < 0) gid = fgid;
    if (acl_set(path, uid, gid, fmode) != 0) { vga_write("chown: acl table full\n"); return; }
    if (acl_save() != 0) vga_write(trs("chown: WARNING: could not persist\n", "chown: 警告： could not persist\n"));
    else vga_write("owner updated\n");
}

/* ---------- file utilities: cat / rm / cp / mv (v2.14) ------------------
 * Imported from the retired "ot" (Ocean's Tools) user-mode tool REPL:
 * every one of these existed only because the kernel shell lacked them.
 * The kernel side is the right home (consistent permissions, no user-space
 * re-implementation of fs internals).  ot keeps its unique value:
 * csc scripting, fmgr, hexedit, grep, vcs, alias, banner, cecheck. */

#define CAT_MAX_PRINT 65536u      /* cat: stop printing after 64 KiB */

/* v2.59: shared pager interface for line-oriented file dumps.  Splits the
 * buffered read on newlines and feeds each line through pager_line(); the
 * pager itself streams straight through in non-interactive (redirect / pipe
 * / bg / script) contexts, so no extra decision lives here. */
static int pager_dump_text(const char *text, uint32_t len) {
    uint32_t i = 0;
    static char line[FS_PATH_MAX + 32];
    int ln = 0;
    while (i < len) {
        char ch = text[i++];
        if (ch == '\n') {
            line[ln] = 0;
            if (!pager_line(line)) return 0;
            ln = 0;
            continue;
        }
        if (ch == '\r') continue;
        if (ln < (int)sizeof(line) - 1) line[ln++] = ch;
    }
    if (ln > 0) { line[ln] = 0; if (!pager_line(line)) return 0; }
    return 1;
}

static void cmd_cat(const char *arg) {
    if (!*arg) { vga_write(trs("usage: cat FILE\n", "用法：cat FILE\n")); return; }
    uint32_t c, s;
    if (fs_resolve_file(arg, &c, &s) != 0) { vga_write(trs("cat: no such file\n", "cat: 找不到文件\n")); return; }
    if (fs_perm_check(arg, g_uid, g_gid, PERM_READ) != 0) {
        vga_write(trs("cat: permission denied\n", "cat: 权限不足\n")); return;
    }
    pager_begin(1);
    static char buf[512];         /* ring0 single shell: no reentry */
    uint32_t off = 0;
    int done = 1;
    while (off < s) {
        uint32_t want = s - off;
        if (want > sizeof(buf) - 1) want = sizeof(buf) - 1;
        int r = fs_read_at(arg, buf, (int)off, (int)want);
        if (r <= 0) { vga_write(trs("cat: read error\n", "cat: 读取 错误\n")); pager_end(); return; }
        buf[r] = 0;
        if (!pager_dump_text(buf, (uint32_t)r)) { done = 0; break; }
        off += (uint32_t)r;
        if (off >= CAT_MAX_PRINT && off < s) {
            vga_write("\n[cat: output truncated at 64 KiB]\n");
            break;
        }
    }
    (void)done;
    pager_end();
}

/* v2.24 recursive helpers (rm -r / cp -r).  Depth-capped: every frame
 * keeps two FS_PATH_MAX buffers, so the cap also bounds shell stack use.
 * FAT32 has no symlinks, so no loop risk beyond pathological nesting. */
#define FSREC_DEPTH_MAX 6

static void rec_join(char *out, int cap, const char *dir, const char *leaf) {
    int o = 0;
    for (; *dir && o < cap - 1; dir++) out[o++] = *dir;
    if (o > 0 && out[o - 1] != '/' && o < cap - 1) out[o++] = '/';
    for (; *leaf && o < cap - 1; leaf++) out[o++] = *leaf;
    out[o] = 0;
}

struct cp_ud { const char *dst; int depth; };

/* v2.50: POSIX cp target normalization.  If DST is an existing directory,
 * the source entry lands inside it under its own basename
 * (`cp -r src existing_dir` -> `existing_dir/src`, merge semantics);
 * otherwise DST is used verbatim as the rename/overwrite target. */
static void cp_target(char *out, int cap, const char *dst, const char *src) {
    if (!fs_is_dir(dst)) { rec_join(out, cap, "", dst); return; }
    const char *bs = src;
    for (const char *q = src; *q; q++) if (*q == '/') bs = q + 1;
    rec_join(out, cap, dst, bs);
}

/* v2.50: ACL mode carried over by a copy.  Plain cp keeps the creator as
 * owner (owner preservation, cp -p, is not implemented); only the mode
 * bits transfer, so an executable .ce stays runnable and a restricted
 * file keeps its bits.  No ACL entry falls back to the 0755 no-entry
 * default used since v2.24. */
static int cp_src_mode(const char *src) {
    int fuid, fgid, fmode;
    if (acl_lookup(src, &fuid, &fgid, &fmode) != 0) return 0755;
    return fmode;
}

static int copy_rec(const char *src, const char *dst, int depth);

/* src prefix for the current fs_foreach walk -- the shell is the only
 * ring0 thread, so a module-static here is race-free (same pattern the
 * job/log code uses). */
static const char *cp_src_prefix;

static void cp_cb(int is_dir, const char *name, uint32_t size, void *ud) {
    (void)size;
    struct cp_ud *u = (struct cp_ud *)ud;
    if (!strcmp_(name, ".") || !strcmp_(name, "..")) return;
    char full_d[FS_PATH_MAX];
    rec_join(full_d, sizeof(full_d), u->dst, name);
    char full_s[FS_PATH_MAX];
    rec_join(full_s, sizeof(full_s), cp_src_prefix, name);
    if (is_dir) copy_rec(full_s, full_d, u->depth + 1);
    else        fs_copy_file(full_s, full_d);
}

static int copy_rec(const char *src, const char *dst, int depth) {
    if (depth > FSREC_DEPTH_MAX) return -5;
    if (fs_is_dir(src)) {
        char parent[FS_PATH_MAX];
        if (perm_write_on_parent(dst, parent, sizeof(parent)) == -13)
            return -13;
        int r = fs_mkdir(dst);
        if (r != FS_OK && r != FS_ERR_EXIST) return -2;
        acl_set(dst, g_uid, g_gid, cp_src_mode(src));
        const char *saved = cp_src_prefix;
        cp_src_prefix = src;
        struct cp_ud u = { dst, depth };
        fs_foreach(src, cp_cb, &u);
        cp_src_prefix = saved;
        return 0;
    }
    return fs_copy_file(src, dst);
}

static int rm_rec(const char *path, int depth);

/* v2.76 (#155): rm -r used to recurse straight out of the fs_foreach
 * callback, i.e. it deleted entries of the very directory it was walking.
 * FAT dirent slots are reused as soon as they are freed, so a child
 * recursion could hand the parent's iterator a recycled slot -- entries
 * survived, or a name got deleted twice.  The walk is now two-phase: take a
 * NAME SNAPSHOT of the directory first (fs_enum_entries, which fills a fixed
 * array), then act on the snapshot, re-scanning once the batch is done.
 * Deleting therefore never happens while a directory stream is open. */
#define RM_SNAP_BATCH   32
static char rm_snap[FSREC_DEPTH_MAX + 1][RM_SNAP_BATCH][FS_NAME_MAX];

static int rm_rec(const char *path, int depth) {
    if (depth > FSREC_DEPTH_MAX) return -5;
    if (!fs_is_dir(path)) return fs_delete(path) == 0 ? 0 : -1;
    if (fs_perm_check(path, g_uid, g_gid, PERM_WRITE) != 0) return -13;

    int rounds = 0;
    while (rounds++ < 64) {
        int n = fs_enum_entries(path, rm_snap[depth], RM_SNAP_BATCH);
        if (n <= 0) break;
        if (n > RM_SNAP_BATCH) n = RM_SNAP_BATCH;
        int acted = 0;
        for (int i = 0; i < n; i++) {
            char nm[FS_NAME_MAX];
            int j = 0;
            while (rm_snap[depth][i][j] && j < FS_NAME_MAX - 1) { nm[j] = rm_snap[depth][i][j]; j++; }
            nm[j] = 0;
            int isdir = 0;
            if (j > 0 && nm[j - 1] == '/') { nm[j - 1] = 0; isdir = 1; }
            if (!nm[0] || !strcmp_(nm, ".") || !strcmp_(nm, "..")) continue;
            char full[FS_PATH_MAX];
            rec_join(full, sizeof(full), path, nm);
            acted = 1;
            if (isdir) {
                int r = rm_rec(full, depth + 1);
                if (r == -13) return -13;
            } else if (fs_perm_check(full, g_uid, g_gid, PERM_WRITE) == 0) {
                if (fs_delete(full) == 0) acl_unset(full);
            }
        }
        if (!acted) break;              /* only "." / ".." left */
    }
    int r = fs_rmdir_force(path);
    if (r == 0) acl_unset(path);
    return r;
}

static void cmd_rm(const char *arg) {
    int rec = 0, dironly = 0, force = 0;
    while (*arg == ' ') arg++;
    while (arg[0] == '-' && arg[1] && arg[1] != ' ') {
        arg++;
        for (; *arg && *arg != ' '; arg++) {
            if (*arg == 'r' || *arg == 'R') rec = 1;
            else if (*arg == 'd') dironly = 1;
            else if (*arg == 'f') force = 1;
        }
        while (*arg == ' ') arg++;
    }
    (void)force;
    if (!*arg) {
        vga_write(trs("usage: rm [-d|-r] [-f] FILE|DIR\n", "用法：rm [-d|-r] [-f] 文件或目录\n"));
        return;
    }
    if (dironly) {
        char abs[FS_PATH_MAX];
        fs_canon_path(arg, abs, sizeof(abs));
        if (!strcmp_(abs, "/")) { vga_write(trs("rm: refusing to remove /\n", "rm: 拒绝 to remove /\n")); return; }
        if (!fs_is_dir(arg)) {
            if (!force) vga_write(trs("rm: not a directory\n", "rm: 不是目录\n"));
            return;
        }
        if (fs_perm_check(arg, g_uid, g_gid, PERM_WRITE) != 0) {
            vga_write(trs("rm: permission denied\n", "rm: 权限不足\n")); return;
        }
        int rd = fs_rmdir(arg);
        if (rd == 0) { acl_unset(arg); return; }
        if (!force) {
            if (rd == -3) vga_write(trs("rm: directory not empty\n", "rm: 目录 非空\n"));
            else vga_write(trs("rm: directory remove failed\n", "rm: 目录 remove 失败\n"));
        }
        return;
    }
    if (fs_is_dir(arg)) {
        if (!rec) { vga_write("rm: is a directory (use rm -r)\n"); return; }
        char abs[FS_PATH_MAX];
        fs_canon_path(arg, abs, sizeof(abs));
        if (!strcmp_(abs, "/")) { vga_write(trs("rm: refusing to remove /\n", "rm: 拒绝 to remove /\n")); return; }
        int r = rm_rec(arg, 0);
        if (r == 0) return;
        if (r == -13) vga_write(trs("rm: permission denied\n", "rm: 权限不足\n"));
        else if (r == -5) vga_write(trs("rm: nesting too deep\n", "rm: 嵌套层数过深\n"));
        else vga_write(trs("rm: directory remove failed (entries left behind?)\n", "rm: 目录 remove 失败 (entries left behind?)\n"));
        return;
    }
    if (fs_perm_check(arg, g_uid, g_gid, PERM_WRITE) != 0) {
        vga_write(trs("rm: permission denied\n", "rm: 权限不足\n")); return;
    }
    int r = fs_delete(arg);
    if (r == 0) { acl_unset(arg); return; }
    vga_write(trs("rm: no such file\n", "rm: 找不到文件\n"));
}

/* v2.50: rmdir DIR -- remove an EMPTY directory only (no -r; non-empty
 * directories are refused, matching POSIX rmdir(2)). */
static void cmd_rmdir(const char *arg) {
    if (!*arg) { vga_write(trs("usage: rmdir DIR\n", "用法：rmdir DIR\n")); return; }
    char abs[FS_PATH_MAX];
    fs_canon_path(arg, abs, sizeof(abs));
    if (!strcmp_(abs, "/")) { vga_write(trs("rmdir: refusing to remove /\n", "rmdir: 拒绝 to remove /\n")); return; }
    if (!fs_is_dir(arg)) { vga_write(trs("rmdir: not a directory\n", "rmdir: 不是目录\n")); return; }
    /* #168: rmdir had no permission check at all (unlike cmd_rm).  Gate
     * removal on PERM_WRITE for the session identity, the same check the
     * file delete path applies. */
    if (fs_perm_check(arg, g_uid, g_gid, PERM_WRITE) != 0) {
        vga_write(trs("rmdir: permission denied\n", "rmdir: 权限不足\n")); return;
    }
    int r = fs_rmdir(arg);
    if (r == 0) { acl_unset(arg); return; }
    if (r == -3)      vga_write(trs("rmdir: directory not empty\n", "rmdir: 目录 非空\n"));
    else if (r == -2) vga_write(trs("rmdir: not a directory or root\n", "rmdir: 不是目录 or root\n"));
    else              vga_write(trs("rmdir: no such directory\n", "rmdir: 找不到目录\n"));
}

/* shared copy core for cp / mv: read src in <=16 KiB chunks, append to a
 * freshly truncated dst.  Returns 0 ok, negative error code. */
static int fs_copy_file(const char *src, const char *dst) {
    uint32_t c, s;
    if (fs_resolve_file(src, &c, &s) != 0) return -1;
    if (fs_perm_check(src, g_uid, g_gid, PERM_READ) != 0) return -13;
    if (fs_perm_check(dst, g_uid, g_gid, PERM_WRITE) != 0) return -13;
    static char chunk[16384];     /* ring0 single shell: no reentry */
    if (fs_write_raw_mode(dst, "", 0, 0) != 0) return -2;   /* create/truncate */
    /* v2.50: carry the source mode bits over; creator stays owner. */
    acl_set(dst, g_uid, g_gid, cp_src_mode(src));
    uint32_t off = 0;
    while (off < s) {
        uint32_t want = s - off;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        int r = fs_read_at(src, chunk, (int)off, (int)want);
        if (r <= 0) return -3;
        if (fs_write_raw_mode(dst, chunk, (uint32_t)r, 1) != 0) return -4;
        off += (uint32_t)r;
    }
    return 0;
}

static void cmd_cp(char *arg) {
    int rec = 0;
    while (*arg == ' ') arg++;
    if (arg[0] == '-' && (arg[1] == 'r' || arg[1] == 'R')) {
        rec = 1;
        arg += 2;
        while (*arg == ' ') arg++;
    }
    char *src = arg;
    while (*arg && *arg != ' ') arg++;
    if (!*arg) { vga_write(trs("usage: cp SRC DST\n", "用法：cp SRC DST\n")); return; }
    *arg++ = 0;
    while (*arg == ' ') arg++;
    char *dst = arg;
    while (*arg && *arg != ' ') arg++;
    *arg = 0;
    if (!*src || !*dst) { vga_write(trs("usage: cp [-r] SRC DST\n", "用法：cp [-r] SRC DST\n")); return; }
    /* v2.50: if DST is an existing directory, merge SRC into DST/<basename> */
    char dstn[FS_PATH_MAX];
    cp_target(dstn, sizeof(dstn), dst, src);
    if (rec && fs_is_dir(src)) {
        char abs[FS_PATH_MAX];
        fs_canon_path(src, abs, sizeof(abs));
        if (!strcmp_(abs, "/")) { vga_write(trs("cp: refusing to copy /\n", "cp: 拒绝 to copy /\n")); return; }
        int r = copy_rec(src, dstn, 0);
        if (r == 0) return;
        if (r == -13) vga_write(trs("cp: permission denied\n", "cp: 权限不足\n"));
        else if (r == -2) vga_write(trs("cp: cannot create target directory\n", "cp: 无法create target 目录\n"));
        else if (r == -5) vga_write(trs("cp: nesting too deep\n", "cp: 嵌套层数过深\n"));
        else              vga_write(trs("cp: copy failed mid-way\n", "cp: copy 失败 mid-way\n"));
        return;
    }
    int r = fs_copy_file(src, dstn);
    if (r == 0) return;
    if (r == -13) vga_write(trs("cp: permission denied\n", "cp: 权限不足\n"));
    else if (r == -1) vga_write(trs("cp: no such file\n", "cp: 找不到文件\n"));
    else if (r == -2) vga_write(trs("cp: cannot create target\n", "cp: 无法create target\n"));
    else              vga_write(trs("cp: copy failed mid-way\n", "cp: copy 失败 mid-way\n"));
}

static void cmd_mv(char *arg) {
    while (*arg == ' ') arg++;
    char *src = arg;
    while (*arg && *arg != ' ') arg++;
    if (!*arg) { vga_write(trs("usage: mv SRC DST\n", "用法：mv SRC DST\n")); return; }
    *arg++ = 0;
    while (*arg == ' ') arg++;
    char *dst = arg;
    while (*arg && *arg != ' ') arg++;
    *arg = 0;
    if (!*src || !*dst) { vga_write(trs("usage: mv SRC DST\n", "用法：mv SRC DST\n")); return; }
    int r = fs_copy_file(src, dst);
    if (r == 0) {
        /* copy complete -> remove the source (write perm on it) */
        if (fs_perm_check(src, g_uid, g_gid, PERM_WRITE) != 0) {
            vga_write(trs("mv: copied but cannot delete source (permission)\n", "mv: copied but 无法delete source (permission)\n"));
            return;
        }
        if (fs_delete(src) != 0) {
            vga_write(trs("mv: copied but could not delete source\n", "mv: 已复制，但无法删除源文件\n"));
            return;
        }
        acl_unset(src);
        return;
    }
    if (r == -13) vga_write(trs("mv: permission denied\n", "mv: 权限不足\n"));
    else if (r == -1) vga_write(trs("mv: no such file\n", "mv: 找不到文件\n"));
    else if (r == -2) vga_write(trs("mv: cannot create target\n", "mv: 无法create target\n"));
    else              vga_write(trs("mv: move failed mid-way\n", "mv: move 失败 mid-way\n"));
}

/* ==================== NIGHT-P4: package manager ============================
 * Local package repository on the FAT volume under /pkg/:
 *   /pkg/PKGS.TXT  line index: name|version|desc|file
 *   pkg/*.catpkg  packaged CE apps (container format, see tools/mkpkg.py)
 *
 * `pkg install` validates the container and drops its payload into
 * /bin/<name>.ce; `pkg remove` deletes that copy.  No network involved
 * (the guest boots with -net none); the repo is seeded from blobs embedded
 * in the kernel image by gen_builtin.py.
 *
 * .catpkg layout (mirrors kernel_v25/tools/mkpkg.py, keep in sync):
 *   0    magic[8]   "CPKG01\0"
 *   8    name[16]
 *   24   version[16]
 *   40   cmd[16]    installed command -> /bin/<cmd>.ce
 *   56   desc[80]
 *   136  payload_len u32 LE
 *   140  signature[64]   RSA-512 signature over header[0..139]|payload
 *   204  payload (raw CE01 image)
 * ------------------------------------------------------------------------- */
#define PKG_DIR        "/pkg"
#define PKG_INDEX      "/pkg/PKGS.TXT"
#define PKG_HDR_SIZE   204
#define PKG_DL_TMP     "/pkg/.pkgdl.tmp"
#define PKG_IDX_TMP    "/pkg/.idx.tmp"

typedef struct {
    char name[16];
    char version[16];
    char desc[80];
    char file[16];
} pkg_rec_t;

static char pkg_idx_buf[4096];

static void pk_pad_copy(char *dst, int cap, const char *src, int width) {
    int i = 0;
    while (i < width && i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Parse /pkg/PKGS.TXT into recs; returns count (0 = no repo yet). */
static int pkg_load_index(pkg_rec_t *recs, int max) {
    int len = fs_read_text(PKG_INDEX, pkg_idx_buf, (int)sizeof(pkg_idx_buf) - 1);
    if (len <= 0) return 0;
    pkg_idx_buf[len] = 0;
    int n = 0;
    char *p = pkg_idx_buf;
    while (*p && n < max) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *e = line + slen_(line);
        if (e > line && e[-1] == '\r') e[-1] = 0;
        if (!*line || line[0] == '#') continue;

        char *fields[4] = { line, 0, 0, 0 };
        int fi = 0;
        for (char *q = line; *q && fi < 3; q++) {
            if (*q == '|') { *q = 0; fields[++fi] = q + 1; }
        }
        if (!fields[0] || !*fields[0] || !fields[3] || !*fields[3]) continue;
        pk_pad_copy(recs[n].name,    sizeof(recs[n].name),    fields[0], 15);
        pk_pad_copy(recs[n].version,sizeof(recs[n].version),fields[1], 15);
        pk_pad_copy(recs[n].desc,    sizeof(recs[n].desc),    fields[2], 79);
        pk_pad_copy(recs[n].file,   sizeof(recs[n].file),     fields[3], 15);
        n++;
    }
    return n;
}

static int pkg_find(const char *name, pkg_rec_t *recs, int n) {
    for (int i = 0; i < n; i++)
        if (!strcmp_(recs[i].name, name)) return i;
    return -1;
}

static int pkg_installed(const pkg_rec_t *r) {
    char p[FS_PATH_MAX];
    p[0] = 0;
    strcat_b(p, sizeof(p), "/bin/");
    strcat_b(p, sizeof(p), r->name);
    strcat_b(p, sizeof(p), ".ce");
    uint32_t c = 0, s = 0;
    return fs_resolve_file(p, &c, &s) == 0;
}

static void pkg_dst_path(const pkg_rec_t *r, char *out, int cap) {
    out[0] = 0;
    strcat_b(out, cap, "/bin/");
    strcat_b(out, cap, r->name);
    strcat_b(out, cap, ".ce");
}

static void pkg_pad(const char *s, int width) {
    int l = slen_(s);
    vga_write(s);
    for (int p = l; p < width; p++) vga_putc(' ');
}
static void cmd_pkg_list(void) {
    pkg_rec_t recs[16];
    int n = pkg_load_index(recs, 16);
    if (n == 0) { vga_write(trs("pkg: no package repository (/pkg/PKGS.TXT missing)\n", "pkg: 没有软件源（缺少 /pkg/PKGS.TXT）\n")); return; }
    vga_write("Package repository (/pkg/):\n");
    for (int i = 0; i < n; i++) {
        int installed = pkg_installed(&recs[i]);
        vga_write("  ");
        con_set_fg_rgb(pal_rgb(PAL_PROMPT_PATH));
        pkg_pad(recs[i].name, 10);
        con_set_fg_rgb(pal_rgb(PAL_DIM));
        vga_write("[");
        pkg_pad(recs[i].version, 8);
        vga_write("]  ");
        con_set_fg_rgb(pal_rgb(installed ? PAL_SUCCESS : PAL_DIM));
        pkg_pad(installed ? "installed" : "available", 11);
        con_set_fg_rgb(pal_rgb(PAL_FG));
        vga_write("- ");
        vga_write(recs[i].desc);
        con_set_fg_rgb(pal_rgb(PAL_FG));
        vga_write("\n");
    }
}

static void cmd_pkg_info(const char *name) {
    if (!*name) { vga_write(trs("usage: pkg info NAME\n", "用法：pkg info NAME\n")); return; }
    pkg_rec_t recs[16];
    int n = pkg_load_index(recs, 16);
    int i = pkg_find(name, recs, n);
    if (i < 0) { vga_write(trs("pkg: no such package: ", "pkg: 找不到软件包: ")); vga_write(name); vga_write("\n"); return; }
    con_set_fg_rgb(pal_rgb(PAL_DIM));
    vga_write("name:    ");
    con_set_fg_rgb(pal_rgb(PAL_PROMPT_PATH)); vga_write(recs[i].name); vga_write("\n");
    con_set_fg_rgb(pal_rgb(PAL_DIM));
    vga_write("version: ");
    con_set_fg_rgb(pal_rgb(PAL_FG)); vga_write(recs[i].version); vga_write("\n");
    con_set_fg_rgb(pal_rgb(PAL_DIM));
    vga_write("desc:    ");
    con_set_fg_rgb(pal_rgb(PAL_FG)); vga_write(recs[i].desc); vga_write("\n");
    con_set_fg_rgb(pal_rgb(PAL_DIM));
    vga_write("file:    ");
    con_set_fg_rgb(pal_rgb(PAL_FG)); vga_write(recs[i].file); vga_write("\n");
    con_set_fg_rgb(pal_rgb(PAL_DIM));
    vga_write("status:  ");
    con_set_fg_rgb(pal_rgb(pkg_installed(&recs[i]) ? PAL_SUCCESS : PAL_DIM));
    vga_write(pkg_installed(&recs[i]) ? "installed" : "available");
    con_set_fg_rgb(pal_rgb(PAL_FG));
    vga_write("\n");
}

/* ---- v2.68: package signature (RSA-512 public key + FNV-1a64) --------
 * The private key lives in tools/pkg_devkey.txt (host-side only); the
 * kernel embeds ONLY the public modulus N with fixed e=65537.  A
 * .catpkg signature is pow(0x01<<56 | digest, d, n), 64 bytes big-endian.
 * Verification is pow(s, e, n) compared with the encoded digest.
 * Big integers are 64-byte arrays; schoolbook multiply + bit long division
 * keeps this small enough for ring0 and free of libc bignum deps. */
static const uint8_t PKG_PUB_N[64] = {0xD0,0x58,0x73,0x7A,0x20,0xF1,0xFB,0x25,0xB3,0xFA,0x74,0x68,0x2E,0x7B,0x67,0xC2,0x96,0x4D,0xA1,0xB0,0x65,0x8B,0x57,0x56,0x81,0x15,0x63,0xC3,0x4C,0xFD,0xDB,0x11,0x27,0x57,0x75,0xBB,0x6B,0xE1,0x55,0xE4,0x0A,0xCF,0x3B,0x0C,0x67,0x0C,0xA6,0x9F,0x1A,0x18,0xF7,0x83,0xB6,0xE5,0x02,0xBF,0xEC,0x10,0x6E,0x5E,0xA2,0x78,0x59,0xD9};

/* v2.68.1: the first bigint attempt was broken three ways -- uint16_t
 * limb accumulation overflowed (64 * 255*255 ~= 4.1M >> 65535), the
 * byte order mixed big-endian keys with little-endian arithmetic, and
 * the "square-and-multiply" loop squared the BASE instead of the result.
 * No signature ever verified.  Replace it wholesale with a compact,
 * internally-consistent little-endian 512-bit bignum (17 x uint32_t,
 * one guard limb) using double-and-add modular multiply + proper
 * square-and-multiply exponentiation.  All external values are converted
 * from their big-endian wire form once, on entry. */
typedef struct { uint32_t w[17]; } bn_t;   /* limbs little-endian, guard limb 16 */

static void bn_zero(bn_t *o) { for (int i = 0; i < 17; i++) o->w[i] = 0; }
static void bn_copy(const bn_t *a, bn_t *o) { for (int i = 0; i < 17; i++) o->w[i] = a->w[i]; }
static void bn_from_be(const uint8_t b[64], bn_t *o) {
    bn_zero(o);
    for (int i = 0; i < 64; i++) o->w[i >> 2] |= (uint32_t)b[63 - i] << ((i & 3) * 8);
}
static int bn_cmp(const bn_t *a, const bn_t *b) {
    for (int i = 16; i >= 0; i--) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}
/* o = a - b, requires a >= b */
static void bn_sub(const bn_t *a, const bn_t *b, bn_t *o) {
    uint32_t borrow = 0;
    for (int i = 0; i < 17; i++) {
        uint64_t av = a->w[i], bv = (uint64_t)b->w[i] + borrow;
        o->w[i] = (uint32_t)(av - bv);
        borrow = (av < bv) ? 1u : 0u;
    }
}
/* o = (a << 1) | addbit */
static void bn_shl1(const bn_t *a, uint32_t addbit, bn_t *o) {
    uint32_t carry = addbit & 1u;
    for (int i = 0; i < 17; i++) {
        uint32_t nc = a->w[i] >> 31;
        o->w[i] = (a->w[i] << 1) | carry;
        carry = nc;
    }
}
/* out = a * b mod n, double-and-add over the bits of b (MSB first) */
static void bn_mulmod(const bn_t *a, const bn_t *b, const bn_t *n, bn_t *out) {
    bn_t r; bn_zero(&r);
    for (int bit = 511; bit >= 0; bit--) {
        uint32_t bbit = (b->w[bit >> 5] >> (bit & 31)) & 1u;
        bn_t t; bn_shl1(&r, 0, &t);          /* t = r*2 */
        if (bbit) {                          /* t += a  (double-and-add:
                                             * do NOT OR bbit into r first --
                                             * that adds a spurious +1 per set
                                             * bit and breaks e.g. 1*sig). */
            uint64_t carry = 0;
            for (int i = 0; i < 17; i++) {
                uint64_t s = (uint64_t)t.w[i] + a->w[i] + carry;
                t.w[i] = (uint32_t)s; carry = s >> 32;
            }
        }
        while (bn_cmp(&t, n) >= 0) bn_sub(&t, n, &t);
        bn_copy(&t, &r);
    }
    bn_copy(&r, out);
}
/* out = sig^65537 mod n -- proper left-to-right square-and-multiply */
static void bn_pow65537(const bn_t *sig, const bn_t *n, bn_t *out) {
    bn_t res, base;
    bn_zero(&res); res.w[0] = 1;
    bn_copy(sig, &base);
    for (int bit = 16; bit >= 0; bit--) {
        if (bit < 16) bn_mulmod(&base, &base, n, &base);   /* square the BASE */
        if ((65537u >> bit) & 1u) bn_mulmod(&res, &base, n, &res); /* accumulate into RESULT */
    }
    bn_copy(&res, out);
}
/* v2.76 (E1): the digest was a hand-rolled FNV-1a64 -- a non-cryptographic
 * hash, so a forged package only needed a 64-bit collision.  It is now
 * SHA-256 over (header || payload) truncated to the 56 bits that fit under
 * the 512-bit RSA modulus; tools/mkpkg.py signs exactly the same value.
 * Note the ceiling: RSA-512 + 56-bit digest is tamper-EVIDENT against
 * accidental or casual forgery, not a strong signature scheme. */
static uint8_t pkg_sig_be[64];      /* signature bytes of the package in hand */
static int pkg_check_sig(uint64_t h) {
    bn_t n, sig, calc, want;
    uint8_t wantbe[64];
    bn_from_be(PKG_PUB_N, &n);
    bn_from_be(pkg_sig_be, &sig);
    bn_pow65537(&sig, &n, &calc);
    uint64_t m = (1ULL << 56) | (h & ((1ULL << 56) - 1));
    for (int i = 0; i < 64; i++) wantbe[i] = 0;
    for (int i = 0; i < 8; i++) wantbe[56 + i] = (uint8_t)(m >> (56 - 8 * i));
    bn_from_be(wantbe, &want);
    return bn_cmp(&calc, &want) == 0 ? 0 : -1;
}

/* ---- v2.70: remote package repository (HTTP) -------------------------
 * `pkg repo URL`        set/show the remote base URL (persisted in hive)
 * `pkg update`          GET <repo>/PKGS.TXT -> /pkg/PKGS.TXT
 * `pkg fetch NAME|URL`  GET a signed .catpkg over HTTP, verify with the
 *                       SAME pkg_verify() as local installs, then drop the
 *                       payload into /bin/<cmd>.ce.  A remote package is
 *                       never trusted until its RSA-512 signature checks.
 *
 * v2.78 (D10): the download goes through net_http_download_stream(), which
 * hands each body slice to the file sink as it arrives; neither the TCP
 * window nor a RAM buffer caps package size. */
#define PKG_REPO_KEY "HKLM\\Software\\CatOS\\Net\\PkgRepo"

static const char *pkg_repo_get(void) {
    const char *v = hive_sz(PKG_REPO_KEY, "");
    return (v && *v) ? v : 0;
}

static void cmd_pkg_repo(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        const char *v = pkg_repo_get();
        if (!v) {
            vga_write(trs("pkg: no remote repo set\n", "pkg：未设置远程仓库\n"));
            vga_write(trs("usage: pkg repo http://IP[:port]/dir\n", "用法：pkg repo http://IP[:port]/dir\n"));
            return;
        }
        vga_write("pkg: remote repo = "); vga_write(v); vga_write("\n");
        return;
    }
    /* reject non-http URLs early */
    if (arg[0] != 'h' || arg[1] != 't' || arg[2] != 't' || arg[3] != 'p' ||
        arg[4] != ':' || arg[5] != '/' || arg[6] != '/') {
        vga_write(trs("pkg: repo URL must start with http://\n", "pkg：仓库 URL 必须以 http:// 开头\n"));
        return;
    }
    if (hive_set_sz(PKG_REPO_KEY, arg) == 0) {
        hive_save();
        vga_write("pkg: remote repo -> "); vga_write(arg); vga_write("\n");
    } else {
        vga_write(trs("pkg: failed to save repo URL\n", "pkg：保存仓库 URL 失败\n"));
    }
}

/* Join <base>/<file> into out (base has no trailing slash). */
static void pkg_url_join(char *out, int cap, const char *base, const char *file) {
    int n = 0;
    while (base[n] && n < cap - 1) { out[n] = base[n]; n++; }
    if (n > 0 && out[n-1] == '/') n--;
    if (n < cap - 1) out[n++] = '/';
    int k = 0;
    while (file[k] && n < cap - 1) out[n++] = file[k++];
    out[n] = 0;
}

/* Generic streamed-download sink: append each body slice to a scratch file.
 * off==0 on the first slice (truncate/create), afterwards append. */
typedef struct { const char *path; uint32_t off; } pkg_file_sink_t;
static int pkg_file_sink_cb(void *ud, const uint8_t *p, int len) {
    pkg_file_sink_t *s = (pkg_file_sink_t *)ud;
    if (fs_write_raw_mode(s->path, (const char *)p, (uint32_t)len, s->off ? 1 : 0) != 0)
        return -1;
    s->off += (uint32_t)len;
    return 0;
}

static void cmd_pkg_update(void) {
    const char *base = pkg_repo_get();
    if (!base) {
        vga_write(trs("pkg: no remote repo set (use 'pkg repo URL')\n", "pkg：未设置远程仓库（用 'pkg repo URL'）\n")); return;
    }
    if (!net_up()) { vga_write(trs("pkg: network not ready\n", "pkg：网络未就绪\n")); net_why_down(); return; }
    char url[FS_PATH_MAX];
    pkg_url_join(url, sizeof(url), base, "PKGS.TXT");
    vga_write("pkg: fetching "); vga_write(url); vga_write(" ...\n");
    /* v2.78 (D10): stream the index to a scratch file, then promote it.
     * The sink's first slice truncates/creates the file (off==0, mode 0). */
    pkg_file_sink_t sk; sk.path = PKG_IDX_TMP; sk.off = 0;
    int st = 0; long total = 0;
    int n = net_http_download_stream(url, 0, pkg_file_sink_cb, &sk, &st, &total);
    if (n < 0 || sk.off == 0) {
        fs_delete(PKG_IDX_TMP);
        vga_write(trs("pkg: download failed (code ", "pkg：下载失败（错误码 "));
        print_int(n); vga_write(")\n"); return;
    }
    int rd = fs_read_text(PKG_IDX_TMP, pkg_idx_buf, (int)sizeof(pkg_idx_buf) - 1);
    fs_delete(PKG_IDX_TMP);
    if (rd <= 0) { vga_write(trs("pkg: empty index\n", "pkg：索引为空\n")); return; }
    pkg_idx_buf[rd] = 0;
    if (fs_write_text(PKG_INDEX, pkg_idx_buf, (uint32_t)rd) != 0) {
        vga_write(trs("pkg: failed to write local index\n", "pkg：写入本地索引失败\n")); return;
    }
    vga_write("pkg: index updated ("); print_int(rd); vga_write(" bytes)\n");
}

/* ---- v2.78 (D10): install straight from a file -------------------------
 * The v2.70 path read the whole package into a 16 KiB static buffer, so the
 * download (tcp.rcv_buf) and the install both capped packages at 16 KiB.
 * Everything below walks the file in 4 KiB slices instead: the SHA-256 is
 * streamed over (header || payload), the signature is read from offset 140,
 * and the CE01 payload is copied chunk by chunk into /bin.  The only size
 * limit left is free disk space. */
#define PKG_IO_CHUNK 4096

static int pkg_file_digest(const char *path, uint32_t plen, uint64_t *out) {
    uint8_t b[PKG_IO_CHUNK];
    sha256_stream_t c;
    sha256_stream_init(&c);
    if (fs_read_at(path, (char *)b, 0, 140) != 140) return -1;
    sha256_stream_update(&c, b, 140);
    uint32_t off = 0;
    while (off < plen) {
        uint32_t want = plen - off;
        if (want > PKG_IO_CHUNK) want = PKG_IO_CHUNK;
        int got = fs_read_at(path, (char *)b, (int)(PKG_HDR_SIZE + off), (int)want);
        if (got <= 0) return -1;
        sha256_stream_update(&c, b, (uint32_t)got);
        off += (uint32_t)got;
    }
    uint8_t d[32];
    sha256_stream_final(&c, d);
    uint64_t h = 0;
    for (int i = 0; i < 7; i++) h = (h << 8) | d[i];
    *out = h;
    return 0;
}

static int pkg_copy_payload(const char *src, const char *dst, uint32_t plen) {
    uint8_t b[PKG_IO_CHUNK];
    if (fs_write_raw_mode(dst, "", 0, 0) != 0) return -1;   /* truncate/create */
    uint32_t off = 0;
    while (off < plen) {
        uint32_t want = plen - off;
        if (want > PKG_IO_CHUNK) want = PKG_IO_CHUNK;
        int got = fs_read_at(src, (char *)b, (int)(PKG_HDR_SIZE + off), (int)want);
        if (got <= 0) return -1;
        if (fs_write_raw_mode(dst, (const char *)b, (uint32_t)got, 1) != 0) return -1;
        off += (uint32_t)got;
    }
    return 0;
}

static void pkg_install_file(const char *path) {
    uint8_t hdr[PKG_HDR_SIZE];
    uint32_t clus = 0, sz = 0;
    if (fs_resolve_file(path, &clus, &sz) != 0 || sz < PKG_HDR_SIZE) {
        vga_write(trs("pkg: bad package magic (not a signed .catpkg)\n", "pkg：包魔数错误（不是已签名的 .catpkg）\n")); return;
    }
    if (fs_read_at(path, (char *)hdr, 0, PKG_HDR_SIZE) != PKG_HDR_SIZE) {
        vga_write(trs("pkg: read error\n", "pkg：读取错误\n")); return;
    }
    if (hdr[0] != 'C' || hdr[1] != 'P' || hdr[2] != 'K' || hdr[3] != 'G' ||
        hdr[4] != '0' || hdr[5] != '2') {
        vga_write(trs("pkg: bad package magic (not a signed .catpkg)\n", "pkg：包魔数错误（不是已签名的 .catpkg）\n")); return;
    }
    char cmd[16];
    pk_pad_copy(cmd, sizeof(cmd), hdr + 40, 15);
    {
        int n = 0;
        for (const char *q = cmd; *q; q++) n++;
        if (n < 1 || n > 15) {
            vga_write(trs("pkg: bad command name (length)\n", "pkg：命令名非法（长度）\n")); return;
        }
        for (const char *q = cmd; *q; q++) {
            char ch = *q;
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
                vga_write(trs("pkg: bad command name (only [A-Za-z0-9_-])\n",
                              "pkg：命令名非法（只允许 [A-Za-z0-9_-]）\n")); return;
            }
        }
    }
    uint32_t plen = (uint32_t)(unsigned char)hdr[136] |
                    ((uint32_t)(unsigned char)hdr[137] << 8) |
                    ((uint32_t)(unsigned char)hdr[138] << 16) |
                    ((uint32_t)(unsigned char)hdr[139] << 24);
    if (plen == 0 || PKG_HDR_SIZE + plen != sz) {
        vga_write(trs("pkg: corrupt package (length mismatch)\n", "pkg：包损坏（长度不匹配）\n")); return;
    }
    {
        unsigned char pl[20];
        if (fs_read_at(path, (char *)pl, PKG_HDR_SIZE, 20) != 20) {
            vga_write(trs("pkg: read error\n", "pkg：读取错误\n")); return;
        }
        uint32_t img = (uint32_t)pl[16] | ((uint32_t)pl[17] << 8) |
                       ((uint32_t)pl[18] << 16) | ((uint32_t)pl[19] << 24);
        uint32_t entry = (uint32_t)pl[4] | ((uint32_t)pl[5] << 8) |
                         ((uint32_t)pl[6] << 16) | ((uint32_t)pl[7] << 24);
        if (pl[0] != 'C' || pl[1] != 'E' || pl[2] != '0' || pl[3] != '1') {
            vga_write(trs("pkg: payload is not a CE01 image -- refused\n",
                          "pkg：载荷不是 CE01 可执行镜像——已拒绝\n")); return;
        }
        if (img == 0 || img != plen - 20 || entry >= img) {
            vga_write(trs("pkg: corrupt CE image header -- refused\n",
                          "pkg：CE 镜像头损坏——已拒绝\n")); return;
        }
    }
    for (int i = 0; i < 64; i++) pkg_sig_be[i] = hdr[140 + i];
    uint64_t h = 0;
    if (pkg_file_digest(path, plen, &h) != 0) {
        vga_write(trs("pkg: read error (digest)\n", "pkg：读取错误（摘要）\n")); return;
    }
    if (pkg_check_sig(h) != 0) {
        vga_write(trs("pkg: SIGNATURE VERIFICATION FAILED - install refused\n", "pkg：签名校验失败——拒绝安装\n"));
        return;
    }
    char dst[FS_PATH_MAX];
    dst[0] = 0;
    strcat_b(dst, sizeof(dst), "/bin/");
    strcat_b(dst, sizeof(dst), cmd);
    strcat_b(dst, sizeof(dst), ".ce");
    if (fs_perm_check(dst, g_uid, g_gid, PERM_WRITE) != 0) {
        vga_write(trs("pkg: permission denied writing to /bin\n", "pkg：权限不足 writing to /bin\n")); return;
    }
    if (pkg_copy_payload(path, dst, plen) != 0) {
        vga_write(trs("pkg: install failed (disk full?)\n", "pkg: install 失败 (disk full?)\n")); return;
    }
    acl_set(dst, g_uid, g_gid, 0755);
    vga_write("pkg: installed "); vga_write(cmd);
    vga_write(" -> "); vga_write(dst); vga_write("\n");
}

/* Fetch a signed package by NAME (via configured repo) or by an explicit
 * http:// URL.  Downloads raw bytes, then runs the shared verifier. */
static void cmd_pkg_fetch(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write(trs("usage: pkg fetch NAME | pkg fetch http://IP[:port]/file.catpkg\n", "用法：pkg fetch NAME | pkg fetch http://IP[:port]/file.catpkg\n")); return;
    }
    if (!net_up()) { vga_write(trs("pkg: network not ready\n", "pkg：网络未就绪\n")); net_why_down(); return; }
    if (!fs_fs_ready()) { vga_write(trs("pkg: no FAT32 volume\n", "pkg：没有 FAT32 卷\n")); return; }

    char url[FS_PATH_MAX];
    if (arg[0]=='h' && arg[1]=='t' && arg[2]=='t' && arg[3]=='p' && arg[4]==':') {
        /* explicit URL */
        int n = 0;
        while (*arg && n < (int)sizeof(url) - 1) url[n++] = *arg++;
        url[n] = 0;
    } else {
        /* by name: look up the filename in the (possibly just updated) index */
        const char *base = pkg_repo_get();
        if (!base) {
            vga_write(trs("pkg: no remote repo set (use 'pkg repo URL')\n", "pkg：未设置远程仓库（用 'pkg repo URL'）\n")); return;
        }
        pkg_rec_t recs[16];
        int nr = pkg_load_index(recs, 16);
        int i = pkg_find(arg, recs, nr);
        if (i < 0) {
            vga_write(trs("pkg: no such package in index: ", "pkg：索引中无此软件包: "));
            vga_write(arg); vga_write("\n"); return;
        }
        pkg_url_join(url, sizeof(url), base, recs[i].file);
    }
    vga_write("pkg: downloading "); vga_write(url); vga_write(" ...\n");
    /* v2.78 (D10): stream the body to a scratch file and consume it slice
     * by slice -- no download-size cap, no whole-package buffer. */
    /* Persistent scratch: the sink's first slice truncates/creates it
     * (off==0, mode 0); the stale copy from a previous fetch is overwritten.
     * We deliberately do not delete/recreate the entry between fetches. */
    const char *tmp = PKG_DL_TMP;
    pkg_file_sink_t sk; sk.path = tmp; sk.off = 0;
    int st = 0; long total = 0; long off = 0; int rd = 0;
    /* A single TCP connection goes idle after ~80 KiB under QEMU slirp, so
     * continue with HTTP ranges: every hop appends the next slice of the
     * same file.  A server that ignores Range (200 instead of 206) makes us
     * restart once; chunked replies cannot be resumed. */
    for (int hop = 0; hop < 400; hop++) {
        st = 0; total = 0;
        rd = net_http_download_stream(url, off, pkg_file_sink_cb, &sk, &st, &total);
        if (st == 200 && off > 0) { fs_write_raw_mode(tmp, "", 0, 0); sk.off = 0; off = 0; continue; }
        if (rd == -13) { off = sk.off; continue; }
        break;
    }
    if (rd < 0 || sk.off == 0) {
        vga_write(trs("pkg: download failed (code ", "pkg：下载失败（错误码 "));
        print_int(rd); vga_write(")\n"); return;
    }
    vga_write("pkg: got "); print_int((int)sk.off); vga_write(trs(" bytes, verifying ...\n", " 字节，校验中 ...\n"));
    pkg_install_file(tmp);
}

static void cmd_pkg_install(const char *name) {
    if (!*name) { vga_write(trs("usage: pkg install NAME\n", "用法：pkg install NAME\n")); return; }
    if (!fs_fs_ready()) { vga_write(trs("pkg: no FAT32 volume\n", "pkg: 没有 FAT32 卷\n")); return; }
    pkg_rec_t recs[16];
    int n = pkg_load_index(recs, 16);
    int i = pkg_find(name, recs, n);
    if (i < 0) { vga_write(trs("pkg: no such package: ", "pkg: 找不到软件包: ")); vga_write(name); vga_write("\n"); return; }

    char cpath[FS_PATH_MAX];
    cpath[0] = 0;
    strcat_b(cpath, sizeof(cpath), PKG_DIR);
    strcat_b(cpath, sizeof(cpath), "/");
    strcat_b(cpath, sizeof(cpath), recs[i].file);

    uint32_t clus = 0, sz = 0;
    if (fs_resolve_file(cpath, &clus, &sz) != 0) {
        vga_write(trs("pkg: payload missing: ", "pkg: 缺少数据文件: ")); vga_write(cpath); vga_write("\n"); return;
    }
    pkg_install_file(cpath);   /* v2.78: shared with remote fetch, streams the file */
}

static void cmd_pkg_remove(const char *name) {
    if (!*name) { vga_write(trs("usage: pkg remove NAME\n", "用法：pkg remove NAME\n")); return; }
    pkg_rec_t recs[16];
    int n = pkg_load_index(recs, 16);
    int i = pkg_find(name, recs, n);
    if (i < 0) { vga_write(trs("pkg: no such package: ", "pkg: 找不到软件包: ")); vga_write(name); vga_write("\n"); return; }

    char dst[FS_PATH_MAX];
    pkg_dst_path(&recs[i], dst, sizeof(dst));
    uint32_t clus = 0, sz = 0;
    if (fs_resolve_file(dst, &clus, &sz) != 0) {
        vga_write("pkg: "); vga_write(recs[i].name);
        vga_write(trs(" is not installed\n", " 未安装\n")); return;
    }
    if (fs_perm_check(dst, g_uid, g_gid, PERM_WRITE) != 0) {
        vga_write(trs("pkg: permission denied\n", "pkg: 权限不足\n")); return;
    }
    if (fs_delete(dst) != 0) { vga_write(trs("pkg: remove failed\n", "pkg: remove 失败\n")); return; }
    acl_unset(dst);
    vga_write("pkg: removed "); vga_write(recs[i].name); vga_write("\n");
}

static void cmd_pkg(char *arg) {
    char sub[16];
    int k = 0;
    while (*arg == ' ') arg++;
    while (*arg && *arg != ' ' && k < (int)sizeof(sub) - 1) sub[k++] = *arg++;
    sub[k] = 0;
    while (*arg == ' ') arg++;
    if (!strcmp_(sub, "list"))       cmd_pkg_list();
    else if (!strcmp_(sub, "info"))  cmd_pkg_info(arg);
    else if (!strcmp_(sub, "install")) cmd_pkg_install(arg);
    else if (!strcmp_(sub, "remove") || !strcmp_(sub, "rm")) cmd_pkg_remove(arg);
    else if (!strcmp_(sub, "repo"))   cmd_pkg_repo(arg);
    else if (!strcmp_(sub, "update")) cmd_pkg_update();
    else if (!strcmp_(sub, "fetch"))  cmd_pkg_fetch(arg);
    else {
        vga_write(trs("usage: pkg list | info NAME | install NAME | remove NAME\n", "用法：pkg list | info NAME | install NAME | remove NAME\n"));
        vga_write("       pkg repo [URL] | pkg update | pkg fetch NAME\n");
    }
}

/* ==================== NIGHT-P4: migrated common tools =====================
 * head / tail / wc / tree: shell builtins over the FAT32 raw read API.
 * Files are small (editor limit 4 KiB, CE payloads a few KiB), so a
 * 16 KiB static ring0 buffer per command is enough, matching cmd_cat.
 * ------------------------------------------------------------------------- */
#define TOOL_BUF_MAX 49152
static char tool_buf[TOOL_BUF_MAX + 1];   /* +1 for the trailing NUL */

/* Parse a leading [-n N] option; returns the file argument, sets *def_n. */
static const char *tool_parse_headtail(char *arg, int *def_n) {
    *def_n = 10;
    while (*arg == ' ') arg++;
    if (arg[0] == '-' && (arg[1] == 'n' || arg[1] == 'N')) {
        arg += 2;
        while (*arg == ' ') arg++;
        int v = 0;
        while (*arg >= '0' && *arg <= '9') { v = v * 10 + (*arg - '0'); arg++; }
        if (v > 0) *def_n = v;
        while (*arg == ' ') arg++;
    }
    return arg;
}

static int tool_read_whole(const char *path) {
    uint32_t clus = 0, sz = 0;
    if (fs_resolve_file(path, &clus, &sz) != 0) return -1;
    if (sz > TOOL_BUF_MAX) return -2;
    if (sz == 0) { tool_buf[0] = 0; return 0; }   /* empty file: resolve OK, no bytes */
    int rd = fs_read_at(path, tool_buf, 0, (int)sz);
    if (rd < 0) return -1;
    tool_buf[rd] = 0;
    return rd;
}

static void cmd_head(char *arg) {
    int def_n;
    const char *path = tool_parse_headtail(arg, &def_n);
    if (!*path) { vga_write(trs("usage: head [-n N] FILE\n", "用法：head [-n N] FILE\n")); return; }
    int rd = tool_read_whole(path);
    if (rd == -1) { vga_write(trs("head: no such file\n", "head: 找不到文件\n")); return; }
    if (rd == -2) { vga_write(trs("head: file too large\n", "head: 文件 过大\n")); return; }

    int lines = 0, cut = 0;
    for (int i = 0; i < rd; i++) {
        if (tool_buf[i] == '\n') {
            lines++;
            if (lines >= def_n) { cut = i + 1; break; }
        }
        cut = i + 1;
    }
    tool_buf[cut] = 0;
    vga_write(tool_buf);
    if (cut && tool_buf[cut - 1] != '\n') vga_write("\n");
}

static void cmd_tail(char *arg) {
    int def_n;
    const char *path = tool_parse_headtail(arg, &def_n);
    if (!*path) { vga_write(trs("usage: tail [-n N] FILE\n", "用法：tail [-n N] FILE\n")); return; }
    int rd = tool_read_whole(path);
    if (rd == -1) { vga_write(trs("tail: no such file\n", "tail: 找不到文件\n")); return; }
    if (rd == -2) { vga_write(trs("tail: file too large\n", "tail: 文件 过大\n")); return; }

    int lines = 0, start = rd;
    for (int i = rd - 1; i >= 0; i--) {
        if (tool_buf[i] == '\n') {
            lines++;
            if (lines >= def_n) { start = i + 1; break; }
        }
        start = i;
    }
    vga_write(tool_buf + start);
    if (start < rd && tool_buf[rd - 1] != '\n') vga_write("\n");
}

static void cmd_wc(char *arg) {
    int f_lines = 0, f_words = 0, f_bytes = 0;
    int any_flag = 0;
    while (*arg == ' ') arg++;
    while (arg[0] == '-' && arg[1]) {      /* collect flags: -l -w -c */
        any_flag = 1;
        for (int q = 1; arg[q] && arg[q] != ' '; q++) {
            if (arg[q] == 'l') f_lines = 1;
            else if (arg[q] == 'w') f_words = 1;
            else if (arg[q] == 'c') f_bytes = 1;
        }
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
    }
    if (!any_flag) f_lines = f_words = f_bytes = 1;   /* default: all */
    if (!*arg) { vga_write(trs("usage: wc [-l] [-w] [-c] FILE\n", "用法：wc [-l] [-w] [-c] FILE\n")); return; }

    int rd = tool_read_whole(arg);
    if (rd == -1) { vga_write(trs("wc: no such file\n", "wc: 找不到文件\n")); return; }
    if (rd == -2) { vga_write(trs("wc: file too large\n", "wc: 文件 过大\n")); return; }

    int lines = 0, words = 0, in_word = 0;
    for (int i = 0; i < rd; i++) {
        char c = tool_buf[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    if (rd > 0 && tool_buf[rd - 1] != '\n') lines++;

    if (f_lines) { print_int(lines); vga_write("  "); }
    if (f_words) { print_int(words); vga_write("  "); }
    if (f_bytes) { print_int(rd); vga_write("  "); }
    vga_write(arg); vga_write("\n");
}

/* v2.59: tree line buffer, accumulated per entry so one whole entry line
 * feeds the shared pager as a single unit (indent + marker + leaf + size). */
static char  g_tree_line[FS_NAME_MAX + 40];
static int   g_tree_ln;

static void tree_line_reset(void) { g_tree_line[0] = 0; g_tree_ln = 0; }
static void tree_line_app(const char *s) {
    while (*s && g_tree_ln < (int)sizeof(g_tree_line) - 1)
        g_tree_line[g_tree_ln++] = *s++;
    g_tree_line[g_tree_ln] = 0;
}
static void tree_line_raw(const uint8_t *raw, char *line, int cap, int *ln) {
    const char *hd = "0123456789ABCDEF";
    for (int k = 0; k < 11; k++) {
        char byte[3];
        byte[0] = hd[(raw[k] >> 4) & 0xF];
        byte[1] = hd[raw[k] & 0xF];
        byte[2] = 0;
        (void)line; (void)cap;
        if (*ln < cap - 2) { line[(*ln)++] = byte[0]; line[(*ln)++] = byte[1]; line[(*ln)] = 0; }
    }
}

/* tree: explicit-path recursion via fs_enum_entries, depth-capped at 6. */
static void tree_walk(const char *dir, int depth, int *hidden) {
    char names[24][FS_NAME_MAX];
    int n = fs_enum_entries(dir, names, 24);
    if (hidden) *hidden += fat_dir_hidden();
    for (int i = 0; i < n; i++) {
        const char *nm = names[i];
        int is_dir = nm[slen_(nm) - 1] == '/';
        char leaf[FS_NAME_MAX];
        int li = 0;
        for (int q = 0; nm[q] && li < (int)sizeof(leaf) - 1; q++) {
            if (nm[q] == '/') continue;
            leaf[li++] = nm[q];
        }
        leaf[li] = 0;
        if (!strcmp_(leaf, ".") || !strcmp_(leaf, "..")) continue;
        tree_line_reset();
        for (int d = 0; d < depth; d++) tree_line_app("  ");
        tree_line_app(is_dir ? "+- " : "- ");
        tree_line_app(leaf);
        if (is_dir) tree_line_app("/");
        if (!pager_line(g_tree_line)) return;
        if (is_dir && depth < 6) {
            char child[FS_PATH_MAX];
            child[0] = 0;
            strcat_b(child, sizeof(child), dir);
            if (dir[slen_(dir) - 1] != '/') strcat_b(child, sizeof(child), "/");
            strcat_b(child, sizeof(child), leaf);
            tree_walk(child, depth + 1, hidden);
            if (pager_quit()) return;
        }
    }
}

/* tree --raw: enumerate every live entry, insane ones flagged with their
 * cluster / slot offset / first byte / raw 8.3 bytes. */
static void tree_walk_raw(const char *dir, int depth, int *hidden) {
    struct fs_raw_ent names[24];
    int h = 0;
    int n = fs_enum_entries_raw(dir, names, 24, &h);
    if (hidden) *hidden += h;
    for (int i = 0; i < n; i++) {
        const char *nm = names[i].name;
        int is_dir = names[i].attr & FAT_ATTR_DIR;
        char leaf[FS_NAME_MAX];
        int li = 0;
        for (int q = 0; nm[q] && li < (int)sizeof(leaf) - 1; q++) {
            if (nm[q] == '/') continue;
            leaf[li++] = nm[q];
        }
        leaf[li] = 0;
        if (!strcmp_(leaf, ".") || !strcmp_(leaf, "..")) continue;
        tree_line_reset();
        for (int d = 0; d < depth; d++) tree_line_app("  ");
        tree_line_app(is_dir ? "+- " : "- ");
        tree_line_app(leaf);
        if (names[i].insane) {
            tree_line_app("  c=");
            { char ib[12]; itoa((int)names[i].cluster, ib, 0); tree_line_app(ib); }
            tree_line_app(" o=");
            { char ob[12]; itoa(names[i].slot * 32, ob, 0); tree_line_app(ob); }
            tree_line_app(" raw="); tree_line_raw(names[i].raw, g_tree_line,
                                                   sizeof(g_tree_line), &g_tree_ln);
        } else if (is_dir) {
            tree_line_app("/");
        }
        if (!pager_line(g_tree_line)) return;
        if (is_dir && depth < 6) {
            char child[FS_PATH_MAX];
            child[0] = 0;
            strcat_b(child, sizeof(child), dir);
            if (dir[slen_(dir) - 1] != '/') strcat_b(child, sizeof(child), "/");
            strcat_b(child, sizeof(child), leaf);
            tree_walk_raw(child, depth + 1, hidden);
            if (pager_quit()) return;
        }
    }
}

static void cmd_tree(char *arg) {
    int raw = 0;
    while (*arg == ' ') arg++;
    if (arg[0] == '-' && arg[1] == '-' && arg[2] == 'r') {
        raw = 1;
        arg += 5;
        while (*arg == ' ') arg++;
    }
    char dir[FS_PATH_MAX];
    if (!*arg) { fs_pwd(dir, sizeof(dir)); }
    else {
        if (arg[0] == '/') strcat_b(dir, sizeof(dir), arg);
        else {
            char pwd[FS_PATH_MAX];
            fs_pwd(pwd, sizeof(pwd));
            strcat_b(dir, sizeof(dir), pwd);
            if (pwd[slen_(pwd) - 1] != '/') strcat_b(dir, sizeof(dir), "/");
            strcat_b(dir, sizeof(dir), arg);
        }
    }
    pager_begin(1);
    { char root[FS_PATH_MAX + 2]; int rn = 0;
      for (const char *p = dir; *p && rn < (int)sizeof(root) - 2; p++) root[rn++] = *p;
      root[rn] = 0;
      if (!pager_line(root)) { pager_end(); return; }
    }
    int hidden = 0;
    if (raw) tree_walk_raw(dir, 0, &hidden);
    else     tree_walk(dir, 0, &hidden);
    if (!pager_quit() && !raw && hidden > 0) {
        char hb[12]; itoa(hidden, hb, 0);
        pager_line(hb);
    }
    pager_end();
}

/* ---------- v2.64: ls redesign (borrowing eza + lsd) -------------------
 * The TUI prototype in deliverables/tui-preview explored six display modes.
 * This is the kernel implementation, using only existing APIs:
 *   ls                            grid, dirs first, type-coloured
 *   ls -l                         long, r/w/x permission bits colourised
 *   ls -t / ls -lt                tree (lsd style: -t == tree; -lt adds sizes)
 *   ls -l --color-scale[=size]    eza: log(size) heat gradient (size/age)
 *   ls -l --blocks=perm,size,name lsd: pick + order the metadata columns
 * Colour goes through con_set_fg_rgb(), which quantises to the CLUT in text
 * mode and renders true colour under VBE24 -- same as the rest of the shell.
 * No Nerd-Font glyphs: the CE font is a real IBM VGA 8x16 ROM, so icons stay
 * ASCII, and no Git column (CatOS has no Git subsystem).
 */

/* case-insensitive name compare (FAT is case-insensitive) */
static int ls_icmp_shell(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* unsigned -> decimal, returns digit count */
static int ls_u2a(uint32_t v, char *o) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) o[i] = t[n - 1 - i];
    return n;
}

/* human size ("820K", "2.1K", "128") -> length */
static int ls_sz(uint32_t s, char *o) {
    int n = 0;
    if (s >= 1024u * 1024u) {
        n = ls_u2a(s >> 20, o);
        int frac = (int)(((s & 0xFFFFFu) * 10u) >> 20);
        o[n++] = '.'; o[n++] = (char)('0' + frac); o[n++] = 'M';
    } else if (s >= 1024u) {
        n = ls_u2a(s >> 10, o);
        int frac = (int)(((s & 0x3FFu) * 10u) >> 10);
        o[n++] = '.'; o[n++] = (char)('0' + frac); o[n++] = 'K';
    } else {
        n = ls_u2a(s, o);
    }
    o[n] = 0;
    return n;
}

/* eza-style heat gradient: t 0..100 -> blue->cyan->green->yellow->red */
static uint32_t ls_heat(int t) {
    static const uint32_t stop[5] = {
        CON_RGB(0x1F,0x6F,0xEB), CON_RGB(0x12,0xB8,0x86),
        CON_RGB(0x7E,0xE7,0x87), CON_RGB(0xF2,0xCC,0x60),
        CON_RGB(0xFF,0x5F,0x57),
    };
    if (t < 0) t = 0; if (t > 100) t = 100;
    int seg = t / 25; if (seg > 3) seg = 3;
    int f = t - seg * 25;
    uint32_t a = stop[seg], b = stop[seg + 1];
    int r  = (int)CON_RGB_R(a) + (((int)CON_RGB_R(b) - (int)CON_RGB_R(a)) * f) / 25;
    int g  = (int)CON_RGB_G(a) + (((int)CON_RGB_G(b) - (int)CON_RGB_G(a)) * f) / 25;
    int bl = (int)CON_RGB_B(a) + (((int)CON_RGB_B(b) - (int)CON_RGB_B(a)) * f) / 25;
    return CON_RGB(r, g, bl);
}

/* log2(size+1) scaled so ~1 MiB == 100 (eza --color-scale=size semantics) */
static int ls_heat_t(uint32_t size) {
    uint32_t s = size + 1, v = 0;
    while (s > 1) { s >>= 1; v++; }
    int t = (int)((v * 100u) / 20u);   /* 2^20 -> 100 */
    if (t > 100) t = 100;
    return t;
}

/* 10-char UNIX-ish permission string from FAT attributes */
static void ls_perm(const struct fs_raw_ent *e, char out[11]) {
    int dir = (e->attr & FAT_ATTR_DIR) != 0;
    int ro  = (e->attr & FAT_ATTR_RO) != 0;
    out[0] = dir ? 'd' : '-';
    out[1] = 'r';
    out[2] = ro ? '-' : 'w';
    out[3] = dir ? 'x' : '-';
    out[4] = 'r'; out[5] = '-'; out[6] = '-';
    out[7] = 'r'; out[8] = '-'; out[9] = '-';
    out[10] = 0;
}

/* print the permission string with r/w/x colourised (r blue, w yellow,
 * x green, '-' grey; the leading type char is directory-cyan) */
static void ls_perm_colored(const struct fs_raw_ent *e) {
    char p[11]; ls_perm(e, p);
    for (int i = 0; i < 10; i++) {
        if (i == 0)        con_set_fg_rgb(CON_RGB(0x3D,0xB7,0xFF));
        else if (p[i] == 'r') con_set_fg_rgb(CON_RGB(0x3D,0xB7,0xFF));
        else if (p[i] == 'w') con_set_fg_rgb(CON_RGB(0xF2,0xCC,0x60));
        else if (p[i] == 'x') con_set_fg_rgb(CON_RGB(0x7E,0xE7,0x87));
        else                  con_set_fg_rgb(CON_RGB(0x6B,0x79,0x8F));
        vga_putc(p[i]);
    }
    con_set_fg_rgb(pal_rgb(PAL_FG));
}

/* resolve an ls argument to an absolute dir path (mirrors cmd_tree) */
static void ls_dir_arg(const char *arg, char *dir, int cap) {
    while (*arg == ' ') arg++;
    if (!*arg || arg[0] == '/') {
        int n = 0;
        if (!*arg) fs_pwd(dir, cap);
        else { for (; *arg && n < cap - 1; arg++) dir[n++] = *arg; dir[n] = 0; }
        return;
    }
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    int n = 0;
    for (const char *q = pwd; *q && n < cap - 1; q++) dir[n++] = *q;
    if (n > 0 && dir[n - 1] != '/') dir[n++] = '/';
    for (; *arg && n < cap - 1; arg++) dir[n++] = *arg;
    dir[n] = 0;
}

/* Collect live entries via fs_foreach() -- one directory walk gives us
 * is_dir + name + real dirent size in a single pass.  This avoids the
 * fs_open_file() trap: that API reads the WHOLE file into a 4 KiB editor
 * buffer, which is both wrong for `ls` and unstable on many built-in .ce
 * files.  fs_enum_entries_raw() is also unsuitable (raw=1 -> insane only). */
struct ls_collect_ud { struct fs_raw_ent *out; int maxn; int n; };
static void ls_collect_cb(int is_dir, const char *name, uint32_t size, void *ud) {
    struct ls_collect_ud *u = (struct ls_collect_ud *)ud;
    if (!strcmp_(name, ".") || !strcmp_(name, "..")) return;
    if (u->n >= u->maxn) return;
    /* v2.68: old volumes were formatted before mkfs wrote VFAT LFNs;
     * APPDEVEL is the 8.3 alias of /appdevelop.  Show the intended name
     * until the volume is reformatted (new mkfs now writes the LFN). */
    if (!strcmp_(name, "APPDEVEL") || !strcmp_(name, "appdevel"))
        name = "appdevelop";
    struct fs_raw_ent *e = &u->out[u->n];
    int m = 0;
    for (; name[m] && m < (int)sizeof(e->name) - 1; m++) e->name[m] = name[m];
    e->name[m] = 0;
    e->attr = is_dir ? FAT_ATTR_DIR : 0;
    e->size = size;
    e->first = e->insane = e->slot = 0;
    u->n++;
}
static int ls_collect(const char *dir, struct fs_raw_ent *out, int maxn) {
    struct ls_collect_ud u = { out, maxn, 0 };
    if (fs_foreach(dir, ls_collect_cb, &u) != 0) return -1;
    /* sort: dirs first, then case-insensitive by name */
    for (int i = 1; i < u.n; i++) {
        struct fs_raw_ent key = out[i];
        int j = i - 1;
        while (j >= 0) {
            int kd = (key.attr & FAT_ATTR_DIR) != 0;
            int jd = (out[j].attr & FAT_ATTR_DIR) != 0;
            int swap = (jd == kd) ? ls_icmp_shell(out[j].name, key.name) > 0
                                  : (jd && !kd) ? 0 : 1;
            if (!swap) break;
            out[j + 1] = out[j]; j--;
        }
        out[j + 1] = key;
    }
    return u.n;
}

/* v2.68: sorted tree walker.  The old recursion printed only the first
 * directory because it mixed per-level callback state with a shared line
 * buffer; collect each level first, then emit in sorted order. */
static int ls_tree_walk2(const char *dir, int depth, int detail) {
    struct fs_raw_ent ents[64];
    int n = ls_collect(dir, ents, 64);
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        struct fs_raw_ent *e = &ents[i];
        int isdir = (e->attr & FAT_ATTR_DIR) != 0;
        tree_line_reset();
        for (int d = 0; d < depth; d++) tree_line_app("  ");
        tree_line_app(isdir ? "+- " : "- ");
        tree_line_app(e->name);
        if (isdir) tree_line_app("/");
        else if (detail) {
            char sz[12];
            ls_sz(e->size, sz);
            tree_line_app("  [");
            tree_line_app(sz);
            tree_line_app("]");
        }
        if (!pager_line(g_tree_line)) return 1;
        if (isdir && depth < 6) {
            char child[FS_PATH_MAX];
            int k = 0;
            for (const char *q = dir; *q && k < (int)sizeof(child) - 1; q++) child[k++] = *q;
            if (k == 0 || child[k - 1] != '/') child[k++] = '/';
            for (const char *q = e->name; *q && k < (int)sizeof(child) - 1; q++) child[k++] = *q;
            child[k] = 0;
            if (ls_tree_walk2(child, depth + 1, detail)) return 1;
        }
    }
    return 0;
}
static void ls_tree_walk(const char *dir, int depth, int detail) {
    ls_tree_walk2(dir, depth, detail);
}

static void cmd_ls(char *arg) {
    int longfmt = 0, tree = 0, scale = 0, raw = 0, blocks_explicit = 0;
    char blocks[64] = "perm,size,name";
    char path[FS_PATH_MAX] = "";
    while (*arg == ' ') arg++;
    char tok[80];
    while (*arg) {
        int n = 0;
        while (*arg && *arg != ' ') { if (n < (int)sizeof(tok) - 1) tok[n++] = *arg; arg++; }
        tok[n] = 0;
        while (*arg == ' ') arg++;
        if (!strcmp_(tok, "-l"))                 longfmt = 1;
        else if (!strcmp_(tok, "-t"))            tree = 1;
        else if (!strcmp_(tok, "--raw"))         raw = 1;
        else if (!strcmp_(tok, "--color-scale") ||
                 !strcmp_(tok, "--color-size"))  { scale = 1; longfmt = 1; }
        else if (!strncmp_(tok, "--color-scale=", 14) ||
                 !strncmp_(tok, "--blocks=", 9)) {
            if (tok[2] == 'c' && !strncmp_(tok, "--color-scale=", 14)) {
                scale = 1; longfmt = 1;
            } else {
                blocks_explicit = 1;
                const char *v = tok + 9;
                int k = 0;
                /* v2.68.3: keep ',' separators. The header builder advances
                 * token-by-token via while(*p != ','), while the row builder
                 * steps a fixed 4 chars per token. Dropping commas here made
                 * --blocks=size,name render the NAME data column but omit
                 * its header. Only spaces need filtering. */
                for (; *v && k < (int)sizeof(blocks) - 1; v++)
                    if (*v != ' ') blocks[k++] = *v;
                blocks[k] = 0; longfmt = 1;
            }
        } else if (tok[0] != '-') {
            int k = 0;
            for (; tok[k] && k < (int)sizeof(path) - 1; k++) path[k] = tok[k];
            path[k] = 0;
        }
    }

    char dir[FS_PATH_MAX];
    ls_dir_arg(path, dir, sizeof(dir));

    /* tree modes: -t / -lt (lsd semantics) */
    if (tree) {
        if (!fs_fs_ready()) {
            vga_write(trs("ls: no FAT32 volume (run 'format' first)\n", "ls: 没有 FAT32 卷 (run 'format' first)\n"));
            return;
        }
        char root[FS_PATH_MAX + 2];
        int rn = 0;
        for (const char *p = dir; *p && rn < (int)sizeof(root) - 2; p++) root[rn++] = *p;
        root[rn] = 0;
        pager_line(root);
        ls_tree_walk(dir, 0, longfmt);   /* -lt adds [size] per node */
        return;
    }

    if (!fs_fs_ready()) {
        vga_write(trs("ls: no FAT32 volume (run 'format' first)\n", "ls: 没有 FAT32 卷 (run 'format' first)\n"));
        return;
    }
    struct fs_raw_ent ents[32];   /* keep small: kernel stack is limited */
    int n = ls_collect(dir, ents, 32);
    if (n < 0) {
        vga_write(trs("ls: cannot read directory (run 'format' first)\n", "ls: 无法read 目录 (run 'format' first)\n"));
        return;
    }
    int cols = 0;
    for (const char *p = blocks; *p; p++) {
        if (*p != ',' && *p != ' ') cols++;
    }
    if (blocks_explicit) {
        char hdr[64]; int ho = 0;
        const char *p = blocks;
        while (*p && ho < (int)sizeof(hdr) - 2) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            if (ho) hdr[ho++] = ' ';
            for (int q = 0; q < 4 && p[q]; q++) {
                char c = p[q];
                if (c >= 'a' && c <= 'z') c -= 32;
                hdr[ho++] = c;
            }
            while (*p && *p != ',') p++;
        }
        hdr[ho] = 0;
        con_set_fg_rgb(pal_rgb(PAL_DIM));
        pager_line(hdr);
        con_set_fg_rgb(pal_rgb(PAL_FG));
    }

    for (int i = 0; i < n; i++) {
        struct fs_raw_ent *e = &ents[i];
        int isdir = (e->attr & FAT_ATTR_DIR) != 0;
        char sz[12];
        int sn = ls_sz(e->size, sz);

        if (!longfmt) {
            /* default grid: one name per line, dirs cyan, .ce green */
            int isce = !isdir && slen_(e->name) >= 3 &&
                       !strcmp_(e->name + slen_(e->name) - 3, ".ce");
            con_set_fg_rgb(isdir ? CON_RGB(0x3D,0xB7,0xFF)
                          : isce ? CON_RGB(0x7E,0xE7,0x87)
                          : (e->attr & FAT_ATTR_RO) ? CON_RGB(0x8B,0x95,0xA7)
                          : CON_RGB(0xF2,0xCC,0x60));
            if (scale) con_set_fg_rgb(ls_heat(ls_heat_t(e->size)));
            pager_line(e->name);
            con_set_fg_rgb(pal_rgb(PAL_FG));
            continue;
        }

        /* long / blocks: emit only the requested columns, in order.
         * When 'perm' is the FIRST column (the default and -l), print it
         * through ls_perm_colored() so r/w/x get their semantic colours;
         * the rest of the columns stream as one line.  If perm appears
         * elsewhere we stay monochrome for correct column alignment. */
        int perm_first = (!strncmp_(blocks, "perm", 4) &&
                          (blocks[4] == ',' || blocks[4] == 0));
        if (perm_first) { ls_perm_colored(e); vga_putc(' '); }
        char line[FS_NAME_MAX + 64];
        int o = 0;
        const char *p = blocks;
        while (*p) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            if (!strncmp_(p, "perm", 4)) {
                if (!perm_first) { ls_perm(e, line + o); o += 10; line[o++] = ' '; }
                p += 4;
            } else if (!strncmp_(p, "size", 4)) {
                for (int q = sn; q < 8; q++) line[o++] = ' ';
                for (int q = 0; q < sn; q++) line[o++] = sz[q];
                line[o++] = ' '; p += 4;
            } else if (!strncmp_(p, "name", 4)) {
                for (const char *q = e->name; *q && o < (int)sizeof(line) - 1; q++) line[o++] = *q;
                if (isdir) line[o++] = '/';
                p += 4;
            } else {
                /* unknown block: skip its token */
                while (*p && *p != ',') p++;
            }
        }
        line[o] = 0;
        con_set_fg_rgb(pal_rgb(scale ? PAL_FG : (isdir ? PAL_PROMPT_PATH : PAL_FG)));
        if (scale) con_set_fg_rgb(ls_heat(ls_heat_t(e->size)));
        pager_line(line);
        con_set_fg_rgb(pal_rgb(PAL_FG));
    }

    char sum[80];
    int z = 0;
    const char *tail = " entries";
    const char *hd = "--- ";
    for (const char *q = hd; *q; q++) sum[z++] = *q;
    z += ls_u2a((uint32_t)n, sum + z);
    for (const char *q = tail; *q; q++) sum[z++] = *q;
    if (raw) {
        const char *r = " (--raw)";
        for (const char *q = r; *q; q++) sum[z++] = *q;
    }
    sum[z] = 0;
    con_set_fg_rgb(pal_rgb(PAL_FG));
    pager_line(sum);
    (void)cols;
}

/* ---------- v2.55: fsck directory integrity scan ---------------------- */
struct fsck_ud { int bad; int repairable; int purge; };

static void fsck_hexbyte(uint8_t b) {
    const char *hd = "0123456789ABCDEF";
    vga_putc(hd[(b >> 4) & 0xF]);
    vga_putc(hd[b & 0xF]);
}

static const char *fsck_kind_name(int k) {
    switch (k) {
    case FSCK_BADNAME:    return "bad-name";
    case FSCK_ORPHAN_LFN: return "orphan-lfn";
    case FSCK_BADCLUST:   return "bad-cluster";
    }
    return "?";
}

static void fsck_report_cb(const fat_fsck_t *e, void *ud) {
    struct fsck_ud *u = (struct fsck_ud *)ud;
    u->bad++;
    if (e->repairable) u->repairable++;
    vga_write("fsck: ");
    vga_write(e->path);
    vga_write(" slot="); print_int(e->slot);
    vga_write(" first=0x"); fsck_hexbyte(e->first);
    vga_write(" attr=0x"); fsck_hexbyte(e->attr);
    vga_write(" kind="); vga_write(fsck_kind_name(e->kind));
    vga_write(" raw=");
    for (int k = 0; k < 11; k++) { fsck_hexbyte(e->raw[k]); vga_putc(' '); }
    vga_write(u->purge ? " [purged]\n" : "\n");
}

static void cmd_fsck(char *arg) {
    int purge = 0;
    while (*arg == ' ') arg++;
    if (!strcmp_(arg, "--purge") || !strcmp_(arg, "-p")) purge = 1;

    int r = fat_verify_mirror();
    if (r < 0)      vga_write(trs("fsck: ATA error while comparing FAT mirrors\n", "fsck: ATA 错误 while comparing FAT mirrors\n"));
    else if (r == 0) vga_write("fsck: FAT mirror OK\n");
    else {
        vga_write("fsck: repaired "); print_int(r);
        vga_write(" FAT sector(s) from mirror #1\n");
    }

    struct fsck_ud u = { 0, 0, purge };
    int n = fat_fsck_scan(fat_root(), fsck_report_cb, &u, purge);
    if (n == 0) {
        vga_write("fsck: directory tree OK\n");
    } else {
        vga_write("fsck: "); print_int(u.bad);
        vga_write(trs(" bad entries, ", " 错误的 entries, ")); print_int(u.repairable);
        vga_write(" repairable, "); print_int(u.bad - u.repairable);
        vga_write(" need rebuild (suggest directory rebuild)\n");
    }
}

/* ---------- v2.23 environment variables (session table + /etc/env) -------
 * Windows/Linux both expose process/session environment variables; CatOS's
 * shell is ring0 and single-session, so a flat table is honest here.
 *   export KEY=value   set (empty value deletes, like POSIX unset semantics)
 *   export KEY=        delete
 *   export / env       list
 *   unset KEY          delete
 *   $KEY / ${KEY}      expanded in typed lines and autostart scripts
 *   PATH               drives bare-NAME program lookup (default /bin)
 * Persistence: /etc/env, one KEY=VALUE per line, saved on every change
 * (same on-demand style as /etc/lang).  CE apps do NOT inherit env yet --
 * passing an environment block needs an append-only ABI slot, tracked in
 * PROGRESS as the next step. */
#define ENV_MAX     16
#define ENV_KEY_MAX 16
#define ENV_VAL_MAX 64
typedef struct { char key[ENV_KEY_MAX]; char val[ENV_VAL_MAX]; } env_rec_t;
static env_rec_t g_env[ENV_MAX];
static int g_env_used[ENV_MAX];

static int env_key_ok(const char *k) {
    if (!k || !*k) return 0;
    for (int i = 0; k[i]; i++) {
        char c = k[i];
        int ok = (c == '_') ||
                 (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (i > 0 && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return slen_(k) < ENV_KEY_MAX;
}

static const char *env_get_(const char *key) {
    if (!env_key_ok(key)) return 0;
    for (int i = 0; i < ENV_MAX; i++)
        if (g_env_used[i] && !strcmp_(g_env[i].key, key)) return g_env[i].val;
    return 0;
}

/* returns 0 set, 1 deleted, -1 table full / bad key */
static int env_set_(const char *key, const char *val) {
    if (!env_key_ok(key)) return -1;
    int slot = -1;
    for (int i = 0; i < ENV_MAX; i++) {
        if (g_env_used[i] && !strcmp_(g_env[i].key, key)) { slot = i; break; }
        if (slot < 0 && !g_env_used[i]) slot = i;
    }
    if (slot < 0) return -1;
    if (val && *val) {
        if (slen_(val) >= ENV_VAL_MAX || slen_(key) >= ENV_KEY_MAX) return -1;
        int i = 0;
        for (; key[i]; i++) g_env[slot].key[i] = key[i];
        g_env[slot].key[i] = 0;
        for (i = 0; val[i]; i++) g_env[slot].val[i] = val[i];
        g_env[slot].val[i] = 0;
        g_env_used[slot] = 1;
        return 0;
    }
    g_env_used[slot] = 0;               /* empty value deletes */
    return 1;
}

static void env_list(void) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (!g_env_used[i]) continue;
        vga_write(g_env[i].key);
        vga_write("=");
        vga_write(g_env[i].val);
        vga_write("\n");
    }
}

int env_save(void) {
    /* v2.67: the hive is canonical.  Rewrite HKLM\Environment wholesale
     * (delete + recreate) so a variable that was unset stops existing;
     * a stale key would otherwise keep coming back on the next boot.
     * /etc/env is still mirrored for old tools, marked as a legacy copy. */
    hive_delkey("HKLM\\Environment");
    hive_mkkey("HKLM\\Environment");
    for (int i = 0; i < ENV_MAX; i++) {
        if (!g_env_used[i]) continue;
        char path[HIVE_PATH_MAX];
        int o = 0;
        const char *base = "HKLM\\Environment\\";
        for (int j = 0; base[j] && o < HIVE_PATH_MAX - 1; j++) path[o++] = base[j];
        for (int j = 0; g_env[i].key[j] && o < HIVE_PATH_MAX - 1; j++) path[o++] = g_env[i].key[j];
        path[o] = 0;
        hive_set_sz(path, g_env[i].val);       /* refuses over-long values */
    }
    hive_save();

    static char buf[2048];
    int o = 0;
    for (int i = 0; i < ENV_MAX; i++) {
        if (!g_env_used[i]) continue;
        for (int j = 0; g_env[i].key[j]; j++) buf[o++] = g_env[i].key[j];
        buf[o++] = '=';
        for (int j = 0; g_env[i].val[j]; j++) buf[o++] = g_env[i].val[j];
        buf[o++] = '\n';
    }
    if (o == 0) return 0;
    return fs_write_text("/etc/env", buf, (uint32_t)o) == 0 ? 0 : -1;
}

void env_init(void) {
    for (int i = 0; i < ENV_MAX; i++) g_env_used[i] = 0;
    if (!fs_fs_ready()) return;

    /* v2.67: primary source = HKLM\Environment in the hive. */
    int used = hive_child_count("HKLM\\Environment");
    if (used > 0) {
        for (int i = 0; i < used; i++) {
            char name[HIVE_NAME_MAX];
            uint8_t kind;
            if (hive_child_at("HKLM\\Environment", i, name, HIVE_NAME_MAX, &kind) != HIVE_OK) break;
            if (kind != HIVE_SZ) continue;
            char path[HIVE_PATH_MAX];
            hive_child_path("HKLM\\Environment", i, path, HIVE_PATH_MAX);
            env_set_(name, hive_sz(path, ""));
        }
    } else {
        /* legacy fallback: adopt /etc/env on the very first boot without
         * a hive (hive_init() normally already imported it). */
        static char buf[2048];
        int n = fs_read_text("/etc/env", buf, (int)sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            char *q = buf;
            while (*q) {
                char *line = q;
                while (*q && *q != '\n') q++;
                if (*q) *q++ = 0;
                char *eq = line;
                while (*eq && *eq != '=') eq++;
                if (!*eq) continue;
                *eq = 0;
                env_set_(line, eq + 1);
            }
            env_save();
        }
    }
    if (!env_get_("PATH")) env_set_("PATH", "/bin");
    if (hive_child_count("HKLM\\Environment") == 0) env_save();
}

/* expand $KEY / ${KEY} in place.  Unknown names expand to empty (sh
 * semantics).  "$$" is a literal '$'. */
static void env_expand(char *line, int cap) {
    char out[256];
    int o = 0;
    for (int i = 0; line[i] && o < (int)sizeof(out) - 1; ) {
        if (line[i] != '$') { out[o++] = line[i++]; continue; }
        if (line[i + 1] == '$') { out[o++] = '$'; i += 2; continue; }
        /* v2.65: $? expands to the last command's exit status (single digit). */
        if (line[i + 1] == '?') {
            if (o < (int)sizeof(out) - 1) out[o++] = (char)('0' + (g_last_rc % 10));
            i += 2; continue;
        }
        int brace = 0;
        int j = i + 1;                      /* skip the '$' itself */
        if (line[j] == '{') { brace = 1; j++; }
        char key[ENV_KEY_MAX];
        int k = 0;
        for (; line[j] && k < ENV_KEY_MAX - 1; j++) {
            char c = line[j];
            int ok = (c == '_') ||
                     (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (k > 0 && c >= '0' && c <= '9');
            if (!ok) break;
            key[k++] = c;
        }
        key[k] = 0;
        if (brace && line[j] == '}') j++;
        i = j;
        if (!k) { if (o < (int)sizeof(out) - 1) out[o++] = '$'; continue; }
        const char *v = env_get_(key);
        if (v) for (; *v && o < (int)sizeof(out) - 1; v++) out[o++] = *v;
    }
    out[o] = 0;
    int j = 0;
    for (; out[j] && j < cap - 1; j++) line[j] = out[j];
    line[j] = 0;
}

static void cmd_export(char *arg) {
    if (!arg || !*arg) { env_list(); return; }
    /* #122: the env table is global and persisted to /etc/env; mutating it
     * is a system-config write, so it goes through the same admin gate as
     * useradd/chcaps.  (There is no per-session env store yet.) */
    if (!session_is_admin()) { vga_write(trs("export: permission denied\n", "export: 权限不足\n")); return; }
    char *eq = arg;
    while (*eq && *eq != '=') eq++;
    if (!*eq) { vga_write(trs("usage: export KEY=VALUE | export KEY= | export | env\n", "用法：export KEY=VALUE | export KEY= | export | env\n")); return; }
    *eq = 0;
    int r = env_set_(arg, eq + 1);
    if (r == -1) { vga_write(trs("export: bad key or table full or value too long\n", "export: 错误的 key or table full or value too long\n")); return; }
    if (r == 0) vga_write("export: "); else vga_write("unset: ");
    vga_write(arg);
    if (r == 0) { vga_write("="); vga_write(eq + 1); }
    vga_write("\n");
    if (env_save() != 0)
        vga_write(tr(L_ENV_NOSAVE));
}

static void cmd_unset(char *arg) {
    /* #122: global persisted env -- admin gate, same rationale as export. */
    if (!session_is_admin()) { vga_write(trs("unset: permission denied\n", "unset: 权限不足\n")); return; }
    if (!arg || !*arg) { vga_write(trs("usage: unset KEY\n", "用法：unset KEY\n")); return; }
    char key[ENV_KEY_MAX];
    int i = 0;
    for (; arg[i] && i < ENV_KEY_MAX - 1; i++) key[i] = arg[i];
    key[i] = 0;
    if (env_set_(key, "") != 1) { vga_write(trs("unset: no such variable\n", "unset: 没有这个变量\n")); return; }
    vga_write("unset: ");
    vga_write(key);
    vga_write("\n");
    if (env_save() != 0)
        vga_write(tr(L_ENV_NOSAVE));
}

/* ---------- v2.25 glyph scale (`size`, persisted in /etc/size) ---------- */
static const char *size_name(int s) {
    if (s == 0) return "compact  (8x8 cell, 100x75)";
    if (s == 2) return "large    (16x32 cell, 50x18)";
    return "normal   (8x16 cell, 100x37)";
}

static void size_apply(int s) {
    if (con_set_cell_size(s) != 0) return;
    /* #122: the cell-size default is a global persisted preference
     * (/etc/size); only admins may write it.  The current session still
     * switches scale for non-root, it just is not saved across reboots. */
    if (session_is_admin()) {
        char b[2] = { (char)('0' + s), 0 };
        hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\FontSize", b); hive_save();
        fs_write_text("/etc/size", b, 1);   /* legacy mirror */
    }
}

/* Re-apply the persisted scale when entering vbe24 (boot default 1). */
static void size_restore(void) {
    const char *v = hive_sz("HKLM\\Software\\CatOS\\Terminal\\FontSize", "");
    char c = 0;
    if (v && *v) c = v[0];
    else {
        char b[8];
        int n = fs_read_text("/etc/size", b, (int)sizeof(b) - 1);
        if (n >= 1) { c = b[0]; b[n] = 0; hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\FontSize", b); hive_save(); }
    }
    if (c >= '0' && c <= '2') con_set_cell_size(c - '0');
}

/* ---------- v2.56 boot display mode (/etc/videomode) -----------------------
 * One token per line: text | gfx | vbe | gfx24 (vbe24 accepted as an alias).
 * Default is gfx24 (800x600x24).  The parser is deliberately fault-tolerant:
 * a missing/empty file, an unknown value, an over-long line or binary bytes
 * all fall back to the default instead of panicking.  Field lengths are
 * bounded so a hostile file can never overflow the stack buffers. */
#define VIDEOMODE_PATH "/etc/videomode"

/* Re-printed after the autostart banner: a boot-time graphics fallback must
 * stay visible on the text console, not scroll away under the banner. */
static char g_fallback_note[128];


/* v2.72: minimal "append a decimal int" for the boot geometry line.  print_int
 * writes straight to the console, so it cannot be used to build a buffer. */
static void geom_num(char *dst, int max, int v) {
    char t[12]; int i = 0;
    if (v < 0) { strcat_b(dst, max, "-"); v = -v; }
    if (!v) t[i++] = '0';
    while (v > 0 && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    char r[12]; int j = 0;
    while (i > 0) r[j++] = t[--i];
    r[j] = 0;
    strcat_b(dst, max, r);
}

static int videomode_parse(void) {
    char buf[32];
    /* v2.62: registry first, legacy file as fallback/migration source. */
    const char *rv = hive_sz("HKLM\\System\\VideoMode", "");
    if (rv && *rv) {
        int k = 0;
        while (*rv && k < (int)sizeof(buf) - 1) buf[k++] = *rv++;
        buf[k] = 0;
    } else {
        int n = fs_read_text(VIDEOMODE_PATH, buf, (int)sizeof(buf) - 1);
        if (n <= 0) return CON_MODE_VBE24;  /* absent / unreadable -> default */
        buf[n] = 0;
        hive_set_sz("HKLM\\System\\VideoMode", buf); hive_save(); /* migrate */
    }

    int i = 0;
    while (buf[i] && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' ||
                      buf[i] == '\n' || (unsigned char)buf[i] < 0x20))
        i++;

    char tok[16];
    int k = 0;
    while (buf[i] && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' &&
           buf[i] != '\n' && (unsigned char)buf[i] >= 0x20) {
        if (k < (int)sizeof(tok) - 1) tok[k++] = buf[i];
        i++;                                /* overflow: keep skipping, cap */
    }
    tok[k] = 0;

    if (!strcmp_(tok, "text")) return CON_MODE_TEXT;
    if (!strcmp_(tok, "gfx"))  return CON_MODE_GFX;
    if (!strcmp_(tok, "vbe"))  return CON_MODE_VBE;
    if (!strcmp_(tok, "gfx24") || !strcmp_(tok, "vbe24")) return CON_MODE_VBE24;
    return CON_MODE_VBE24;                  /* unknown/garbage -> default     */
}

/* Apply the persisted boot mode before the shell banner prints.  A graphics
 * back-end that fails to probe (no VBE/SVGA: e.g. -vga cirrus or a VMware
 * guest whose SVGA init fails) MUST land back in text with a reason on
 * screen -- never a black, silent boot. */
static void boot_apply_videomode(void) {
    int want = videomode_parse();
    if (want == CON_MODE_TEXT) return;      /* GRUB already set text mode 03 */

    int r = con_request_mode(want, CON_ORIGIN_KERNEL);
    if (r == 0) {
        if (want == CON_MODE_VBE24) size_restore();
        /* v2.72: the mode switch does its own clear+redraw, but it runs
         * while the grid is still the pre-switch one; every later stage
         * (scale restore / reflow) can leave a corner of the frame in the
         * pre-theme backdrop colour.  One clear+repaint here, after the
         * geometry is final, is the last word on the backdrop -- and it is
         * what makes the window edge-to-edge one colour. */
        con_repaint_frame();
        vga_write("[display] boot mode: ");
        vga_write(con_mode_name(want));
        /* v2.72: state the geometry we really got, so a host/window that
         * disagrees with the surface is diagnosable from the boot log
         * instead of from a screenshot.  `vdiag` repeats it on demand. */
        {
            char g[64];
            int w = con_info(CON_INFO_PIXEL_W), h = con_info(CON_INFO_PIXEL_H);
            int c = con_info(CON_INFO_COLS), r = con_info(CON_INFO_ROWS);
            int cw = con_info(CON_INFO_CELL_W), chh = con_info(CON_INFO_CELL_H);
            g[0] = 0;
            strcat_b(g, (int)sizeof(g), " (");
            geom_num(g, (int)sizeof(g), w);   strcat_b(g, (int)sizeof(g), "x");
            geom_num(g, (int)sizeof(g), h);   strcat_b(g, (int)sizeof(g), ", ");
            geom_num(g, (int)sizeof(g), c);   strcat_b(g, (int)sizeof(g), "x");
            geom_num(g, (int)sizeof(g), r);   strcat_b(g, (int)sizeof(g), " cells @");
            geom_num(g, (int)sizeof(g), cw);  strcat_b(g, (int)sizeof(g), "x");
            geom_num(g, (int)sizeof(g), chh); strcat_b(g, (int)sizeof(g), ")");
            vga_write(g);
        }
        vga_write("\n");
        return;
    }

    /* switch_backend() already restored the previous (text) back-end when the
     * new one's enter() failed; con_force_text() is the safety net for any
     * other failure path so the console can never stay in a dead mode. */
    con_force_text();
    const char *why;
    if (r == CON_ENODEV)      why = "no VBE/SVGA graphics device detected";
    else if (r == CON_EINVAL) why = "invalid boot mode requested";
    else                      why = "graphics back-end initialisation failed";
    g_fallback_note[0] = 0;
    strcat_b(g_fallback_note, sizeof(g_fallback_note),
             "[display] WARNING: ");
    strcat_b(g_fallback_note, sizeof(g_fallback_note),
             con_mode_name(want));
    strcat_b(g_fallback_note, sizeof(g_fallback_note),
             " unavailable (");
    strcat_b(g_fallback_note, sizeof(g_fallback_note), why);
    strcat_b(g_fallback_note, sizeof(g_fallback_note),
             "); safe fallback to 80x25 text mode.");
    vga_write(g_fallback_note);
    vga_write("\n");
}

static void cmd_size(const char *arg) {
    if (con_get_mode() != CON_MODE_VBE24) {
        vga_write("size: only supported in vbe24 mode (run `gfx24` first)\n");
        return;
    }
    int s = con_info(CON_INFO_CELL_SCALE);
    if (!*arg) {
        vga_write("size: current "); vga_write(size_name(s));
        vga_write(trs("\nusage: size compact|normal|large  (rows x cols change with\n", "\n用法：size compact|normal|large  (rows x cols change with\n"));
        vga_write("       the cell size; layout is re-flowed, choice persists)\n");
        return;
    }
    if (!strcmp_(arg, "compact") || !strcmp_(arg, "small") ||
        arg[0] == '0')      size_apply(0);
    else if (!strcmp_(arg, "normal") || arg[0] == '1') size_apply(1);
    else if (!strcmp_(arg, "large") || !strcmp_(arg, "big") ||
        arg[0] == '2')      size_apply(2);
    else { vga_write("size: compact|normal|large\n"); return; }
    vga_write("size: now "); vga_write(size_name(con_info(CON_INFO_CELL_SCALE)));
    vga_write(" -- grid "); print_int(con_cols());
    vga_write(" x "); print_int(con_rows()); vga_write("\n");
}


/* ---------- v2.25 full-screen user administration UI (`useredit`) -------
 * Full-screen takeover: con_screen_push() saves the live grid, the TUI
 * owns every row until Q/Esc, con_screen_pop() restores the shell view
 * verbatim.  Requires CAP_USERADM (or root). */
static void ue_line(int y, int x, const char *txt, uint8_t fg, uint8_t bg) {
    if (y < 0 || y >= con_rows()) return;
    con_set_style(fg, bg, 0);
    con_write_at((uint8_t)x, (uint8_t)y, txt);
}

static void ue_pad(int y, uint8_t bg) {
    char blanks[128];
    int w = con_cols();
    if (w > 120) w = 120;
    for (int i = 0; i < w; i++) blanks[i] = ' ';
    blanks[w] = 0;
    con_set_style(CON_LIGHT_GREY, bg, 0);
    con_write_at(0, (uint8_t)y, blanks);
}

static void ue_frame(const char *title) {
    for (int y = 0; y < con_rows(); y++) ue_pad(y, CON_BLUE);
    ue_line(0, 2, "+-------------------------------------------------------------+",
            CON_YELLOW, CON_BLUE);
    ue_line(1, 2, "|                                                             |",
            CON_YELLOW, CON_BLUE);
    char hdr[80];
    int o = 0;
    for (const char *q = "| "; *q && o < 78; q++) hdr[o++] = *q;
    for (const char *q = title; *q && o < 77; q++) hdr[o++] = *q;
    while (o < 63) hdr[o++] = ' ';
    for (const char *q = "|"; *q && o < 78; q++) hdr[o++] = *q;
    hdr[o] = 0;
    ue_line(1, 2, hdr, CON_YELLOW, CON_BLUE);
}

static const char *ue_pwmark(const user_rec_t *u) {
    return user_pw_is_set(u) ? "yes" : "no ";
}

static void ue_draw_list(int sel) {
    ue_frame("CatOS user administration (v2.25 fine-grained caps)");
    ue_line(2, 4, "  # name        uid gid caps      password",
            CON_WHITE, CON_BLUE);
    char ln[80];
    int row = 3;
    for (int i = 0; i < 10 && row < con_rows() - 3; i++) {
        user_rec_t *u = user_at(i);
        if (!u || !u->used) continue;
        int o = 0;
        ln[o++] = u->uid == (int)sel ? '>' : ' ';
        ln[o++] = ' ';
        char num[12];
        int n = 0;
        int v = i;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        if (n < 2) ln[o++] = '0';
        while (n > 0) ln[o++] = num[--n];
        ln[o++] = ' ';
        for (const char *q = u->name; *q && o < 14; q++) ln[o++] = *q;
        while (o < 16) ln[o++] = ' ';
        n = 0; v = u->uid;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (n > 0) ln[o++] = num[--n];
        ln[o++] = ' ';
        n = 0; v = u->gid;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (n > 0) ln[o++] = num[--n];
        while (o < 24) ln[o++] = ' ';
        for (int k = 0; k < 8; k++) {
            uint32_t c = user_caps_of(u->uid);
            ln[o++] = "0123456789ABCDEF"[(c >> (28 - k * 4)) & 0xF];
        }
        ln[o++] = ' ';
        for (const char *q = ue_pwmark(u); *q && o < 76; q++) ln[o++] = *q;
        ln[o] = 0;
        ue_line(row, 2, ln,
                u->uid == (int)sel ? CON_BLACK : CON_LIGHT_GREY,
                u->uid == (int)sel ? CON_YELLOW : CON_BLUE);
        row++;
    }
    ue_line(con_rows() - 2, 2,
            " arrows=select  N=new  E=edit caps  P=password  D=delete  Q=quit ",
            CON_BLACK, CON_LIGHT_GREY);
}

static void ue_draw_caps(const char *name, uint32_t caps) {
    ue_frame("editing capabilities (space toggles, R=preset, Esc=done)");
    static const struct { uint32_t bit; const char *q; } W[] = {
        { CAP_NET,     "network (http/dns/udp/ping)" },
        { CAP_KILL,    "kill other users' tasks" },
        { CAP_FSADMIN, "fs admin broad (format+fsck)" },
        { CAP_FORMAT,  "format disks (fine)" },
        { CAP_FSCK,    "fsck repair (fine)" },
        { CAP_AUDIO,   "audio playback" },
        { CAP_USERADM, "user admin" },
        { CAP_DEVICE,  "raw device broad" },
        { CAP_RAWIO,   "raw I/O (fine)" },
        { CAP_HWINFO,  "hw info read (fine)" },
        { CAP_SYS,     "system broad (reboot+diag)" },
        { CAP_REBOOT,  "reboot (fine)" },
        { CAP_DIAG,    "kernel introspection (fine)" },
    };
    char ln[80];
    int row = 3;
    for (int i = 0; i < (int)(sizeof(W) / sizeof(W[0])) && row < con_rows() - 3; i++) {
        int o = 0;
        ln[o++] = '[';
        ln[o++] = (caps & W[i].bit) ? 'x' : ' ';
        ln[o++] = ']';
        ln[o++] = ' ';
        for (const char *q = W[i].q; *q && o < 60; q++) ln[o++] = *q;
        ln[o] = 0;
        ue_line(row, 4, ln, (caps & W[i].bit) ? CON_LIGHT_GREEN : CON_LIGHT_GREY,
                CON_BLUE);
        row++;
    }
    char st[80];
    int o = 0;
    for (const char *q = name; *q && o < 30; q++) st[o++] = *q;
    for (const char *q = "  caps=0x"; *q && o < 62; q++) st[o++] = *q;
    for (int k = 0; k < 8 && o < 70; k++)
        st[o++] = "0123456789ABCDEF"[(caps >> (28 - k * 4)) & 0xF];
    st[o] = 0;
    ue_line(con_rows() - 2, 2, st, CON_BLACK, CON_LIGHT_GREY);
}

static void ue_edit_caps(const char *name) {
    user_rec_t *u = user_by_name(name);
    if (!u) return;
    uint32_t caps = user_caps_of(u->uid);
    static const uint32_t bits[] = {
        CAP_NET, CAP_KILL, CAP_FSADMIN, CAP_FORMAT, CAP_FSCK, CAP_AUDIO,
        CAP_USERADM, CAP_DEVICE, CAP_RAWIO, CAP_HWINFO, CAP_SYS,
        CAP_REBOOT, CAP_DIAG
    };
    int nb = (int)(sizeof(bits) / sizeof(bits[0]));
    int cur = 0;
    for (;;) {
        ue_draw_caps(name, caps);
        int k = kb_getkey();
        if (k == KB_UP)   { if (--cur < 0) cur = nb - 1; }
        else if (k == KB_DOWN) { if (++cur >= nb) cur = 0; }
        else if (k == ' ') caps ^= bits[cur];
        else if (k == 27 || k == '\n') {
            /* #181: strip any bit the non-root issuer does not hold before
             * persisting; the interactive toggle cannot mint new powers. */
            if (g_uid != UID_ROOT) caps &= g_caps;
            user_set_caps(name, caps);
            if (user_save() != 0) vga_write(trs("useredit: persist failed\n", "useredit: persist 失败\n"));
            return;
        }
        else if (k == 'r' || k == 'R') {
            ue_line(con_rows() - 1, 2,
                    " preset: admin/operator/developer/viewer/guest: ",
                    CON_WHITE, CON_BLUE);
            char p[16];
            kb_readline(p, sizeof(p));
            uint32_t pc;
            if (p[0] && user_preset_caps(p, &pc) == 0) caps = pc;
        }
    }
}

static void ue_password(const char *name) {
    char p1[64], p2[64];
    ue_line(con_rows() - 1, 2, " new password (masked): ", CON_WHITE, CON_BLUE);
    kb_readline_masked(p1, sizeof(p1));
    ue_line(con_rows() - 1, 2, " retype:                 ", CON_WHITE, CON_BLUE);
    kb_readline_masked(p2, sizeof(p2));
    if (!p1[0] || strcmp_(p1, p2) != 0) {
        ue_line(con_rows() - 1, 2, " password NOT set (empty or mismatch) ",
                CON_LIGHT_RED, CON_BLUE);
        return;
    }
    user_set_pw(name, p1);
    if (user_save() != 0)
        ue_line(con_rows() - 1, 2, " persist FAILED ", CON_LIGHT_RED, CON_BLUE);
    else
        ue_line(con_rows() - 1, 2, " password set + persisted ", CON_LIGHT_GREEN, CON_BLUE);
}

static void cmd_useredit(void) {
    if (!session_is_admin()) {
        vga_write(trs("useredit: permission denied (need CAP_USERADM)\n", "useredit: 权限不足 (need CAP_USERADM)\n"));
        return;
    }
    if (con_screen_push() != 0) { vga_write(trs("useredit: push failed\n", "useredit: push 失败\n")); return; }
    int sel = UID_USER;
    for (;;) {
        ue_draw_list(sel);
        int k = kb_getkey();
        if (k == KB_UP) {
            int best = -1;
            for (int i = 0; i < 10; i++) {
                user_rec_t *u = user_at(i);
                if (u && u->used && u->uid < sel && u->uid > best) best = u->uid;
            }
            if (best >= 0) sel = best;
        } else if (k == KB_DOWN) {
            int best = -1;
            for (int i = 0; i < 10; i++) {
                user_rec_t *u = user_at(i);
                if (u && u->used && u->uid > sel &&
                    (best < 0 || u->uid < best)) best = u->uid;
            }
            if (best >= 0) sel = best;
        } else if (k == 'q' || k == 'Q' || k == 27) break;
        else if (k == 'e' || k == 'E') {
            user_rec_t *u = user_by_uid(sel);
            if (u && u->uid != UID_ROOT) ue_edit_caps(u->name);
        } else if (k == 'p' || k == 'P') {
            user_rec_t *u = user_by_uid(sel);
            if (u) ue_password(u->name);
        } else if (k == 'd' || k == 'D') {
            user_rec_t *u = user_by_uid(sel);
            if (u && u->uid != UID_ROOT) {
                user_del(u->name);
                user_save();
                sel = UID_ROOT;
            }
        } else if (k == 'n' || k == 'N') {
            ue_line(con_rows() - 1, 2, " new user name: ", CON_WHITE, CON_BLUE);
            char nm[24], uid[8];
            kb_readline(nm, sizeof(nm));
            if (nm[0] && !user_by_name(nm)) {
                ue_line(con_rows() - 1, 2, " uid (>=100):   ", CON_WHITE, CON_BLUE);
                kb_readline(uid, sizeof(uid));
                int uidv = uid[0] ? atoi_(uid) : 101;
                if (uidv < 100) uidv = 101;
                char home[48];
                int o = 0;
                for (const char *q = "/home/"; *q && o < 47; q++) home[o++] = *q;
                for (const char *q = nm; *q && o < 47; q++) home[o++] = *q;
                home[o] = 0;
                user_add(nm, uidv, uidv, home, "/bin/shell.ce", 0);
                user_save();
                sel = uidv;
            }
        }
    }
    con_screen_pop();
}

/* ---------- shell loop ---------- */
/* v2.33: shell aliases (bash-style, single-level expansion) */
#define ALIAS_MAX 8
static char al_name[ALIAS_MAX][16];
static char al_value[ALIAS_MAX][64];
static int  al_count;

static int alias_lookup(const char *n) {
    for (int i = 0; i < al_count; i++)
        if (!strcmp_(al_name[i], n)) return i;
    return -1;
}

static void cmd_alias(char *arg) {
    char *eq = 0;
    for (char *p = arg; *p; p++) if (*p == '=') { eq = p; break; }
    if (!eq) {                          /* list all (or one) */
        if (*arg) {
            int i = alias_lookup(arg);
            if (i < 0) { vga_write(trs("alias: not found: ", "alias: 未找到: ")); vga_write(arg); vga_write("\n"); }
            else { vga_write("alias "); vga_write(al_name[i]); vga_write("='");
                   vga_write(al_value[i]); vga_write("'\n"); }
            return;
        }
        for (int i = 0; i < al_count; i++) {
            vga_write("alias "); vga_write(al_name[i]); vga_write("='");
            vga_write(al_value[i]); vga_write("'\n");
        }
        if (!al_count) vga_write("(no aliases)\n");
        return;
    }
    *eq = 0;
    char *name = arg, *value = eq + 1;
    if (!*name || !*value) { vga_write(trs("usage: alias name=value\n", "用法：alias 名称=value\n")); return; }
    int l = 0; while (name[l]) l++;
    if (l >= 16) { vga_write(trs("alias: name too long\n", "alias: 名称太长\n")); return; }
    int i = alias_lookup(name);
    if (i < 0) {
        if (al_count >= ALIAS_MAX) { vga_write("alias: table full\n"); return; }
        i = al_count++;
        for (int j = 0; j < l; j++) al_name[i][j] = name[j];
        al_name[i][l] = 0;
    }
    al_value[i][0] = 0;   /* #120: an existing alias must be REPLACED, not appended to */
    if (strcat_b(al_value[i], 64, value) != 0) {
        int v = 0; while (value[v] && v < 63) { al_value[i][v] = value[v]; v++; }
        al_value[i][v] = 0;
    }
}

static void cmd_unalias(char *arg) {
    if (!*arg) { vga_write(trs("usage: unalias name\n", "用法：unalias 名称\n")); return; }
    int i = alias_lookup(arg);
    if (i < 0) { vga_write(trs("unalias: not found: ", "unalias: 未找到: ")); vga_write(arg); vga_write("\n"); return; }
    for (int j = i; j < al_count - 1; j++) {
        for (int k = 0; k < 16; k++) al_name[j][k] = al_name[j + 1][k];
        for (int k = 0; k < 64; k++) al_value[j][k] = al_value[j + 1][k];
    }
    al_count--;
}

/* v2.34: df -- volume usage, one row per mount point (P2) */
static void cmd_df(void) {
    fs_df();
}

/* P2: mount <partition|device> <empty-dir>; bare `mount` lists the table.
 * spec: partition number 1..4 on drive 0, or "hd<d>p<e>" (e.g. hd0p2). */
static void cmd_mount(char *arg) {
    if (!*arg) { fs_mount_list(); return; }
    char *argv[4];
    int argc = split_words(arg, argv, 3);
    if (argc < 2) { vga_write(trs("usage: mount <1..4|hd0pN> <empty-dir>\n", "用法：挂载 <1..4|hd0pN> <empty-dir>\n")); return; }
    int rc = fs_mount(argv[0], argv[1]);
    switch (rc) {
    case FS_OK:
        vga_write("mount: "); vga_write(argv[1]); vga_write(" mounted\n");
        break;
    case FS_ERR_NOENT:  vga_write(trs("mount: target directory not found\n", "挂载: target 目录 未找到\n")); break;
    case FS_ERR_NOTDIR: vga_write(trs("mount: target is not a directory\n", "挂载: target is 不是目录\n")); break;
    case FS_ERR_INVAL:  vga_write(trs("mount: bad spec (partition 1..4 or hd0pN)\n", "挂载: 错误的 规格 (partition 1..4 or hd0pN)\n")); break;
    case FS_ERR_EXIST:  vga_write("mount: directory is already a mount point\n"); break;
    case -10: vga_write("mount: unsupported partition (extended or non-FAT32)\n"); break;
    case -11: vga_write(trs("mount: no free mount slot\n", "mount: 没有空闲挂载位\n")); break;
    case -12: vga_write(trs("mount: failed to probe a FAT32 volume there\n", "挂载: 失败 to probe a FAT32 volume there\n")); break;
    case -13: vga_write(trs("mount: target directory is not empty\n", "挂载: target 目录 is 非空\n")); break;
    case -15: vga_write("mount: partition already mounted elsewhere\n"); break;
    default:  vga_write(trs("mount: failed\n", "挂载: 失败\n")); break;
    }
}

/* P2: umount <directory> -- detach the partition bound there. */
static void cmd_umount(char *arg) {
    if (!*arg) { vga_write(trs("usage: umount <directory>\n", "用法：umount <目录>\n")); return; }
    char *argv[4];
    int argc = split_words(arg, argv, 1);
    if (argc < 1) { vga_write(trs("usage: umount <directory>\n", "用法：umount <目录>\n")); return; }
    int rc = fs_umount(argv[0]);
    switch (rc) {
    case FS_OK:         vga_write("umount: detached\n"); break;
    case FS_ERR_NOENT:  vga_write(trs("umount: not a mount point\n", "umount: 不是挂载点\n")); break;
    case FS_ERR_INVAL:  vga_write(trs("umount: bad argument\n", "umount: 错误的 参数\n")); break;
    case -14:           vga_write("umount: busy (current directory is inside)\n"); break;
    default:            vga_write(trs("umount: failed\n", "umount: 失败\n")); break;
    }
}

static char input[256];


/* ---- v2.28: TAB completion --------------------------------------------
 * helpers: bounded strncmp and bounded string append */
static int sh_strneq_(const char *s, const char *p, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] != p[i]) return 0;
        if (!s[i]) return 1;              /* prefix already shorter */
    }
    return 1;
}
static void bar_put_str(char *b, int *n, const char *s) {
    while (*s && *n < 158) b[(*n)++] = *s++;
}
/* ---- v2.28: TAB completion ----
 * token 0 completes against the command table; later tokens complete
 * against the current directory's file names (prefix only, no path
 * parsing).  Multiple matches go into the candidate bar (the row under
 * the input line), exactly one replacement goes back into the line. */
static const char *sh_cmds[] = {
    "about","alias","autostart","bg","cal","caps","cat","cd","chcaps","chmod","chown","clear","cls",
    "color","cowsay","cp","curl","date","df","dir","dns","echo","edit","env",
    "export","faultinfo","crashlog","fg","fortune","format","fsck","gfx","hang","help","hwinfo",
    "bsod","blue","panicmode",
    "groupadd","groups","daemon","history","id","jobs","kill","lang","log","login","ls","lsuser","mem","mkdir","mount","mv","umount",
    "netcfg","netdiag","pkg","passwd","ping","ps","reboot","rm","rmdir","run","sshd",
    "setenv","rain","sl","sndinfo","sndtest","stop","style","su","sudo","text","top",
    "head","tail","wc","tree","touch","udp","uname","unalias","unset","uptime","usb","useradd","userdel","useredit",
    "users","vdiag","whoami","yes","shutdown","theme","hive","reg","colors",  /* v2.72 */
};
#define SH_NCMDS ((int)(sizeof(sh_cmds) / sizeof(sh_cmds[0])))

static void sh_best_prefix(const char *const *tab, int n,
                           const char *pfx, int plen,
                           char *out, char *bar) {
    out[0] = 0; bar[0] = 0;
    const char *hit = 0; int hits = 0;
    for (int i = 0; i < n; i++)
        if (plen == 0 || (sh_strneq_(tab[i], pfx, plen))) { hit = tab[i]; hits++; }
    if (hits == 1) {
        int j = 0;
        while (hit[j] && j < 120) out[j] = hit[j], j++;
        out[j++] = ' '; out[j] = 0;
        return;
    }
    if (hits > 1) {                       /* candidate bar, first N names */
        int n2 = 0;
        bar_put_str(bar, &n2, "[tab] ");
        for (int i = 0; i < n && n2 < 140; i++)
            if (plen == 0 || sh_strneq_(tab[i], pfx, plen)) {
                bar_put_str(bar, &n2, tab[i]);
                bar_put_str(bar, &n2, " ");
            }
        bar[n2] = 0;
    }
}

static int sh_complete(const char *line, int cur, char out[KB_BUF], char bar[160]) {
    /* command token = the region from the first non-blank char up to the
     * first blank; if `cur` is inside it (or nothing typed yet) complete
     * against the command table, otherwise against file names. */
    int s0 = 0;
    while (s0 < cur && line[s0] == ' ') s0++;
    int blank = (s0 >= cur);
    int cmd_end = s0;
    while (cmd_end < cur && line[cmd_end] != ' ') cmd_end++;
    if (blank || cmd_end >= cur) {        /* inside the command token */
        int plen = cur - s0;
        if (plen == 0) { out[0] = 0; bar[0] = 0; return 0; }  /* no hint on bare TAB */
        sh_best_prefix(sh_cmds, SH_NCMDS, line + s0, plen, out, bar);
        return (out[0] != 0);
    }
    int ts = s0;
    int i = cmd_end;
    while (i < cur) {
        while (i < cur && line[i] == ' ') i++;
        if (i >= cur) break;
        ts = i;
        while (i < cur && line[i] != ' ') i++;
        if (i >= cur) break;
    }
    int te = (i > cur) ? cur : i;
    if (ts >= te) { out[0] = 0; bar[0] = 0; return 0; }
    /* path completion only for a plain name (no '/' yet) */
    for (int j = ts; j < te; j++)
        if (line[j] == '"') { out[0] = 0; bar[0] = 0; return 0; }
    static char names[64][FS_NAME_MAX];
    char cwdp[FS_PATH_MAX];
    fs_pwd(cwdp, sizeof(cwdp));
    int n = fs_enum_entries(cwdp, names, 64);
    if (n <= 0) { out[0] = 0; bar[0] = 0; return 0; }
    /* shell order: files enum returns dir names first?  just prefix-match */
    int plen = te - ts;
    int kept = 0;
    static const char *ptrs[64];
    static char flat[64][FS_NAME_MAX];
    for (int j = 0; j < n; j++)
        if (plen == 0 || sh_strneq_(names[j], line + ts, plen)) {
            for (int k2 = 0; k2 < FS_NAME_MAX; k2++) flat[kept][k2] = names[j][k2];
            ptrs[kept] = flat[kept];
            kept++;
        }
    if (kept == 0) { out[0] = 0; bar[0] = 0; return 0; }
    char fo[KB_BUF], fbar[160];
    sh_best_prefix(ptrs, kept, line + ts, plen, fo, fbar);
    if (fo[0]) {                          /* splice the completed token */
        int j = 0, o = 0;
        for (; j < ts && o < KB_BUF - 1; j++) out[o++] = line[j];
        for (j = 0; fo[j] && o < KB_BUF - 1; j++) out[o++] = fo[j];
        for (j = te; line[j] && o < KB_BUF - 1; j++) out[o++] = line[j];
        out[o] = 0;
        if (fbar[0]) { int b = 0; while (fbar[b] && b < 159) { bar[b] = fbar[b]; b++; } bar[b] = 0; }
        return 1;
    }
    if (fbar[0]) { int b = 0; while (fbar[b] && b < 159) { bar[b] = fbar[b]; b++; } bar[b] = 0; }
    return 0;
}

static void prompt(void) {
    /* v2.25: keep the input line on the middle row of the screen. */
    con_scroll_anchor();
    char pwd[160];
    fs_pwd(pwd, sizeof(pwd));
    user_rec_t *u = user_by_uid(g_uid);

    /* v2.35: PS1 custom prompt (\u = user, \h = host, \w = cwd,
     * \$ = $; anything else passes through).  Unset PS1 -> default. */
    const char *ps1 = env_get_("PS1");
    if (ps1 && *ps1) {
        vga_set_fg(pal_vga(PAL_PROMPT_USER));
        for (const char *p = ps1; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'u' && u) vga_write(u->name);
                else if (*p == 'h') vga_write("catos");
                else if (*p == 'w') { vga_set_fg(pal_vga(PAL_PROMPT_PATH)); vga_write(pwd); vga_set_fg(pal_vga(PAL_PROMPT_USER)); }
                else if (*p == '$') vga_write(g_uid == UID_ROOT ? "#" : "$");
                else { vga_write("\\"); vga_putc(*p); }
            } else vga_putc(*p);
        }
        vga_write(" ");
        con_set_fg_rgb(pal_rgb(PAL_FG));
        return;
    }

    vga_set_fg(pal_vga(PAL_PROMPT_USER));
    if (u) { vga_write(u->name); vga_write("@"); }
    vga_write("catos");
    vga_set_fg(pal_vga(PAL_PROMPT_PATH));
    vga_write(":");
    vga_write(pwd);
    vga_set_fg(pal_vga(PAL_PROMPT_SYMBOL));
    vga_write(g_uid == UID_ROOT ? "# " : "$ ");
    con_set_fg_rgb(pal_rgb(PAL_FG));
}

/* show the NIC state on the boot screen (kernel's net_init line is
 * cleared by vga_clear, so this re-prints it for the user) */
static void net_boot_status(void) {
    extern int net_ready;
    extern int netdbg_pci_count;
    extern uint16_t netdbg_pci_vend[8];
    extern uint16_t netdbg_pci_dev[8];
    extern int net_fail_code;
    extern uint8_t net_mac[6];
    extern int net_env_vmware(void);
    if (net_ready) {
        vga_write("net: AMD PCnet-PCI up  mac=");
        print_hex((net_mac[0]<<8)|net_mac[1]); vga_write(":");
        print_hex((net_mac[2]<<8)|net_mac[3]); vga_write(":");
        print_hex((net_mac[4]<<8)|net_mac[5]); vga_write("\n");
        return;
    }
    vga_write("net: no usable NIC");
    if (net_fail_code == 1) vga_write(" (no 1022:2000)");
    else if (net_fail_code == 2) vga_write(" (open bus)");
    else if (net_fail_code == 3) vga_write(" (MAC unreadable)");
    else if (net_fail_code == 4) vga_write(trs(" (INIT/START failed)", " (INIT/START 失败)"));
    vga_write("; PCI bus 0: ");
    if (netdbg_pci_count == 0) {
        vga_write(trs("no devices\n", "无设备\n"));
    } else {
        for (int i = 0; i < netdbg_pci_count; i++) {
            vga_write("vend="); print_hex(netdbg_pci_vend[i]);
            vga_write(" dev="); print_hex(netdbg_pci_dev[i]);
            if (i < netdbg_pci_count - 1) vga_write(" ");
        }
        vga_write("\n");
    }
    if (net_env_vmware())
        vga_write("net: VMware guest -> set the VM NIC to Intel E1000\n"
                  "     (PCnet path works only when the adapter is 'AMD PCnet')\n");
    else
        vga_write("net: QEMU -> -netdev user,id=n0 -device pcnet,netdev=n0\n");
}

void shell_run(void) {
    mbi = (multiboot_info_t *)multiboot_info_ptr;
    btime_mark("S0 shell_run enter");

    vga_clear();
    fs_init();
    btime_mark("S1 fs_init done");
    hive_set_user(g_uid);     /* v2.67: HKCU -> HKU\<uid>                      */
    hive_init();              /* v2.67: binary hive; migrates /etc/reg + files */
    pal_init();               /* v2.67: colour roles from the hive             */
    lang_init();              /* v2.21 T50: load lang via reg, default en */
    btime_mark("S2 lang_init");
    env_init();               /* v2.23: load /etc/env, default PATH=/bin */
    btime_mark("S3 env_init");
    /* v2.72 boot order fix: the theme must be resolved BEFORE the frame is
     * painted.  Applying it afterwards left the backdrop in the OLD
     * background colour while the cells used the new one -- on screen that
     * is a lighter/darker rectangle around the text (the "font boxes" and
     * the leftover region the user reported on VMware). */
    /* v2.72: the hive is the single source of truth for colours, and the
     * Theme value is only a label now.  Re-applying the named scheme here
     * used to overwrite every role the user had just written with
     * `reg set` / `colors set`, so those edits were silently useless on the
     * next boot.  `theme <name>` is still how the whole table gets rewritten
     * on purpose. */
    con_set_style_rgb(pal_rgb(PAL_FG), pal_rgb(PAL_BG), 0);
    boot_apply_videomode();   /* v2.56: boot display mode via reg, default gfx24 */
    bsod_init();              /* v2.59: cache panic action config */
    ce_init();
    btime_mark("S4 ce_init");
    /* v2.13: load the persistent user database + ACL (seeds root/user and
     * the default owner/group/other table when the files are absent) */
    user_db_init();
    acl_init();
    group_db_init();        /* v2.53 supplementary groups (/etc/group) */
    session_apply();                /* boot session = root (initial user) */
    btime_mark("S5 user/acl/session");
    if (!fs_fs_ready())
        vga_write(tr(L_NO_FAT32));
    else {
        ce_install_builtins();      /* fallback: install built-in apps at boot */
        btime_mark("S6 builtins installed");
        ce_install_pkg_repo();      /* NIGHT-P4: seed /pkg/ local repo + index */
        btime_mark("S7 pkg repo seeded");
        /* v2.17: boot output now comes from /etc/autostart scripts
         * (echo lines) instead of a hardcoded banner.  Falls back to a
         * seeded default script on a fresh volume. */
        boot_autostart();
        btime_mark("S8 autostart done");
        /* Re-show the boot fallback warning below the banner so it does not
         * scroll off the console before the user reads it. */
        if (g_fallback_note[0]) {
            vga_write("\n");
            vga_write(g_fallback_note);
            vga_write("\n");
        }
        /* v2.72: the boot banner script starts with `clear`, so the
         * "[display] boot mode: ..." line printed before autostart is gone
         * by the time the user sees a prompt.  Re-state the geometry once
         * here -- this is the line to quote when a host window and the
         * framebuffer disagree. */
        {
            char g[80];
            int w = con_info(CON_INFO_PIXEL_W), h = con_info(CON_INFO_PIXEL_H);
            int c = con_info(CON_INFO_COLS), r = con_info(CON_INFO_ROWS);
            int cw = con_info(CON_INFO_CELL_W), chh = con_info(CON_INFO_CELL_H);
            g[0] = 0;
            strcat_b(g, (int)sizeof(g), "[display] frame ");
            geom_num(g, (int)sizeof(g), w);   strcat_b(g, (int)sizeof(g), "x");
            geom_num(g, (int)sizeof(g), h);   strcat_b(g, (int)sizeof(g), ", ");
            geom_num(g, (int)sizeof(g), c);   strcat_b(g, (int)sizeof(g), "x");
            geom_num(g, (int)sizeof(g), r);   strcat_b(g, (int)sizeof(g), " cells @");
            geom_num(g, (int)sizeof(g), cw);  strcat_b(g, (int)sizeof(g), "x");
            geom_num(g, (int)sizeof(g), chh); strcat_b(g, (int)sizeof(g), ")\n");
            vga_write(g);
        }
    }

    kb_set_completer(sh_complete);        /* v2.28: TAB completion */
    btime_mark("S9 shell prompt ready");

    while (1) {
        prompt();
        int ret = kb_readline(input, sizeof(input));
        if (ret == 2 || ret == 3) {               /* ^C cancels / ^Z suspends */
            con_set_fg_rgb(pal_rgb(PAL_SUCCESS));
            vga_write(ret == 2 ? "^C\n" : "^Z\n");
            con_set_fg_rgb(pal_rgb(PAL_FG));
            int tid = g_fg_tid;
            g_fg_tid = -1;
            if (tid > 0) {
                task_t *t = task_by_tid(tid);
                if (t)
                    task_signal(tid, ret == 2 ? SIGINT : SIGTSTP);
                if (ret == 3 && t) {             /* ^Z -> bg */
                    t->bg_mode = 1;
                    g_last_bg_tid = tid;
                    vga_write("[bg tid "); print_int(tid); vga_write("] suspended\n");
                }
            }
            continue;
        }
        env_expand(input, (int)sizeof(input));   /* v2.23: $KEY / ${KEY} */

        /* Split first token */
        char *cmd = input;
        while (*cmd == ' ') cmd++;
        char *arg = cmd;
        while (*arg && *arg != ' ') arg++;
        if (*arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }

        if (!*cmd) continue;

        /* v2.47: pending-guess one-shot confirm.  A bare y/Y (no trailing
         * args) replays the suggested command line; anything else consumes
         * the offer.  This check lives ONLY in the main prompt loop --
         * in-command confirmations read their own input and never reach
         * here, so their y/n answers follow the original path. */
        if (pend_valid && !*arg &&
            (!strcmp_(cmd, "y") || !strcmp_(cmd, "Y"))) {
            char replay[256];
            replay[0] = 0;
            strcat_b(replay, sizeof(replay), pend_cmd);
            if (*pend_arg) {
                strcat_b(replay, sizeof(replay), " ");
                strcat_b(replay, sizeof(replay), pend_arg);
            }
            pend_clear();               /* one-shot: consumed on replay */
            /* echo the actual line being run so the replay is obvious */
            con_set_fg_rgb(pal_rgb(PAL_WARNING));
            vga_write("-> ");
            con_set_fg_rgb(pal_rgb(PAL_PROMPT_PATH));
            vga_write(replay);
            con_set_fg_rgb(pal_rgb(PAL_WARNING));
            vga_write("\n");
            con_set_fg_rgb(pal_rgb(PAL_FG));
            char *rc = replay;
            while (*rc == ' ') rc++;
            char *ra = rc;
            while (*ra && *ra != ' ') ra++;
            if (*ra) { *ra = 0; ra++; while (*ra == ' ') ra++; }
            sec_fg_arm(rc);
            shell_exec(rc, ra);
            sec_fg_disarm();
            continue;
        }
        pend_clear();                   /* any other line covers the offer */

        /* v2.33: single-level alias expansion of the command word */
        int ai = alias_lookup(cmd);
        if (ai >= 0) {
            char expanded[256];
            expanded[0] = 0;
            strcat_b(expanded, (int)sizeof(expanded), al_value[ai]);
            if (*arg && strcat_b(expanded, (int)sizeof(expanded), " ") == 0)
                strcat_b(expanded, (int)sizeof(expanded), arg);
            char *earg = expanded;
            while (*earg && *earg != ' ') earg++;
            if (*earg) { *earg = 0; earg++; while (*earg == ' ') earg++; }
            sec_fg_arm(expanded);
            shell_exec(expanded, earg);
            sec_fg_disarm();
            continue;
        }
        sec_fg_arm(cmd);
        shell_exec(cmd, arg);
        sec_fg_disarm();
    }
}

/* Execute one parsed command line: builtins first, then current-dir
 * program and /bin PATH lookup.  Shared by the interactive loop and the
 * /etc/autostart script runner (identical semantics for both). */
/* ---------- v2.67 security audit --------------------------------------
 * `audit` is the honest answer to "what could take this box away from me".
 * It prints the live policy, the conclusions drawn from the code as it
 * stands today, and the decision log kept by sec.c. */
static void cmd_audit(char *arg) {
    int limit = 0, clear = 0;
    if (arg) {
        char *p = arg;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            if (p[0] == '-' && p[1] == '-' && !strcmp_(p, "--clear")) { clear = 1; }
            else if (p[0] == '-' && p[1] == 'n') {
                p += 2;
                while (*p == ' ') p++;
                int v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                limit = v;
                continue;
            }
            while (*p && *p != ' ') p++;
        }
    }
    if (clear) { sec_audit_clear(); vga_write("audit: ring cleared\n"); return; }

    char brief[128];
    pager_begin(1);
    pager_line("CatOS security audit -- v2.67 (policy: trust nothing)");
    pager_line("");
    pager_line("[live policy]");
    sec_policy_brief(brief, sizeof(brief));
    pager_line(brief);
    pager_line("Exec.Strict = world/group-writable binaries refused");
    pager_line("");
    pager_line("[1] foreground task taking the console");
    pager_line("  Any builtin runs ring0 with interrupts on and never yields;");
    pager_line("  the keyboard IRQ still buffers keys, but the shell cannot read");
    pager_line("  them until the command returns.  Ctrl+C is honoured only by");
    pager_line("  loops that poll it (yes, pager, sleep).  `hang` therefore freezes");
    pager_line("  the console until Ctrl-Alt-Del -- by design, but it is exactly");
    pager_line("  the shape a malicious CE program can take.  There is no");
    pager_line("  watchdog and no per-task CPU quota yet: a busy ring3 task can");
    pager_line("  still starve the scheduler.  Mitigation today: Ctrl-Alt-Del,");
    pager_line("  and do not run untrusted .ce binaries.");
    pager_line("");
    pager_line("[2] virus surface");
    pager_line("  - .ce executables are loaded from the FAT32 volume; anything that");
    pager_line("    can write a file can plant one.  sec_exec_check refuses");
    pager_line("    world/group-writable binaries (HKSEC\\Policy\\Exec\\Strict=1).");
    pager_line("  - /etc/autoexec scripts run as root at boot: that IS persistence.");
    pager_line("    Keep the directory root-owned 0755; chmod it and check ACLs.");
    pager_line("  - `pkg install` unpacks unsigned packages; only install from a");
    pager_line("    directory you control.");
    pager_line("  - there is no NX/ASLR and no user/kernel isolation in ring0 code.");
    pager_line("");
    pager_line("[3] hijack surface");
    pager_line("  - bare NAME lookup is cwd first, then $PATH.  A planted foo.ce in a");
    pager_line("    writable cwd runs instead of the intended tool.  Set");
    pager_line("    HKSEC\\Policy\\Exec\\Strict=1 and keep cwd clean; builtins always");
    pager_line("    win over files, so core commands cannot be shadowed.");
    pager_line("  - PATH itself is registry-backed (HKLM\\Environment\\PATH).  A user");
    pager_line("    who can edit HKLM can point it at a writable directory; hive");
    pager_line("    writes need root or CAP_USERADM.");
    pager_line("  - redirects and pipes go through fs_perm_check with the session");
    pager_line("    identity, so a non-root session cannot truncate /etc files.");
    pager_line("");
    pager_line("[4] capability model");
    pager_line("  root passes everything; everyone else needs every bit of the");
    pager_line("  command's requirement.  Override per command in");
    pager_line("  HKSEC\\Policy\\Cmd\\<name>\\Caps (e.g. \"NET|KILL\", \"none\").  A");
    pager_line("  malformed policy string fails CLOSED.");
    pager_line("");
    pager_line("[decision log]  (newest first; `audit -n 10`, `audit --clear`)");
    sec_audit_dump(limit ? limit : 24);
    pager_end();
}

/* v2.68: retired aliases deliberately stay as visible diagnostics. */
static void cmd_retired(const char *old, const char *now) {
    con_set_fg_rgb(pal_rgb(PAL_ERROR));
    vga_write("catos: 我们不支持此命令了，请使用: ");
    vga_write(now);
    vga_write("\n");
    con_set_fg_rgb(pal_rgb(PAL_FG));
    (void)old;
}

static void shell_exec(char *cmd, char *arg) {
    /* v2.62: hide the prompt caret while a command owns the console.  The
     * reverse-video cursor parked on the input line used to overwrite the
     * first characters a foreground command printed ("swallowed text"). */
    con_suppress_cursor(1);
    g_last_rc = 0;                 /* v2.65: each command starts with status 0 */
    /* v2.59: parse the redirect/pipe/background marker ONCE, up front, before
     * the line is truncated.  The same out_redir/append/pipe_to feed both the
     * non-interactive pager latch and the builtin stdout tee below. */
    char *out_redir = 0, *pipe_to = 0; int append = 0; int bg = 0;
    int ni = shell_line_noninteractive(arg, &out_redir, &append, &pipe_to, &bg);
    /* The pager latch follows THIS line's state exactly: a redirect, pipe,
     * background job, or script/non-tty run streams straight through without
     * ever parking on a More prompt.  Re-derive the boolean from the parsed
     * pointers + bg + script state (not the helper's cached return) so a
     * stale latch cannot leak across commands. */
    ni = (out_redir || pipe_to || bg || g_script_mode) ? 1 : 0;
    pager_set_noninteractive(ni);
    /* v2.59: arm the ring0 builtin stdout tee for a plain file redirect.
     * Pipes and `&` keep their existing task ring/pipe wiring below. */
    redir_arm(out_redir, append, pipe_to);

    /* ---- v2.67 efficiency + safety gate ------------------------------
     * Two checks run BEFORE any builtin body:
     *   1. cli_validate() -- the syntax contract (posix.c).  An invalid
     *      option or a missing operand is reported as a usage error with
     *      status 2, and "cat -z" can no longer be mistaken for a file
     *      called "-z".
     *   2. sec_allow() -- the capability gate (sec.c).  A command can
     *      never forget its own permission check, and the requirement is
     *      overridable per command in the registry.
     * Root bypasses the capability check, never the syntax check. */
    {
        int v = cli_validate(cmd, (const char *)arg);
        if (v == -200) {                       /* --help printed; skip body */
            redir_flush();
            con_unsuppress_cursor();
            return;
        }
        if (v != 0) {
            g_last_rc = v;
            sec_audit(g_uid, "usage", cmd, v);
            redir_flush();
            con_unsuppress_cursor();
            return;
        }
        int bad_policy = 0;
        int al = sec_allow(cmd, g_uid, g_caps);
        if (al != SEC_ALLOW) {
            char need[64]; need[0] = 0;
            sec_caps_str(sec_required(cmd, &bad_policy), need, sizeof(need));
            vga_write(cmd); vga_write(trs(": permission denied", ": 权限不足"));
            if (al == SEC_HOSTILE) {
                vga_write(" (policy string for '"); vga_write(cmd);
                vga_write("' is malformed -- failing closed)");
            } else {
                vga_write(" (need CAP_"); vga_write(need); vga_write(")");
            }
            vga_write("\n");
            g_last_rc = 13;                    /* EACCES */
            sec_audit(g_uid, "deny", cmd, 13);
            redir_flush();
            con_unsuppress_cursor();
            return;
        }
        sec_audit(g_uid, "exec", cmd, 0);
    }
    /* v2.73: from here to the single exit below, everything printed is
     * command output -- indent every new output line by two spaces. */
    con_set_out_indent(1);
    pager_reset();                       /* v2.76: fresh flood-abort state per command */
        if (!strcmp_(cmd, "help"))            cmd_help();
        else if (!strcmp_(cmd, "keys") ||
                 !strcmp_(cmd, "shortcuts")) cmd_keys();
        else if (!strcmp_(cmd, "lang"))      cmd_lang(arg);
        else if (!strcmp_(cmd, "export") ||
                 !strcmp_(cmd, "setenv"))    cmd_export(arg);
        else if (!strcmp_(cmd, "env"))       env_list();
        else if (!strcmp_(cmd, "PATH")) {
            const char *pv = env_get_("PATH");
            vga_write(pv && *pv ? pv : "/bin"); vga_write("\n");
        }
        else if (!strcmp_(cmd, "unset"))     cmd_unset(arg);
        else if (!strcmp_(cmd, "alias"))     cmd_alias(arg);
        else if (!strcmp_(cmd, "df"))        cmd_df();
        else if (!strcmp_(cmd, "mount"))     cmd_mount(arg);
        else if (!strcmp_(cmd, "umount"))     cmd_umount(arg);
        else if (!strcmp_(cmd, "unalias"))   cmd_unalias(arg);
        else if (!strcmp_(cmd, "clear") ||
                 !strcmp_(cmd, "cls"))      vga_clear();   /* NIGHT-P3: cls alias */
        else if (!strcmp_(cmd, "gfx"))        cmd_gfx(arg);
        else if (!strcmp_(cmd, "text"))       cmd_text();
        else if (!strcmp_(cmd, "style"))      cmd_style(arg);
        else if (!strcmp_(cmd, "rgb"))        cmd_rgb(arg);
        else if (!strcmp_(cmd, "palette"))    cmd_palette(arg);
        else if (!strcmp_(cmd, "display"))    cmd_display();
        else if (!strcmp_(cmd, "size"))       cmd_size(arg);
        else if (!strcmp_(cmd, "theme"))      cmd_theme(arg);  /* v2.61 */
        else if (!strcmp_(cmd, "colors"))     cmd_colors(arg); /* v2.72 */
        else if (!strcmp_(cmd, "hive") ||
                 !strcmp_(cmd, "reg"))        cmd_reg(arg);    /* v2.67 */
        else if (!strcmp_(cmd, "audit"))      cmd_audit(arg);  /* v2.67 */
        else if (!strcmp_(cmd, "dash") ||
                 !strcmp_(cmd, "sysmon")) {   /* v2.67 TUI */
            int pg = 0;
            if (arg && *arg) {
                if      (!strcmp_(arg, "system"))   pg = 0;
                else if (!strcmp_(arg, "storage"))  pg = 1;
                else if (!strcmp_(arg, "network"))  pg = 2;
                else if (!strcmp_(arg, "security")) pg = 3;
                else if (arg[0] >= '1' && arg[0] <= '4') pg = arg[0] - '1';
                else {
                    vga_write(trs("dash: unknown page '", "dash: 未知 page '")); vga_write(arg);
                    vga_write("' (system|storage|network|security)\n");
                    g_last_rc = 2;
                    pg = -1;
                }
            }
            if (pg >= 0) dash_run(g_uid, g_caps, pg);
        }
        else if (!strcmp_(cmd, "useredit"))   cmd_useredit();
        else if (!strcmp_(cmd, "usb")) {
            if (!strcmp_(arg, "scan")) {
                int n = usb_scan_devices();
                vga_write(n ? "usb: keyboard attached\n"
                            : "usb: no device found\n");
            } else {
                char b[96];
                vga_write("usb: "); vga_write(usb_status(b, sizeof(b)));
                vga_write("\n");
            }
        }
        else if (!strcmp_(cmd, "vdiag"))      con_diag();
        else if (!strcmp_(cmd, "echo"))      cmd_echo(arg);
        else if (!strcmp_(cmd, "clip"))      cmd_clip(arg);
        else if (!strcmp_(cmd, "mouse"))     cmd_mouse();
        /* NIGHT-P3: Linux-style easter eggs */
        else if (!strcmp_(cmd, "cowsay"))    cmd_cowsay(arg);
        else if (!strcmp_(cmd, "sl"))         cmd_sl();
        else if (!strcmp_(cmd, "rain"))       cmd_rain();
        else if (!strcmp_(cmd, "train"))      cmd_train();
        else if (!strcmp_(cmd, "fortune"))    cmd_fortune();
        else if (!strcmp_(cmd, "cal"))        cmd_cal(arg);
        else if (!strcmp_(cmd, "yes"))        cmd_yes(arg);
        else if (!strcmp_(cmd, "about"))      cmd_about();
        else if (!strcmp_(cmd, "autostart"))  boot_autostart();
        else if (!strcmp_(cmd, "uname"))      cmd_uname();
        else if (!strcmp_(cmd, "date"))       cmd_date();
        else if (!strcmp_(cmd, "mem"))        cmd_mem();
        else if (!strcmp_(cmd, "reboot")) {
            /* v2.25 fine-grained split: CAP_SYS (broad) or CAP_REBOOT. */
            if (g_uid == UID_ROOT || (g_caps & (CAP_SYS | CAP_REBOOT)))
                cmd_reboot();
            else
                vga_write(trs("reboot: permission denied (need CAP_SYS or CAP_REBOOT)\n", "reboot: 权限不足 (need CAP_SYS or CAP_REBOOT)\n"));
        }
        else if (!strcmp_(cmd, "hang"))      cmd_hang();
        else if (!strcmp_(cmd, "shutdown")) {
            if (g_uid == UID_ROOT || (g_caps & CAP_SYS))
                cmd_shutdown();
            else
                vga_write(trs("shutdown: permission denied (need root or CAP_SYS)\n", "shutdown: 权限不足 (need root or CAP_SYS)\n"));
        }
        else if (!strcmp_(cmd, "bsod") ||
                 !strcmp_(cmd, "blue"))      cmd_bsod(arg);
        else if (!strcmp_(cmd, "panicmode")) cmd_panicmode(arg);
        else if (!strcmp_(cmd, "dir"))           cmd_ls(arg);   /* v2.68: dir == ls */
        else if (!strcmp_(cmd, "ls"))            cmd_ls(arg);  /* v2.64: eza/lsd-style modes */
        else if (!strcmp_(cmd, "cd"))         cmd_cd(arg);
        else if (!strcmp_(cmd, "mkdir")) {
            if (arg[0] == '-' && arg[1] == 'p' && (arg[2] == ' ' || arg[2] == 0)) {
                const char *pa = arg + 2;
                while (*pa == ' ') pa++;
                cmd_mkdir_p(pa, 1);
            } else               cmd_mkdir_p(arg, 0);
        }
        else if (!strcmp_(cmd, "touch"))      cmd_retired("touch", "edit");
        else if (!strcmp_(cmd, "cat"))        cmd_retired("cat", "edit -c");
        else if (!strcmp_(cmd, "rm"))         cmd_rm(arg);
        else if (!strcmp_(cmd, "rmdir"))      cmd_retired("rmdir", "rm -d");
        else if (!strcmp_(cmd, "cp"))         cmd_cp(arg);
        else if (!strcmp_(cmd, "mv"))         cmd_mv(arg);
        /* NIGHT-P4: package manager + migrated common tools */
        else if (!strcmp_(cmd, "pkg"))         cmd_pkg(arg);
        else if (!strcmp_(cmd, "head"))       cmd_retired("head", "edit -c N");
        else if (!strcmp_(cmd, "tail"))       cmd_retired("tail", "edit -c -N");
        else if (!strcmp_(cmd, "wc"))         cmd_wc(arg);
        else if (!strcmp_(cmd, "tree"))       cmd_tree(arg);
        else if (!strcmp_(cmd, "format")) {
            /* v2.25: fine-grained split -- CAP_FSADMIN (broad) or the
             * new CAP_FORMAT successor both grant mkfs. */
            if (session_is_fsadmin() || (g_caps & CAP_FORMAT)) cmd_format();
            else vga_write(trs("format: permission denied (need CAP_FSADMIN or CAP_FORMAT)\n", "format: 权限不足 (need CAP_FSADMIN or CAP_FORMAT)\n"));
        }
        else if (!strcmp_(cmd, "edit"))      cmd_edit(arg);
        else if (!strcmp_(cmd, "run"))        cmd_run(arg, g_uid);
        else if (!strcmp_(cmd, "sudo")) {
            /* sudo CMD ...: run as root; requires CAP_USERADM (or root) */
            if (!session_is_admin()) {
                vga_write(trs("sudo: permission denied (need CAP_USERADM)\n", "sudo: 权限不足 (need CAP_USERADM)\n"));
            } else {
            while (*arg == ' ') arg++;
            char *scmd = arg;
            char *sarg = arg;
            while (*sarg && *sarg != ' ') sarg++;
            if (*sarg) { *sarg = 0; sarg++; while (*sarg == ' ') sarg++; }
            if (!*scmd) { vga_write(trs("usage: sudo COMMAND [args...]\n", "用法：sudo COMMAND [args...]\n")); }
            else if (!strcmp_(scmd, "run"))   cmd_run(sarg, UID_ROOT);
            else if (!strcmp_(scmd, "sudo")) {
                vga_write(trs("sudo: no nested sudo\n", "sudo: 不支持嵌套 sudo\n"));
            } else {
                /* v2.68: re-enter the builtin dispatcher as root, so
                 * `sudo ls` / `sudo top` work like Linux. */
                char sub[512]; int k = 0;
                while (sarg[k] && k < (int)sizeof(sub) - 1) { sub[k] = sarg[k]; k++; }
                sub[k] = 0;
                int ouid = g_uid, ogid = g_gid;
                g_uid = UID_ROOT; g_gid = 0;
                shell_exec(scmd, sub);
                g_uid = ouid; g_gid = ogid;
            }
            }
        }
        else if (!strcmp_(cmd, "whoami"))      cmd_whoami();
        else if (!strcmp_(cmd, "id"))          cmd_id(arg);
        else if (!strcmp_(cmd, "lsuser") ||
                 !strcmp_(cmd, "users"))       cmd_lsuser();
        else if (!strcmp_(cmd, "caps"))        cmd_caps_legend();
        else if (!strcmp_(cmd, "login"))       cmd_login(arg, 0);
        else if (!strcmp_(cmd, "su"))          cmd_login(arg, 1);
        else if (!strcmp_(cmd, "useradd"))     cmd_useradd(arg);
        else if (!strcmp_(cmd, "groupadd"))    cmd_groupadd(arg);
        else if (!strcmp_(cmd, "groups"))      cmd_groups();
        else if (!strcmp_(cmd, "daemon"))      cmd_daemon(arg);
        else if (!strcmp_(cmd, "userdel"))     cmd_userdel(arg);
        else if (!strcmp_(cmd, "passwd"))      cmd_passwd(arg);
        else if (!strcmp_(cmd, "chcaps"))      cmd_chcaps(arg);
        else if (!strcmp_(cmd, "chmod"))       cmd_chmod(arg);
        else if (!strcmp_(cmd, "chown"))       cmd_chown(arg);
        else if (!strcmp_(cmd, "history")) {
            int n = kb_hist_count();
            for (int i = 0; i < n; i++) {
                vga_write("  ");
                print_int(i + 1);
                vga_write("  ");
                vga_write(kb_hist_get(i));
                vga_write("\n");
            }
        }
        else if (!strcmp_(cmd, "ps"))          task_ps_print();
        else if (!strcmp_(cmd, "top"))         cmd_top();
        else if (!strcmp_(cmd, "jobs"))        cmd_jobs();
        else if (!strcmp_(cmd, "log"))         cmd_log(arg);
        else if (!strcmp_(cmd, "fg"))          cmd_fg(arg);
        else if (!strcmp_(cmd, "bg"))         cmd_bg(arg);
        else if (!strcmp_(cmd, "stop"))        cmd_stop(arg);
        else if (!strcmp_(cmd, "hwinfo"))      cmd_hwinfo();
        else if (!strcmp_(cmd, "sndinfo"))     cmd_sndinfo(arg);
        else if (!strcmp_(cmd, "sndregs"))     cmd_sndregs();   /* v2.72 */
        else if (!strcmp_(cmd, "fprobe"))      cmd_fprobe();    /* v2.72 */
        else if (!strcmp_(cmd, "sndmap"))      cmd_sndmap();    /* v2.72 */
        else if (!strcmp_(cmd, "sndtest"))     cmd_sndtest(arg);
        else if (!strcmp_(cmd, "netcfg"))       cmd_netcfg(arg);
        else if (!strcmp_(cmd, "uptime")) {
            print_int((int)(task_uptime_ms() / 1000));
            vga_write("s (");
            print_int((int)task_count());
            vga_write(" tasks)\n");
        }
        else if (!strcmp_(cmd, "kill")) {
            int tid = 0, neg = 0;
            while (*arg == ' ') arg++;
            if (*arg == '-') { neg = 1; arg++; }
            while (*arg >= '0' && *arg <= '9') { tid = tid * 10 + (*arg - '0'); arg++; }
            if (neg) tid = -tid;
            if (tid == 0) { vga_write(trs("usage: kill TID\n", "用法：kill TID\n")); }
            else {
                /* #184: hand the session identity to task_kill so its owner /
                 * CAP_KILL check actually runs.  The old UID_ROOT argument
                 * bypassed that check entirely, letting a caps=0 non-root
                 * session kill root-owned tasks. */
                int kr = task_kill(tid, g_uid);
                if (kr == -2) vga_write(trs("kill: permission denied\n", "kill: 权限不足\n"));
                else if (kr != 0) vga_write(trs("kill: no such task\n", "kill: 找不到任务\n"));
            }
        }
        else if (!strcmp_(cmd, "sshd"))         cmd_sshd(arg);
        else if (!strcmp_(cmd, "curl"))        cmd_curl(arg);
        else if (!strcmp_(cmd, "ping"))        cmd_ping(arg);
        else if (!strcmp_(cmd, "dns"))         cmd_dns(arg);
        else if (!strcmp_(cmd, "udp"))         cmd_udp(arg);
        else if (!strcmp_(cmd, "netdiag"))     cmd_netdiag(arg);
        else if (!strcmp_(cmd, "faultinfo"))   faultinfo_print();
        else if (!strcmp_(cmd, "crashlog"))    cmd_crashlog(arg);
        else if (!strcmp_(cmd, "fsck")) {
            if (!(session_is_fsadmin() || (g_caps & CAP_FSCK))) {
                vga_write(trs("fsck: permission denied (need CAP_FSADMIN or CAP_FSCK)\n", "fsck: 权限不足 (need CAP_FSADMIN or CAP_FSCK)\n"));
            } else {
                cmd_fsck(arg);
            }
        }
        else if (!strcmp_(cmd, "color")) {
            if (!*arg) {
                vga_write(trs("usage: color 0..255  (text mode maps to 16 colors)\n", "用法：color 0..255（文本模式映射到 16 色）\n"));
            } else {
                int c = 0;
                while (*arg >= '0' && *arg <= '9') { c = c * 10 + (*arg - '0'); arg++; }
                if (c >= 0 && c <= 255) con_set_fg((uint8_t)c);
                else vga_write(trs("usage: color 0..255\n", "用法：color 0..255\n"));
            }
        }
        else {
            /* not a builtin: current-dir program, then /bin (PATH), else
             * report an unknown command.  Runs as uid USER (use 'sudo'
             * for root).  A trailing '&' backgrounds the task.  v2.5:
             * `cmd [> F | >> F | | CMD]` redirection is supported. */
            char *out_redir = 0, *pipe_to = 0; int append = 0;
            parse_redir(arg, &out_redir, &append, &pipe_to);
            char *argv[16];
            int argc = 1;
            argv[0] = cmd;
            argc += split_words(arg, argv + 1, 15);
            int bg = 0;
            if (argc > 1 && argv[argc - 1][0] == '&' && argv[argc - 1][1] == 0) {
                bg = 1;
                argc--;
            }
            if (out_redir || pipe_to) {
                /* resolve the left-hand command via PATH, then spawn with
                 * redirection wired (run_from_path cannot wire it) */
                char p[FS_PATH_MAX];
                char pwd[FS_PATH_MAX];
                fs_pwd(pwd, sizeof(pwd));
                uint32_t c = 0, s = 0;
                p[0] = 0;
                if (!(pwd[0] == '/' && pwd[1] == 0)) {
                    if (strcat_b(p, sizeof(p), pwd) == 0) strcat_b(p, sizeof(p), "/");
                } else strcat_b(p, sizeof(p), "/");
                if (p[0]) strcat_b(p, sizeof(p), cmd);
                if (p[0]) strcat_b(p, sizeof(p), cmd_has_ext(cmd) ? "" : ".ce");
                if (p[0] && fs_resolve_file(p, &c, &s) != 0) {
                    p[0] = 0;
                    if (strcat_b(p, sizeof(p), "/bin/") == 0 &&
                        strcat_b(p, sizeof(p), cmd) == 0 &&
                        strcat_b(p, sizeof(p), cmd_has_ext(cmd) ? "" : ".ce") == 0 &&
                        fs_resolve_file(p, &c, &s) != 0) p[0] = 0;
                }
                if (!p[0]) {
                    con_set_fg_rgb(pal_rgb(PAL_ERROR));
                    vga_write(trs("catos: unknown command: ", "catos: 未知 命令: ")); vga_write(cmd); vga_write("\n");
                    con_set_fg_rgb(pal_rgb(PAL_FG));
                    g_last_rc = 127;             /* POSIX: command not found */
                } else if (pipe_to && *pipe_to) {
                    char *file2 = pipe_to;
                    char *sp = pipe_to;
                    while (*sp && *sp != ' ') sp++;
                    if (*sp) { *sp = 0; sp++; }
                    char *argv2[16];
                    int argc2 = 1;
                    argv2[0] = file2;
                    argc2 += split_words(sp, argv2 + 1, 15);
                    int h = kpipe_create();
                    if (h < 0) { vga_write(trs("run: no pipe slots\n", "run: 没有空闲管道\n")); }
                    else {
                        vga_clear();
                        spawn_ce_redirect(p, argc, argv, 0, g_uid, 0, 0, h, -1);
                        spawn_ce_redirect(file2, argc2, argv2, 1, g_uid, 0, 0, -1, h);
                    }
                } else {
                    vga_clear();
                    spawn_ce_redirect(p, argc, argv, !bg, g_uid,
                                      out_redir, append, -1, -1);
                }
            } else if (run_from_path(cmd, argv, argc, bg, g_uid) != 0) {
                con_set_fg_rgb(pal_rgb(PAL_ERROR));
                vga_write(trs("catos: unknown command: ", "catos: 未知 命令: "));
                vga_write(cmd); vga_write("\n");
                con_set_fg_rgb(pal_rgb(PAL_FG));
                g_last_rc = 127;             /* POSIX: command not found */
                /* NIGHT-P3: friendly nearest-command suggestion (yellow hint) */
                const char *sug = suggest_cmd(cmd);
                if (sug) {
                    con_set_fg_rgb(pal_rgb(PAL_WARNING));
                    vga_write(trs("catos: did you mean: ", "catos: 你指的是: "));
                    con_set_fg_rgb(pal_rgb(PAL_PROMPT_PATH));
                    vga_write(sug);
                    con_set_fg_rgb(pal_rgb(PAL_WARNING));
                    vga_write("?\n");
                    con_set_fg_rgb(pal_rgb(PAL_FG));
                    /* v2.47: arm the one-shot y-confirm for this guess. */
                    pend_arm(sug, arg);
                }
            }
        }
    /* v2.59: flush the builtin stdout tee to the redirect target, if armed. */
    if (pager_aborted())
        vga_write(trs("  (output skipped -- rest not shown, the keystroke you typed is kept)\n", "  （输出已跳过：剩余内容未显示，键入的按键已保留）\n"));
    redir_flush();
    con_set_out_indent(0);               /* v2.73: prompt stays flush left */
    con_unsuppress_cursor();             /* v2.62: caret returns for next prompt */
}

/* ---------- v2.17 boot autostart (/etc/autoexec) ---------- */

/* 8.3 names only: the volume has no LFN support, so "autostart" (9-char
 * stem) is rejected by path_has_long_names() -- hence "autoexec". */
#define AUTOSTART_DIR "/etc/autoexec"

/* Seeded on a fresh volume so the boot banner still works out of the
 * box; the user may edit or delete it and add more scripts. */
static const char *const AUTOSTART_DEFAULT =
    "# CatOS boot scripts live in /etc/autoexec\n"
    "# (one shell command per line, '#' comments and blank lines skipped,\n"
    "#  8.3 file names -- the volume has no long-name support).\n"
    "clear\n"
    "echo \"       _   _          _____        \"\n"
    "echo \"      | \\ | |   /\\   |  __ \\       \"\n"
    "echo \"      |  \\/ |  /  \\  | |__) |      \"\n"
    "echo \"      | \\   | / /\\ \\ |  ___/       \"\n"
    "echo \"      |  \\_/ / ____ \\| |           \"\n"
    "echo \"       \\___/_/    \\_\\_|           \"\n"
    "echo \"                             CatOS v2.78                            \"\n"
    "echo \"\"\n";

/* v2.72 final: the sshd boot seed was removed on purpose (see
 * boot_autostart()); keeping the constant would invite someone to re-seed
 * it by accident. */

static void boot_autostart(void) {
    if (!fs_fs_ready()) return;
    /* seed dir + default banner (fs_write_raw skips an existing file) */
    fs_mkdir(AUTOSTART_DIR);
    fs_write_raw(AUTOSTART_DIR "/banner.sh", AUTOSTART_DEFAULT,
                 (uint32_t)slen_(AUTOSTART_DEFAULT));
    /* v2.72 final: sshd is NOT started at boot any more.  The seed below
     * is deliberately gone -- an always-on :22 listener on a hobby OS with
     * an unsigned package path is a liability, not a feature.  `sshd` (or
     * `sshd -d`) still starts it by hand when a session needs it. */

    char names[8][FS_NAME_MAX];
    int n = fs_enum_files(AUTOSTART_DIR, names, 8);
    if (n <= 0) return;
    for (int i = 0; i < n && i < 8; i++) {
        char path[FS_PATH_MAX];
        int k = 0;
        const char *s;
        for (s = AUTOSTART_DIR; *s && k < (int)sizeof(path) - 1; s++)
            path[k++] = *s;
        if (k < (int)sizeof(path) - 1) path[k++] = '/';
        for (s = names[i]; *s && k < (int)sizeof(path) - 1; s++)
            path[k++] = *s;
        path[k] = 0;

        static char buf[2048];
        int len = fs_read_text(path, buf, (int)sizeof(buf) - 1);
        if (len <= 0) continue;
        buf[len] = 0;

        vga_write(trs("autostart: ", "自动启动："));
        vga_write(names[i]);
        vga_write("\n");
        g_script_mode = 1;          /* P1: no interactive prompts in scripts */
        char *p = buf;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            char *e = line + slen_(line);
            if (e > line && e[-1] == '\r') e[-1] = 0;   /* CRLF safety */
            env_expand(line, 256);                      /* v2.23 */

            char *lcmd = line;
            while (*lcmd == ' ' || *lcmd == '\t') lcmd++;
            if (!*lcmd || *lcmd == '#') continue;
            /* v2.54: a leading '@' marks an autostart service: launched
             * detached as a daemon instead of run in the boot script. */
            int as_daemon = 0;
            if (*lcmd == '@') { as_daemon = 1; lcmd++;
                while (*lcmd == ' ' || *lcmd == '\t') lcmd++; }
            char *larg = lcmd;
            while (*larg && *larg != ' ' && *larg != '\t') larg++;
            if (*larg) {
                *larg = 0; larg++;
                while (*larg == ' ' || *larg == '\t') larg++;
            }
            if (as_daemon) {
                /* v2.72 final: the builtin-daemon branch is gone.  It existed
                 * only to boot-start sshd, and that is exactly what the user
                 * asked to remove (an unattended network listener).  An
                 * explicit `@sshd` line now falls through to the .ce lookup
                 * below, which cannot load a builtin and says so. */
                char *dargv[16];
                int dargc = 1;
                dargv[0] = lcmd;
                dargc += split_words(larg, dargv + 1, 15);
                if (run_from_path(lcmd, dargv, dargc, 1, UID_ROOT) == 0) {
                    task_t *dt = task_by_tid(g_last_bg_tid);
                    if (dt) dt->daemon = 1;
                }
            } else {
                shell_exec(lcmd, larg);
            }
        }
        g_script_mode = 0;
    }
    net_boot_status();              /* keep the one-line NIC status at boot */
    vga_write("\n");
}
