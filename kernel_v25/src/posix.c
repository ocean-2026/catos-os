/*
 * posix.c -- v2.67 command-line contract for every shell builtin.
 *
 * Design goals (matching what a Linux user expects):
 *   - one syntax table, not 97 hand-written usage strings;
 *   - option errors detected BEFORE the command runs, so a typo can never
 *     be silently treated as a file name ("cat -z" used to print
 *     "cat: no such file", which is a lie);
 *   - exit statuses are POSIX: 2 for syntax, 1 for runtime failures,
 *     13 for permission denials;
 *   - `--help` works for every listed command without extra code.
 */
#include <stdint.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

#include "posix.h"
#include "vga.h"
#include "fs.h"

/* ---- tiny kernel-safe helpers ----------------------------------------- */
static int p_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int p_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ---- syntax table ------------------------------------------------------ */
#define EXIST1  0x01   /* first positional must exist (file or dir)   */
#define EXIST2  0x02   /* second positional must exist (cp/mv source) */
#define HELP    0x04   /* --help is meaningful                        */
#define DIR1    0x08   /* first positional must be a directory        */

typedef struct {
    const char *name;
    const char *syn;
    const char *opts;    /* short letters, ':' suffix = takes a value      */
    const char *longs;   /* long options, space separated; '=' = prefix     */
    int8_t  minpos;
    int8_t  maxpos;      /* -1 = unlimited                                  */
    uint8_t checks;
} spec_t;

