/*
 * sec.c -- v2.67 capability policy + audit ring.
 *
 * Philosophy: "怀疑一切" (trust nothing).
 *   - a command never decides for itself whether it may run; the dispatcher
 *     asks sec_allow() before the builtin body executes;
 *   - the registry can tighten a command's requirement but can never make
 *     the *kernel* trust the caller: root always passes, everyone else must
 *     hold every required bit;
 *   - a policy string that does not parse fails CLOSED (SEC_HOSTILE), so a
 *     corrupted hive cannot silently grant anything;
 *   - every allow/deny is recorded in a fixed-size ring that `audit`
 *     prints, because an audit trail you cannot read is decoration.
 */
#include <stdint.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

#include "sec.h"
#include "user.h"
#include "task.h"
#include "hive.h"
#include "vga.h"
#include "intr.h"
#include "fs.h"
#include "user.h"

/* ---- helpers ----------------------------------------------------------- */
static int s_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}
static void s_cat(char *dst, int max, const char *src) {
    int n = s_len(dst), i = 0;
    while (src && src[i] && n + i < max - 1) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}
static void s_num(char *dst, int max, int v) {
    char t[12]; int i = 0;
    if (v < 0) { s_cat(dst, max, "-"); v = -v; }
    if (!v) { t[i++] = '0'; }
    while (v > 0 && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    char r[12]; int j = 0;
    while (i > 0) r[j++] = t[--i];
    r[j] = 0;
    s_cat(dst, max, r);
}
static void out(const char *s) { vga_write(s); }

/* ---- capability names -------------------------------------------------- */
typedef struct { const char *name; uint32_t bit; } capname_t;
static const capname_t CAPNAMES[] = {
    { "NET",      CAP_NET      },
    { "KILL",     CAP_KILL     },
    { "FSADMIN",  CAP_FSADMIN  },
    { "AUDIO",    CAP_AUDIO    },
    { "USERADM",  CAP_USERADM  },
    { "DEVICE",   CAP_DEVICE   },
    { "SYS",      CAP_SYS      },
    { "FORMAT",   CAP_FORMAT   },
    { "FSCK",     CAP_FSCK     },
    { "RAWIO",    CAP_RAWIO    },
    { "HWINFO",   CAP_HWINFO   },
    { "REBOOT",   CAP_REBOOT   },
    { "DIAG",     CAP_DIAG     },
    { "ALL",      CAP_ALL      },
};
#define NCAPS (int)(sizeof(CAPNAMES) / sizeof(CAPNAMES[0]))

int sec_parse_caps(const char *s, uint32_t *out_bits) {
    if (!out_bits) return -1;
    *out_bits = 0;
    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;                       /* empty = no requirement */
    if (s_eq(s, "none") || s_eq(s, "guest")) return 0;
    if (s_eq(s, "admin") || s_eq(s, "all")) { *out_bits = CAP_ALL; return 0; }

    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '|' || *p == ',') p++;
        if (!*p) break;
        char tok[24]; int n = 0;
        while (*p && *p != '|' && *p != ',' && *p != ' ' && n < 23) tok[n++] = *p++;
        tok[n] = 0;
        /* numeric form 0x... or decimal */
        if ((tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) ||
            (tok[0] >= '0' && tok[0] <= '9')) {
            uint32_t v = 0; int k = (tok[1] == 'x' || tok[1] == 'X') ? 2 : 0;
            for (; tok[k]; k++) {
                int d;
                if (tok[k] >= '0' && tok[k] <= '9') d = tok[k] - '0';
                else if (tok[k] >= 'a' && tok[k] <= 'f') d = tok[k] - 'a' + 10;
                else if (tok[k] >= 'A' && tok[k] <= 'F') d = tok[k] - 'A' + 10;
                else return -1;
                v = v * ((k >= 2) ? 16u : 10u) + (uint32_t)d;
            }
            *out_bits |= v;
            continue;
        }
        int hit = 0;
        for (int i = 0; i < NCAPS; i++)
            if (s_eq(CAPNAMES[i].name, tok)) { *out_bits |= CAPNAMES[i].bit; hit = 1; break; }
        if (!hit) return -1;                 /* unknown token: fail closed */
    }
    return 0;
}

