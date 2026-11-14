/*
 * user.c -- CatOS user database + capability + ACL security model.
 *
 * See include/user.h for the model.  This module owns the two persistent
 * stores on the FAT32 volume and the kernel-side permission check used by
 * the syscall layer (fs_perm_check).
 */
#include "user.h"
#include "task.h"
#include "fs.h"

/* ---- tiny local string helpers (kernel has no libc) -------------------- */
static int ustrlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int useq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static void ucopy(char *d, const char *s, int cap) {
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static int uprefix(const char *s, const char *pre) {
    while (*pre) { if (*s != *pre) return 0; s++; pre++; }
    return 1;
}

static uint32_t parse_uint(const char *s, int base) {
    uint32_t v = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * (uint32_t)base + (uint32_t)d;
        s++;
    }
    return v;
}

static void emit_uint(char *out, uint32_t v, int base) {
    static const char hexd[] = "0123456789abcdef";
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = hexd[v % (uint32_t)base]; v /= (uint32_t)base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

/* ---- user database ----------------------------------------------------- */
static user_rec_t users[MAX_USERS];
static int users_dirty;

extern volatile uint32_t timer_ticks;   /* hw.c IRQ0 counter */

uint32_t user_hash_pw(const char *pw) {
    /* djb2 -- legacy pre-v2.22 scheme, kept ONLY to verify old /etc/passwd
     * records (and transparently upgrade them on the next login). */
    uint32_t h = 5381;
    for (; pw && *pw; pw++) h = h * 33u + (uint8_t)*pw;
    return h;
}

/* v2.22 salted iterated hash.  Deliberately NOT cryptographic (no crypto
 * primitives in this kernel) -- but unlike the bare djb2 above, a leaked
 * /etc/passwd no longer yields password candidates in microseconds: each
 * guess costs 8192 mixing rounds and every record has its own random salt,
 * so identical passwords hash differently and precomputed tables are
 * useless.  Constants are FNV-1a; rounds fixed at 8192 (~ms on this CPU). */
#define PW_ROUNDS 8192u

static uint32_t pw_hash_salted(const char *pw, uint32_t salt) {
    uint32_t h = 2166136261u ^ salt;
    for (uint32_t r = 0; r < PW_ROUNDS; r++) {
        for (const char *p = pw; p && *p; p++) {
            h ^= (uint8_t)*p;
            h *= 16777619u;
        }
        h ^= (uint8_t)r;
        h *= 16777619u;
        h ^= h >> 13;
        h *= 16777619u;
        h ^= salt + r;
    }
    /* final avalanche (murmur3 fmix32) */
    h ^= h >> 16; h *= 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static const char PWHEX[] = "0123456789abcdef";

static void pw_emit32(uint32_t v, char out[8]) {
    for (int i = 7; i >= 0; i--) { out[i] = PWHEX[v & 0xFu]; v >>= 4; }
}

static int pw_parse32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return -1;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 0;
}

void user_pw_set_rec(user_rec_t *u, const char *pw) {
    /* salt: IRQ0 tick counter mixed with the record's address -- on this
     * non-preemptible kernel the tick phase at password-set time is the
     * best entropy available; enough to distinct two same-password users. */
    uint32_t salt = timer_ticks * 2654435761u
                  ^ (uint32_t)(uintptr_t)u * 0x9E3779B9u
                  ^ (uint32_t)(uintptr_t)pw;
    char d[8];
    /* record = "$S$" + 8 hex salt + '$' + 8 hex digest  (20 chars + NUL) */
    u->pw[0] = '$'; u->pw[1] = 'S'; u->pw[2] = '$';
    pw_emit32(salt, u->pw + 3);
    u->pw[11] = '$';
    pw_emit32(pw_hash_salted(pw, salt), d);
    for (int i = 0; i < 8; i++) u->pw[12 + i] = d[i];
    u->pw[20] = 0;
}

int user_pw_is_set(const user_rec_t *u) {
    return u && u->pw[0] != 0;
}

int user_check_pw(user_rec_t *u, const char *pw) {
    if (!u) return -1;
    if (u->pw[0] == 0) return 0;               /* open account */
    if (u->pw[0] == '$' && u->pw[1] == 'S' && u->pw[2] == '$' && u->pw[3] != '$') {
        /* "$S$" + 8 hex salt + 8 hex digest */
        uint32_t salt, want;
        if (u->pw[11] != '$' || pw_parse32(u->pw + 3, &salt) != 0 ||
            pw_parse32(u->pw + 12, &want) != 0) return -1;
        return pw_hash_salted(pw ? pw : "", salt) == want ? 0 : -1;
    }
    /* legacy bare-djb2 record: verify, then upgrade in place */
    uint32_t want = parse_uint(u->pw, 16);
    if (user_hash_pw(pw) == want) {
        user_pw_set_rec(u, pw);
        users_dirty = 1;
        return 0;
    }
    return -1;
}

user_rec_t *user_by_name(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].used && useq(users[i].name, name)) return &users[i];
    return 0;
}