static const spec_t SPECS[] = {
 /* name        synopsis                              short opts      long opts  min max  checks */
 {"wc",      "wc [-lwc] FILE",                         "lwc",          "",         1,  1, EXIST1|HELP},
 {"ls",      "ls [-l] [-t] [--raw] [--color-scale[=SCALE]] [--blocks=LIST] [PATH]", "lt", "--raw --blocks= --color-scale --color-size --color-scale= --color-size=", 0, 1, HELP},
 {"dir",     "dir [-l] [-t] [PATH]",                   "lt",           "",         0,  1, HELP},
 {"tree",    "tree [--raw] [PATH]",                    "",             "--raw",    0,  1, DIR1|HELP},
 {"mkdir",   "mkdir [-p] DIR...",                      "p",            "",         1, -1, HELP},
 {"rm",      "rm [-d|-r] [-f] FILE...",                "rdf",          "",         1, -1, HELP},
 {"cp",      "cp [-r] SRC... DST",                     "r",            "",         2, -1, EXIST1|HELP},
 {"mv",      "mv SRC... DST",                          "",             "",         2, -1, EXIST1|HELP},
 {"chmod",   "chmod OCTAL MODE FILE...",               "",             "",         2, -1, EXIST2|HELP},
 {"chown",   "chown UID[:GID] FILE...",                "",             "",         2, -1, EXIST2|HELP},
 {"du",      "du [-s] [PATH]",                         "s",            "",         0,  1, DIR1|HELP},
 {"df",      "df",                                     "",             "",         0,  0, HELP},
 {"edit",    "edit [-c|--cat] [+N|-N] FILE",           "c",            "--cat",    1,  2, HELP},
 {"kill",    "kill [-s SIG] PID...",                   "s:",           "",         1, -1, HELP},
 {"ps",      "ps",                                     "",             "",         0,  0, HELP},
 {"free",    "free",                                   "",             "",         0,  0, HELP},
 {"jobs",    "jobs",                                   "",             "",         0,  0, HELP},
 {"fg",      "fg [JOB]",                               "",             "",         0,  1, HELP},
 {"bg",      "bg",                                     "",             "",         0,  0, HELP},
 {"date",    "date",                                   "",             "",         0,  0, HELP},
 {"uname",   "uname [-a]",                             "a",            "",         0,  0, HELP},
 {"id",      "id [USER]",                              "",             "",         0,  1, HELP},
 {"whoami",  "whoami",                                 "",             "",         0,  0, HELP},
 {"pwd",     "pwd",                                    "",             "",         0,  0, HELP},
 {"uptime",  "uptime",                                 "",             "",         0,  0, HELP},
 {"mem",     "mem",                                    "",             "",         0,  0, HELP},
 {"useradd", "useradd [NAME]",                         "",             "",         0,  1, HELP},
 {"userdel", "userdel NAME",                           "",             "",         1,  1, HELP},
 {"passwd",  "passwd [USER [NEWPW]]",                 "",             "",         0,  2, HELP},
 {"su",      "su [USER]",                              "",             "",         0,  1, HELP},
 {"chcaps",  "chcaps USER CAP1[|CAP2...]",             "",             "",         2,  2, HELP},
 {"caps",    "caps [USER]",                            "",             "",         0,  1, HELP},
 {"users",   "users",                                  "",             "",         0,  0, HELP},
 {"groups",  "groups [USER]",                          "",             "",         0,  1, HELP},
 {"login",   "login [USER]",                           "",             "",         0,  1, HELP},
 {"mount",   "mount [DEVICE PATH]",                    "",             "",         0,  2, HELP},
 {"umount",  "umount PATH",                            "",             "",         1,  1, HELP},
 {"format",  "format",                                 "",             "",         0,  0, HELP},
 {"fsck",    "fsck",                                   "",             "",         0,  0, HELP},
 {"pkg",     "pkg {list|install|remove|info} [NAME]",  "",             "",         0, -1, HELP},
 {"sudo",    "sudo CMD [ARGS...]",                     "",             "",         1, -1, HELP},
 {"curl",    "curl URL",                               "",             "",         1,  1, HELP},
 {"ping",    "ping HOST",                              "",             "",         1,  1, HELP},
 {"dns",     "dns NAME",                               "",             "",         1,  1, HELP},
 {"udp",     "udp HOST PORT",                          "",             "",         2,  2, HELP},
 {"netcfg",  "netcfg [dhcp|static IP]",                "",             "",         0,  2, HELP},
 {"netdiag", "netdiag",                                "",             "",         0,  0, HELP},
 {"sndtest", "sndtest",                                "",             "",         0,  0, HELP},
 {"sndinfo", "sndinfo [-j]",                           "j",            "",         0,  0, HELP},
 {"history", "history [N]",                            "",             "",         0,  1, HELP},
 {"lang",    "lang [en|zh]",                           "",             "",         0,  1, HELP},
 {"theme",   "theme [dark|light|solarized|mono]",      "",             "",         0,  1, HELP},
 {"color",   "color FG [BG]",                          "",             "",         1,  2, HELP},
 {"rgb",     "rgb R G B",                              "",             "",         3,  3, HELP},
 {"palette", "palette [IDX [#RRGGBB]]",                "",             "",         0,  2, HELP},
 {"style",   "style FG [BG]",                          "",             "",         1,  2, HELP},
 {"size",    "size [COLS ROWS]",                       "",             "",         0,  2, HELP},
 {"display", "display",                                "",             "",         0,  0, HELP},
 {"vdiag",   "vdiag",                                  "",             "",         0,  0, HELP},
 {"hwinfo",  "hwinfo",                                 "",             "",         0,  0, HELP},
 {"usb",     "usb [scan]",                             "",             "",         0,  1, HELP},
 {"hive",    "hive {ls|get|set|setdw|mkkey|del|delkey|export|import|chk|save} [ARGS]", "", "", 0, -1, HELP},
 {"reg",     "reg {ls|get|set|...} [ARGS]",            "",             "",         0, -1, HELP},
 {"audit",   "audit [-n COUNT] [--clear]",              "n:",          "--clear",  0,  0, HELP},
 {"cal",     "cal [MONTH YEAR]",                       "",             "",         0,  2, HELP},
 {"cowsay",  "cowsay [TEXT...]",                       "",             "",         0, -1, HELP},
 {"yes",     "yes [STRING...]",                        "",             "",         0, -1, HELP},
 {"echo",    "echo [ARG...]",                          "",             "",         0, -1, HELP},
 {"export",  "export NAME=VALUE...",                   "",             "",         1, -1, HELP},
 {"unset",   "unset NAME...",                          "",             "",         1, -1, HELP},
 {"alias",   "alias [NAME[=CMD]]",                     "",             "",         0,  1, HELP},
 {"unalias", "unalias NAME",                           "",             "",         1,  1, HELP},
 {"run",     "run FILE.ce [ARGS]",                     "",             "",         1, -1, HELP},
 {"dash",    "dash",                                     "",             "",         0,  0, HELP},
 {"pager",   "pager [on|off]",                         "",             "",         0,  1, HELP},
};