void sec_caps_str(uint32_t caps, char *out_s, int max) {
    if (!out_s || max < 1) return;
    out_s[0] = 0;
    if (caps == 0) { s_cat(out_s, max, "none"); return; }
    if (caps == CAP_ALL) { s_cat(out_s, max, "ALL"); return; }
    int first = 1;
    for (int i = 0; i < NCAPS; i++) {
        if (CAPNAMES[i].bit == CAP_ALL) continue;
        if (!(caps & CAPNAMES[i].bit)) continue;
        if (!first) s_cat(out_s, max, "|");
        s_cat(out_s, max, CAPNAMES[i].name);
        first = 0;
    }
    if (first) { s_cat(out_s, max, "0x"); s_num(out_s, max, (int)caps); }
}

/* ---- built-in requirement table ---------------------------------------- */
typedef struct { const char *cmd; uint32_t caps; } need_t;
static const need_t NEEDS[] = {
    { "reboot",    CAP_SYS | CAP_REBOOT },
    { "shutdown",  CAP_SYS | CAP_REBOOT },
    { "format",    CAP_FSADMIN | CAP_FORMAT },
    { "fsck",      CAP_FSADMIN | CAP_FSCK },
    { "mount",     CAP_FSADMIN },
    { "umount",    CAP_FSADMIN },
    { "useradd",   CAP_USERADM },
    { "userdel",   CAP_USERADM },
    { "passwd",    CAP_USERADM },
    { "chcaps",    CAP_USERADM },
    { "su",        CAP_USERADM },
    { "chown",     CAP_USERADM },
    { "kill",      CAP_KILL },
    { "curl",      CAP_NET },
    { "ping",      CAP_NET },
    { "dns",       CAP_NET },
    { "udp",       CAP_NET },
    { "netcfg",    CAP_NET },
    { "netdiag",   CAP_NET },
    { "sndtest",   CAP_AUDIO },
    { "sndinfo",   CAP_AUDIO },
    { "hwinfo",    CAP_HWINFO },
    { "vdiag",     CAP_DIAG },
    { "hive",      CAP_USERADM },   /* write verbs re-check inside */
    { "reg",       CAP_USERADM },
};

static uint32_t builtin_need(const char *cmd) {
    for (unsigned i = 0; i < sizeof(NEEDS) / sizeof(NEEDS[0]); i++)
        if (s_eq(NEEDS[i].cmd, cmd)) return NEEDS[i].caps;
    return 0;
}

/* Registry override: HKSEC\Policy\Cmd\<cmd>\Caps.
 * Present and empty  -> "no requirement" (explicit downgrade).
 * Present and bogus  -> *bad_policy = 1 (fail closed). */
uint32_t sec_required(const char *cmd, int *bad_policy) {
    if (bad_policy) *bad_policy = 0;
    uint32_t base = builtin_need(cmd);
    if (!cmd || !*cmd) return base;
    char path[HIVE_PATH_MAX];
    path[0] = 0;
    s_cat(path, sizeof(path), "HKSEC\\Policy\\Cmd\\");
    s_cat(path, sizeof(path), cmd);
    s_cat(path, sizeof(path), "\\Caps");
    if (!hive_exists(path)) return base;
    const char *v = hive_sz(path, 0);
    if (!v) { if (bad_policy) *bad_policy = 1; return base; }
    uint32_t bits = 0;
    if (sec_parse_caps(v, &bits) != 0) {
        if (bad_policy) *bad_policy = 1;
        return base;
    }
    return bits;
}

int sec_allow(const char *cmd, int uid, uint32_t caps) {
    if (uid == UID_ROOT) return SEC_ALLOW;          /* root is root */
    int bad = 0;
    uint32_t need = sec_required(cmd, &bad);
    if (bad) return SEC_HOSTILE;
    if (!need) return SEC_ALLOW;
    if ((caps & need) == need) return SEC_ALLOW;
    return SEC_DENIED;
}

/* ---- audit ring -------------------------------------------------------- */
#define AUDIT_ROWS 48
typedef struct {
    uint32_t tick;
    int16_t  uid;
    int16_t  rc;
    uint8_t  used;
    char     kind[8];
    char     what[44];
} audit_ent_t;