user_rec_t *user_by_uid(int uid) {
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].used && users[i].uid == (uint16_t)uid) return &users[i];
    return 0;
}

user_rec_t *user_at(int idx) {
    if (idx < 0 || idx >= MAX_USERS) return 0;
    return &users[idx];
}

uint32_t user_caps_of(int uid) {
    user_rec_t *u = user_by_uid(uid);
    if (u) return u->caps;
    return uid == UID_ROOT ? CAP_ALL : 0;
}

static user_rec_t *user_alloc_slot(void) {
    for (int i = 0; i < MAX_USERS; i++)
        if (!users[i].used) return &users[i];
    return 0;
}

int user_add(const char *name, int uid, int gid,
             const char *home, const char *shell, uint32_t caps) {
    if (!name || !*name || ustrlen(name) >= 16) return -3;
    if (user_by_name(name)) return -5;                 /* EEXIST */
    if (user_by_uid(uid)) return -5;
    user_rec_t *u = user_alloc_slot();
    if (!u) return -4;                                 /* table full */
    ucopy(u->name, name, 16);
    u->uid = (uint16_t)uid;
    u->gid = (uint16_t)gid;
    ucopy(u->home, home && *home ? home : "/", 48);
    ucopy(u->shell, shell && *shell ? shell : "/bin/shell.ce", 32);
    u->caps = caps;
    u->pw[0] = 0;
    u->used = 1;
    users_dirty = 1;
    return 0;
}

int user_del(const char *name) {
    if (!name || useq(name, "root")) return -3;        /* root is undeletable */
    user_rec_t *u = user_by_name(name);
    if (!u) return -1;
    u->used = 0;
    users_dirty = 1;
    return 0;
}

int user_set_pw(const char *name, const char *pw) {
    user_rec_t *u = user_by_name(name);
    if (!u) return -1;
    if (pw && *pw) user_pw_set_rec(u, pw);
    else u->pw[0] = 0;                         /* clear = open account */
    users_dirty = 1;
    return 0;
}

int user_set_caps(const char *name, uint32_t caps) {
    user_rec_t *u = user_by_name(name);
    if (!u) return -1;
    u->caps = caps;
    users_dirty = 1;
    return 0;
}

int user_save(void) {
    static char buf[4096];
    int o = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].used) continue;
        user_rec_t *u = &users[i];
        char num[12];
        /* name:uid:gid:home:shell:caps:pw (v2.22 salted or legacy hex) */
        for (int j = 0; u->name[j] && o < 4000; j++) buf[o++] = u->name[j];
        buf[o++] = ':';
        emit_uint(num, u->uid, 10);  for (int j = 0; num[j]; j++) buf[o++] = num[j];
        buf[o++] = ':';
        emit_uint(num, u->gid, 10);  for (int j = 0; num[j]; j++) buf[o++] = num[j];
        buf[o++] = ':';
        for (int j = 0; u->home[j] && o < 4000; j++) buf[o++] = u->home[j];
        buf[o++] = ':';
        for (int j = 0; u->shell[j] && o < 4000; j++) buf[o++] = u->shell[j];
        buf[o++] = ':';
        emit_uint(num, u->caps, 16); for (int j = 0; num[j]; j++) buf[o++] = num[j];
        buf[o++] = ':';
        for (int j = 0; u->pw[j] && o < 4000; j++) buf[o++] = u->pw[j];
        buf[o++] = '\n';
    }
    buf[o] = 0;
    (void)fs_delete("/etc/passwd");
    if (fs_write_raw("/etc/passwd", buf, (uint32_t)o) != 0) return -1;
    users_dirty = 0;
    return 0;
}