static const spec_t *find_spec(const char *cmd) {
    for (unsigned i = 0; i < sizeof(SPECS) / sizeof(SPECS[0]); i++)
        if (p_eq(SPECS[i].name, cmd)) return &SPECS[i];
    return NULL;
}

const char *cli_synopsis(const char *cmd) {
    const spec_t *s = find_spec(cmd);
    return s ? s->syn : NULL;
}
int cli_known(const char *cmd) { return find_spec(cmd) != NULL; }

/* ---- diagnostics ------------------------------------------------------- */
static void say(const char *s) { if (s) vga_write(s); }

int cli_fail(const char *cmd, const char *what, const char *why) {
    say(cmd); say(": ");
    if (what) { say(what); say(": "); }
    say(why);
    say("\n");
    return CLI_FAIL;
}

int cli_usage(const char *cmd, const char *detail) {
    say(cmd); say(": "); say(detail); say("\n");
    say("Try '"); say(cmd); say(" --help' for more information.\n");
    return CLI_USAGE;
}

static int bad_long(const char *cmd, const char *what) {
    say(cmd); say(": unrecognized option '"); say(what); say("'\n");
    say("Try '"); say(cmd); say(" --help' for more information.\n");
    return CLI_USAGE;
}

static int bad_short(const char *cmd, char c) {
    say(cmd); say(": invalid option -- '");
    { char b[2]; b[0] = c; b[1] = 0; say(b); }
    say("'\n");
    say("Try '"); say(cmd); say(" --help' for more information.\n");
    return CLI_USAGE;
}

/* ---- help ------------------------------------------------------------- */
void cli_help(const char *cmd) {
    const spec_t *s = find_spec(cmd);
    if (!s) return;
    say("Usage: "); say(s->syn); say("\n\n");
    say("  -h, --help     show this help and exit\n");
    if (s->opts && s->opts[0]) {
        say("Short options: ");
        for (const char *o = s->opts; *o; o++) {
            if (*o == ':') { say(" ARG"); continue; }
            say("-"); { char b[2]; b[0] = *o; b[1] = 0; say(b); } say(" ");
        }
        say("\n");
    }
    if (s->longs && s->longs[0]) { say("Long options: "); say(s->longs); say("\n"); }
    say("Exit status: 0 success, 1 runtime failure, 2 usage error, 13 permission denied.\n");
}

/* ---- tokenizer --------------------------------------------------------- */
int cli_tokenize(char *buf, char *tok[], int max) {
    int n = 0;
    if (!buf) return 0;
    char *p = buf;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char quote = 0;
        if (*p == '"' || *p == '\'') quote = *p++;
        tok[n++] = p;
        while (*p) {
            if (quote) {
                if (*p == quote) { *p++ = 0; break; }
                if (*p == '\\' && p[1]) { p[0] = p[1]; p += 2; continue; }
                p++;
                continue;
            }
            if (*p == ' ' || *p == '\t') break;
            p++;
        }
        if (!quote && *p) { *p++ = 0; }
    }
    return n;
}

/* ---- validation -------------------------------------------------------- */
static int opt_takes_arg(const spec_t *s, char c) {
    for (const char *o = s->opts; o && *o; o++)
        if (*o == c) return (o[1] == ':') ? 1 : 0;
    return -1;                 /* unknown short option */
}

static int long_ok(const spec_t *s, const char *t) {
    if (!s->longs) return 0;
    const char *p = s->longs;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *q = p;
        int n = 0;
        while (*q && *q != ' ') { q++; n++; }
        int prefix = (p[n - 1] == '=');
        int cpl = prefix ? n - 1 : n;
        int eq = 1;
        for (int i = 0; i < cpl; i++) if (t[i] != p[i]) { eq = 0; break; }
        if (eq) {
            if (prefix) { if (t[cpl] == '=') return 1; }
            else if (t[cpl] == 0) return 1;
        }
        p = q;
    }
    return 0;
}