static audit_ent_t g_audit[AUDIT_ROWS];
static int g_audit_head;      /* next slot */
static int g_audit_count;

void sec_audit(int uid, const char *kind, const char *what, int rc) {
    audit_ent_t *e = &g_audit[g_audit_head];
    e->tick = timer_ticks;
    e->uid  = (int16_t)uid;
    e->rc   = (int16_t)rc;
    e->used = 1;
    int i = 0;
    while (kind && kind[i] && i < 7) { e->kind[i] = kind[i]; i++; }
    e->kind[i] = 0;
    int j = 0;
    while (what && what[j] && j < 43) { e->what[j] = what[j]; j++; }
    e->what[j] = 0;
    g_audit_head = (g_audit_head + 1) % AUDIT_ROWS;
    if (g_audit_count < AUDIT_ROWS) g_audit_count++;
}

void sec_audit_clear(void) {
    for (int i = 0; i < AUDIT_ROWS; i++) g_audit[i].used = 0;
    g_audit_head = 0; g_audit_count = 0;
}

int sec_audit_dump(int n) {
    int total = g_audit_count;
    if (n > 0 && n < total) total = n;
    int printed = 0;
    out("  TICKS  UID  RESULT  KIND      COMMAND\n");
    for (int k = total - 1; k >= 0; k--) {
        int idx = (g_audit_head - 1 - k + AUDIT_ROWS * 2) % AUDIT_ROWS;
        audit_ent_t *e = &g_audit[idx];
        if (!e->used) continue;
        char line[96]; line[0] = 0;
        out("  "); s_num(line, sizeof(line), (int)e->tick); s_cat(line, sizeof(line), "");
        out(line); line[0] = 0;
        for (int p = s_len(line); p < 8; p++) out(" ");
        s_num(line, sizeof(line), (int)e->uid); out(line);
        out("    ");
        out(e->rc == 0 ? "allow " : "DENY  ");
        out("   ");
        out(e->kind);
        for (int p = s_len(e->kind); p < 10; p++) out(" ");
        out(e->what);
        out("\n");
        printed++;
    }
    if (!printed) out("  (audit ring empty)\n");
    return printed;
}

int sec_exec_check(const char *path, int uid, char *why, int whymax) {
    if (why && whymax > 0) why[0] = 0;
    if (!path || !*path) return -1;
    int strict = (int)hive_dw("HKSEC\\Policy\\Exec\\Strict", 1);
    if (!strict) return 0;
    int fuid = UID_ROOT, fgid = 0, fmode = 0644;
    if (acl_lookup(path, &fuid, &fgid, &fmode) != 0) {
        fuid = UID_ROOT; fgid = 0; fmode = 0644;
    }
    /* world-writable (or group-writable in a shared group) binaries are
     * the textbook local-root upgrade path: anything that can drop a file
     * in that directory can run code as whoever launches it. */
    if (fmode & 0002) {
        if (why) s_cat(why, whymax, "world-writable binary (chmod 6xx or 7xx)");
        return -1;
    }
    if (fmode & 0020) {
        if (why) s_cat(why, whymax, "group-writable binary in a shared group");
        return -1;
    }
    (void)uid;
    return 0;
}

int sec_policy_brief(char *out_s, int max) {
    if (!out_s || max < 1) return -1;
    out_s[0] = 0;
    s_cat(out_s, max, "Foreground.AllowCtrlC=");
    s_cat(out_s, max, hive_dw("HKSEC\\Policy\\Foreground\\AllowCtrlC", 1) ? "1" : "0");
    s_cat(out_s, max, " AllowCtrlZ=");
    s_cat(out_s, max, hive_dw("HKSEC\\Policy\\Foreground\\AllowCtrlZ", 1) ? "1" : "0");
    s_cat(out_s, max, " UnknownBinary.AllowUnsigned=");
    s_cat(out_s, max, hive_dw("HKSEC\\Policy\\UnknownBinary\\AllowUnsigned", 1) ? "1" : "0");
    s_cat(out_s, max, " UnknownBinary.AllowFromWritableDir=");
    s_cat(out_s, max, hive_dw("HKSEC\\Policy\\UnknownBinary\\AllowFromWritableDir", 0) ? "1" : "0");
    return 0;
}