static void user_seed_default(void) {
    for (int i = 0; i < MAX_USERS; i++) users[i].used = 0;
    /* root: the initial account.  Full capabilities, no password. */
    user_add("root", UID_ROOT, 0, "/root", "/bin/shell.ce", CAP_ALL);
    /* default unprivileged account: enough for the historical uid-100
     * behaviour (net/kill/audio/device), no administration rights. */
    user_add("user", UID_USER, UID_USER, "/home/user", "/bin/shell.ce",
             CAP_NET | CAP_KILL | CAP_AUDIO | CAP_DEVICE);
    user_save();
}

/* split a line into ':' fields; returns field count */
static int split_fields(char *line, char *fld[], int maxf) {
    int n = 0;
    char *p = line;
    fld[n++] = p;
    while (*p && n < maxf) {
        if (*p == ':') { *p = 0; fld[n++] = p + 1; }
        p++;
    }
    /* strip trailing \r\n of the last field */
    char *last = fld[n - 1];
    for (char *q = last; *q; q++)
        if (*q == '\n' || *q == '\r') { *q = 0; break; }
    return n;
}

void user_db_init(void) {
    for (int i = 0; i < MAX_USERS; i++) users[i].used = 0;
    static char buf[4096];
    int n = fs_read_text("/etc/passwd", buf, sizeof(buf));
    if (n <= 0) { user_seed_default(); return; }
    buf[n] = 0;
    int restored = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        int eol = (*nl == '\n');
        *nl = 0;
        char *fld[7];
        if (split_fields(line, fld, 7) == 7 && fld[0][0]) {
            user_rec_t *u = user_alloc_slot();
            if (u) {
                ucopy(u->name, fld[0], 16);
                u->uid = (uint16_t)parse_uint(fld[1], 10);
                u->gid = (uint16_t)parse_uint(fld[2], 10);
                ucopy(u->home, fld[3], 48);
                ucopy(u->shell, fld[4], 32);
                u->caps = parse_uint(fld[5], 16);
                ucopy(u->pw, fld[6], 24);
                /* legacy "0" meant OPEN account; keep that meaning */
                if (u->pw[0] != '$' && parse_uint(u->pw, 16) == 0)
                    u->pw[0] = 0;
                u->used = 1;
                restored++;
            }
        }
        line = eol ? nl + 1 : 0;
    }
    if (restored == 0) user_seed_default();
}

/* ---- access control list ---------------------------------------------- */
static acl_entry_t acl_tab[ACL_MAX];
static int acl_dirty;

/* ACL keys are CANONICAL paths (absolute, dot/dotdot/slash normalised,
 * ASCII-lowercased).  FAT32 resolves "/BIN/X.CE", "/bin//x.ce" and
 * "/bin/../bin/x.ce" to the same file, but a raw-string comparison used
 * to treat them as different ACL keys -- a trivial chmod 000 bypass
 * (#97).  Store and look up canonically so one file == one entry. */
static void acl_key(const char *path, char *out) {
    fs_canon_path(path, out, FS_PATH_MAX);
}

int acl_lookup(const char *path, int *uid, int *gid, int *mode) {
    if (!path) return -1;
    char key[FS_PATH_MAX];
    acl_key(path, key);
    for (int i = 0; i < ACL_MAX; i++) {
        if (acl_tab[i].used && useq(acl_tab[i].path, key)) {
            if (uid) *uid = acl_tab[i].uid;
            if (gid) *gid = acl_tab[i].gid;
            if (mode) *mode = acl_tab[i].mode;
            return 0;
        }
    }
    return -1;
}

int acl_set(const char *path, int uid, int gid, int mode) {
    if (!path || !*path) return -3;
    char key[FS_PATH_MAX];
    acl_key(path, key);
    acl_entry_t *slot = 0;
    for (int i = 0; i < ACL_MAX; i++) {
        if (acl_tab[i].used && useq(acl_tab[i].path, key)) { slot = &acl_tab[i]; break; }
        if (!slot && !acl_tab[i].used) slot = &acl_tab[i];
    }
    if (!slot) return -4;
    ucopy(slot->path, key, 160);
    slot->uid = (uint16_t)uid;
    slot->gid = (uint16_t)gid;
    slot->mode = (uint16_t)(mode & 0xFFF);
    slot->used = 1;
    acl_dirty = 1;
    return 0;
}