/* argv[] of max 24, positional list, returns status or CLI_HANDLED_HELP */
int cli_validate(const char *cmd, const char *arg) {
    const spec_t *s = find_spec(cmd);
    if (!s) return 0;

    static char buf[512];
    int n = p_len(arg ? arg : "");
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    for (int i = 0; i < n; i++) buf[i] = arg[i];
    buf[n] = 0;

    char *tok[24];
    int nt = cli_tokenize(buf, tok, 24);
    char *pos[24];
    int np = 0;
    int endopts = 0;
    int help = 0;

    /* v2.68.1: `sudo CMD [ARGS...]` forwards everything after the first
     * operand to the subcommand.  Once a positional operand has been seen
     * for those commands, stop option parsing entirely so `sudo ls -l`
     * passes `-l` through instead of rejecting it as a sudo option. */
    int passthrough_after_first = p_eq(cmd, "sudo") || p_eq(cmd, "run") ||
                                  p_eq(cmd, "cowsay") || p_eq(cmd, "echo") ||
                                  p_eq(cmd, "yes");
    for (int i = 0; i < nt; i++) {
        char *t = tok[i];
        if (passthrough_after_first && np > 0) { if (np < 24) pos[np++] = t; continue; }
        if (!endopts && t[0] == '-' && t[1] == '-' && t[2] == 0) { endopts = 1; continue; }
        if (!endopts && t[0] == '-' && t[1] == '-') {
            if (p_eq(t, "--help") && (s->checks & HELP)) { help = 1; continue; }
            if (p_eq(t, "--version")) { help = 1; continue; }
            if (!long_ok(s, t)) return bad_long(cmd, t);
            continue;
        }
        if (!endopts && p_eq(cmd, "edit") && t[0] == '-' && t[1] >= '0' && t[1] <= '9') {
            if (np < 24) pos[np++] = t;                 /* -N = tail N lines */
            continue;
        }
        if (!endopts && t[0] == '-' && t[1]) {
            for (int k = 1; t[k]; k++) {
                if (t[k] == 'h') { if (s->checks & HELP) help = 1; }
                int need = opt_takes_arg(s, t[k]);
                if (need < 0) return bad_short(cmd, t[k]);
                if (need) {
                    if (!t[k + 1] && i + 1 < nt) i++;   /* consume value */
                    break;
                }
            }
            continue;
        }
        if (np < 24) pos[np++] = t;
    }

    if (help) { cli_help(cmd); return -200; }   /* handled: caller skips */

    if (np < s->minpos) {
        if (s->minpos == 2 && np == 1) {
            say(cmd); say(": missing destination file operand after '");
            say(pos[0]); say("'\n");
            say("Try '"); say(cmd); say(" --help' for more information.\n");
            return CLI_USAGE;
        }
        return cli_usage(cmd, "missing operand");
    }
    if (s->maxpos >= 0 && np > s->maxpos) {
        say(cmd); say(": extra operand '"); say(pos[s->maxpos]); say("'\n");
        say("Try '"); say(cmd); say(" --help' for more information.\n");
        return CLI_USAGE;
    }

    /* existence checks: report the POSIX strerror wording, not "no such file" */
    if (np >= 1 && (s->checks & DIR1)) {
        if (!fs_is_dir(pos[0]))
            return cli_fail(cmd, pos[0], "No such directory");
    } else if (np >= 1 && (s->checks & EXIST1)) {
        uint32_t c, sz;
        if (fs_resolve_file(pos[0], &c, &sz) != 0 && !fs_is_dir(pos[0]))
            return cli_fail(cmd, pos[0], "No such file or directory");
    }
    if (np >= 2 && (s->checks & EXIST2)) {
        uint32_t c, sz;
        if (fs_resolve_file(pos[1], &c, &sz) != 0 && !fs_is_dir(pos[1]))
            return cli_fail(cmd, pos[1], "No such file or directory");
    }
    return 0;
}