int acl_default(const char *path, int uid, int gid, int mode) {
    /* seed-time insert that never replaces an existing entry */
    for (int i = 0; i < ACL_MAX; i++)
        if (acl_tab[i].used && useq(acl_tab[i].path, path)) return 0;
    return acl_set(path, uid, gid, mode);
}

int acl_save(void) {
    static char buf[4096];
    int o = 0;
    for (int i = 0; i < ACL_MAX; i++) {
        if (!acl_tab[i].used) continue;
        acl_entry_t *a = &acl_tab[i];
        char num[12];
        for (int j = 0; a->path[j] && o < 4000; j++) buf[o++] = a->path[j];
        buf[o++] = ':';
        emit_uint(num, a->uid, 10);  for (int j = 0; num[j]; j++) buf[o++] = num[j];
        buf[o++] = ':';
        emit_uint(num, a->gid, 10);  for (int j = 0; num[j]; j++) buf[o++] = num[j];
        buf[o++] = ':';
        emit_uint(num, a->mode, 8);  for (int j = 0; num[j]; j++) buf[o++] = num[j];
        buf[o++] = '\n';
    }
    buf[o] = 0;
    (void)fs_delete("/etc/acl");
    if (fs_write_raw("/etc/acl", buf, (uint32_t)o) != 0) return -1;
    acl_dirty = 0;
    return 0;
}

void acl_init(void) {
    for (int i = 0; i < ACL_MAX; i++) acl_tab[i].used = 0;
    static char buf[4096];
    int n = fs_read_text("/etc/acl", buf, sizeof(buf));
    if (n <= 0) {
        /* same defaults the old fs_perm_seed() established, now with
         * group semantics: world-writable scratch + read-only system */
        acl_set("/home",       UID_ROOT, 0, 0777);
        acl_set("/tmp",        UID_ROOT, 0, 0777);
        acl_set("/bin",        UID_ROOT, 0, 0755);
        acl_set("/appdevelop", UID_ROOT, 0, 0755);
        acl_set("/etc",        UID_ROOT, 0, 0755);
        acl_set("/root",       UID_ROOT, 0, 0700);
        acl_save();
        return;
    }
    buf[n] = 0;
    int restored = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        int eol = (*nl == '\n');
        *nl = 0;
        char *fld[4];
        if (split_fields(line, fld, 4) == 4 && fld[0][0]) {
            acl_set(fld[0],
                    (int)parse_uint(fld[1], 10),
                    (int)parse_uint(fld[2], 10),
                    (int)parse_uint(fld[3], 8));
            restored++;
        }
        line = eol ? nl + 1 : 0;
    }
    if (restored == 0) {
        acl_set("/home", UID_ROOT, 0, 0777);
        acl_set("/tmp", UID_ROOT, 0, 0777);
        acl_set("/bin", UID_ROOT, 0, 0755);
        acl_set("/appdevelop", UID_ROOT, 0, 0755);
        acl_set("/etc", UID_ROOT, 0, 0755);
        acl_set("/root", UID_ROOT, 0, 0700);
        acl_save();
    }
}

/* ---- the permission check used by every file syscall ------------------- */

int fs_perm_check(const char *path, int uid, int gid, int need) {
    /* need is a bitmask of PERM_READ/WRITE/EXEC.  mode is the standard
     * 12-bit unix layout: owner bits 6-8, group 3-5, other 0-2. */
    if (uid == UID_ROOT) return 0;
    /* #97: canonicalise BOTH the ACL lookup and the scratch whitelist.
     * Without this, "/TMP/x" misses "/tmp/" and, worse, a case variant
     * of an ACL entry falls into "no entry -> read/exec allowed". */
    char key[FS_PATH_MAX];
    fs_canon_path(path, key, sizeof(key));
    int fuid, fgid, mode;
    if (acl_lookup(key, &fuid, &fgid, &mode) == 0) {
        int have;
        if (uid == fuid)            have = (mode >> 6) & 7;   /* owner */
        else if (gid == fgid)       have = (mode >> 3) & 7;   /* group */
        else                        have = mode & 7;          /* other */
        return (have & need) == need ? 0 : -13;
    }
    /* no ACL entry: read/exec default allowed (0644/0755 semantics);
     * writes go through the world-writable scratch whitelist */
    if (need & PERM_WRITE) {
        int scratch = (uprefix(key, "/home/") || useq(key, "/home") ||
                       uprefix(key, "/tmp/") || useq(key, "/tmp"));
        if (!scratch) return -13;
    }
    return 0;
}
