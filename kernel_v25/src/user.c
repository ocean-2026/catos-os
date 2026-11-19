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
#include "rtc.h"

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

/* #185: true only when every character is a hex digit.  parse_uint stops at
 * the first non-digit and returns 0, so it alone cannot tell "value zero"
 * from "not a number". */
static int is_hex_word(const char *s) {
    if (!s || !*s) return 0;
    for (const char *q = s; *q; q++) {
        char c = *q;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
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

/* #164/#178/#166: bounded append for the persistent DB save buffers.
 * Every separator, digit and newline used to be written unconditionally
 * at buf[o++] -- when a static save buffer approached its limit that
 * spilled into adjacent bss.  These helpers stop appending once one byte
 * short of the capacity; the caller still places the trailing NUL at
 * buf[o].  Truncation (rather than overflow) is the safe fallback. */
static inline int buf_put(char *buf, int *o, int cap, char c) {
    if (*o < cap - 1) { buf[(*o)++] = c; return 1; }
    return 0;
}
static inline void buf_str(char *buf, int *o, int cap, const char *s) {
    while (s && *s) { if (!buf_put(buf, o, cap, *s++)) break; }
}
static inline void buf_num(char *buf, int *o, int cap, const char *num) {
    while (num && *num) { if (!buf_put(buf, o, cap, *num++)) break; }
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

/* v2.51: rounds are a tunable knob.  Default keeps the v2.22 cost (~ms on
 * this CPU); a later admin/boot knob may raise pw_rounds without touching
 * the record format.  Memory-hard schemes (Argon2/scrypt) are deferred by
 * design -- this kernel has no crypto primitives. */
#define PW_ROUNDS_DEFAULT 8192u
static uint32_t pw_rounds = PW_ROUNDS_DEFAULT;

static uint32_t avalanche32(uint32_t h) {
    h ^= h >> 16; h *= 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* v1 (v2.22) 32-bit salted FNV-1a mix, retained ONLY to verify old $S$
 * records; those are transparently upgraded to the v2 128-bit record on
 * the next successful login. */
static uint32_t pw_hash_salted32(const char *pw, uint32_t salt) {
    uint32_t h = 2166136261u ^ salt;
    for (uint32_t r = 0; r < PW_ROUNDS_DEFAULT; r++) {
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
    return avalanche32(h);
}

/* v2.51 128-bit digest: four FNV-1a lanes with distinct seed constants, each
 * mixed through password + salt + round counter, with per-round cross-lane
 * diffusion so the lanes are not four independent hashes.  out[0..3] are the
 * four 32-bit digest words. */
static void pw_hash128(const char *pw, uint32_t s0, uint32_t s1,
                       uint32_t out[4]) {
    uint32_t h0 = 2166136261u ^ s0;
    uint32_t h1 = 2166136261u ^ s1 ^ 0x9E3779B9u;
    uint32_t h2 = 2166136261u ^ s0 ^ 0x85EBCA6Bu;
    uint32_t h3 = 2166136261u ^ s1 ^ 0xC2B2AE35u;
    for (uint32_t r = 0; r < pw_rounds; r++) {
        for (const char *p = pw; p && *p; p++) {
            uint8_t c = (uint8_t)*p;
            h0 ^= c; h0 *= 16777619u;
            h1 ^= c; h1 *= 16777619u;
            h2 ^= c; h2 *= 16777619u;
            h3 ^= c; h3 *= 16777619u;
        }
        h0 ^= (uint8_t)r;       h0 *= 16777619u;
        h1 ^= (uint8_t)(r >> 8);  h1 *= 16777619u;
        h2 ^= (uint8_t)(r >> 16); h2 *= 16777619u;
        h3 ^= (uint8_t)(r >> 24); h3 *= 16777619u;
        /* cross-lane diffusion */
        h0 ^= h1; h1 *= 16777619u;
        h1 ^= h2; h2 *= 16777619u;
        h2 ^= h3; h3 *= 16777619u;
        h3 ^= h0; h0 *= 16777619u;
    }
    out[0] = avalanche32(h0); out[1] = avalanche32(h1);
    out[2] = avalanche32(h2); out[3] = avalanche32(h3);
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

/* v2.51: 64-bit salt from three independent sources mixed through an
 * avalanche: IRQ0 tick phase, RTC wall-clock components, and the record /
 * password pointer addresses.  The weak v2.22 salt was tick*prime ^ address
 * only; RTC adds a wall-clock component the tick counter does not carry. */
static void pw_salt_gen(const user_rec_t *u, const char *pw,
                        uint32_t *lo, uint32_t *hi) {
    rtc_time_t t;
    rtc_read(&t);
    uint32_t rtc = ((uint32_t)(t.year - 1980) << 26)
                 ^ ((uint32_t)t.month << 22)
                 ^ ((uint32_t)t.day << 17)
                 ^ ((uint32_t)t.hour << 12)
                 ^ ((uint32_t)t.minute << 6)
                 ^ (uint32_t)t.second;
    uint32_t a = timer_ticks * 2654435761u
               ^ (uint32_t)(uintptr_t)u * 0x9E3779B9u
               ^ (uint32_t)(uintptr_t)pw;
    uint32_t b = rtc ^ (timer_ticks >> 13)
               ^ (uint32_t)(uintptr_t)u * 0x85EBCA6Bu
               ^ (uint32_t)(uintptr_t)pw * 0xC2B2AE35u;
    *lo = avalanche32(a);
    *hi = avalanche32(b);
}

void user_pw_set_rec(user_rec_t *u, const char *pw) {
    uint32_t sl, sh;
    pw_salt_gen(u, pw, &sl, &sh);
    uint32_t d[4];
    pw_hash128(pw ? pw : "", sl, sh, d);
    /* record = "$S$" + 16 hex salt (lo+hi) + '$' + 32 hex digest (4 words).
     * 3+16+1+32 = 52 chars + NUL; pw[] holds 64. */
    u->pw[0] = '$'; u->pw[1] = 'S'; u->pw[2] = '$';
    pw_emit32(sl, u->pw + 3);
    pw_emit32(sh, u->pw + 11);
    u->pw[19] = '$';
    for (int i = 0; i < 4; i++) pw_emit32(d[i], u->pw + 20 + i * 8);
    u->pw[52] = 0;
}

int user_pw_is_set(const user_rec_t *u) {
    return u && u->pw[0] != 0;
}

int user_check_pw(user_rec_t *u, const char *pw) {
    if (!u) return -1;
    if (u->pw[0] == 0) return 0;               /* open account */
    if (u->pw[0] == '$' && u->pw[1] == 'S' && u->pw[2] == '$') {
        if (u->pw[19] == '$') {
            /* v2: "$S$" + 16 hex salt + '$' + 32 hex digest */
            uint32_t s0, s1, w[4], d[4];
            if (pw_parse32(u->pw + 3, &s0) != 0 ||
                pw_parse32(u->pw + 11, &s1) != 0) return -1;
            for (int i = 0; i < 4; i++)
                if (pw_parse32(u->pw + 20 + i * 8, &w[i]) != 0) return -1;
            pw_hash128(pw ? pw : "", s0, s1, d);
            for (int i = 0; i < 4; i++) if (d[i] != w[i]) return -1;
            return 0;
        }
        if (u->pw[11] == '$') {
            /* v1: "$S$" + 8 hex salt + '$' + 8 hex digest.  Verify with the
             * legacy 32-bit mix, then transparently upgrade to v2. */
            uint32_t salt, want;
            if (pw_parse32(u->pw + 3, &salt) != 0 ||
                pw_parse32(u->pw + 12, &want) != 0) return -1;
            if (pw_hash_salted32(pw ? pw : "", salt) != want) return -1;
            user_pw_set_rec(u, pw);
            users_dirty = 1;
            return 0;
        }
        return -1;
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

/* v2.25 preset capability templates (useradd -p NAME / useredit UI) */
int user_preset_caps(const char *preset, uint32_t *out) {
    static const struct { const char *n; uint32_t c; } P[] = {
        { "admin",     CAP_ALL },
        { "operator",  CAP_NET | CAP_KILL | CAP_AUDIO | CAP_FSCK |
                       CAP_HWINFO | CAP_DIAG },
        { "developer", CAP_NET | CAP_AUDIO | CAP_RAWIO | CAP_DIAG },
        { "viewer",    CAP_HWINFO },
        { "guest",     0 },
    };
    if (!preset || !out) return -1;
    for (unsigned i = 0; i < sizeof(P) / sizeof(P[0]); i++)
        if (useq(P[i].n, preset)) { *out = P[i].c; return 0; }
    return -1;
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
    const int cap = (int)sizeof(buf);
    int o = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].used) continue;
        user_rec_t *u = &users[i];
        char num[12];
        /* name:uid:gid:home:shell:caps:pw (v2.22 salted or legacy hex) */
        buf_str(buf, &o, cap, u->name);
        buf_put(buf, &o, cap, ':');
        emit_uint(num, u->uid, 10);  buf_num(buf, &o, cap, num);
        buf_put(buf, &o, cap, ':');
        emit_uint(num, u->gid, 10);  buf_num(buf, &o, cap, num);
        buf_put(buf, &o, cap, ':');
        buf_str(buf, &o, cap, u->home);
        buf_put(buf, &o, cap, ':');
        buf_str(buf, &o, cap, u->shell);
        buf_put(buf, &o, cap, ':');
        emit_uint(num, u->caps, 16); buf_num(buf, &o, cap, num);
        buf_put(buf, &o, cap, ':');
        buf_str(buf, &o, cap, u->pw);
        buf_put(buf, &o, cap, '\n');
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

/* split a line into ':' fields; returns field count.
 * (#163) every slot is initialised NULL: a line with fewer fields than
 * maxf must leave the unused entries as NULL instead of uninitialised
 * stack garbage, so callers can safely test fld[k] == NULL. */
static int split_fields(char *line, char *fld[], int maxf) {
    int n = 0;
    char *p = line;
    for (int i = 0; i < maxf; i++) fld[i] = 0;
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
                ucopy(u->pw, fld[6], 64);
                /* #185: an open account is the explicit legacy marker "0"
                 * (or a valid all-zero hex word).  A field that cannot be
                 * parsed as hex (e.g. "xyz") used to be cleared here and
                 * unlocked the account; keep it as a set-but-unknown
                 * password so login is rejected instead. */
                if (u->pw[0] != '$' &&
                    (u->pw[0] == 0 ||
                     (is_hex_word(u->pw) && parse_uint(u->pw, 16) == 0)))
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

int acl_unset(const char *path) {
    /* v2.24: deleting a file used to leave its ACL entry behind -- a
     * re-created file with the same name then INHERITED the stale
     * permission.  Drop the entry on delete instead. */
    if (!path || !*path) return -1;
    char key[FS_PATH_MAX];
    acl_key(path, key);
    for (int i = 0; i < ACL_MAX; i++) {
        if (acl_tab[i].used && useq(acl_tab[i].path, key)) {
            acl_tab[i].used = 0;
            acl_dirty = 1;
            return 0;
        }
    }
    return -1;
}

int acl_default(const char *path, int uid, int gid, int mode) {
    /* seed-time insert that never replaces an existing entry */
    for (int i = 0; i < ACL_MAX; i++)
        if (acl_tab[i].used && useq(acl_tab[i].path, path)) return 0;
    return acl_set(path, uid, gid, mode);
}

int acl_save(void) {
    static char buf[4096];
    const int cap = (int)sizeof(buf);
    int o = 0;
    for (int i = 0; i < ACL_MAX; i++) {
        if (!acl_tab[i].used) continue;
        acl_entry_t *a = &acl_tab[i];
        char num[12];
        buf_str(buf, &o, cap, a->path);
        buf_put(buf, &o, cap, ':');
        emit_uint(num, a->uid, 10);  buf_num(buf, &o, cap, num);
        buf_put(buf, &o, cap, ':');
        emit_uint(num, a->gid, 10);  buf_num(buf, &o, cap, num);
        buf_put(buf, &o, cap, ':');
        emit_uint(num, a->mode, 8);  buf_num(buf, &o, cap, num);
        buf_put(buf, &o, cap, '\n');
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

/* ---- v2.53 supplementary groups (persistent /etc/group) ---------------- */

static struct {
    char name[16];
    int  gid;
    int  nmem;
    int  mem_uid[GROUP_MEMBERS];
} ggroups[GROUP_MAX];
static int gn_groups;

int group_db_init(void) {
    gn_groups = 0;
    static char buf[2048];
    int n = fs_read_text("/etc/group", buf, sizeof(buf));
    if (n <= 0) return 0;                     /* no group db yet: empty table */
    buf[n] = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        int eol = (*nl == '\n');
        *nl = 0;
        char *fld[3];
        if (split_fields(line, fld, 3) >= 2 && fld[0][0] && gn_groups < GROUP_MAX) {
            int ngid = (int)parse_uint(fld[1], 10);
            /* #187: reject duplicate gids at load time.  Two records with
             * the same gid otherwise share one uid membership list through
             * user_in_group, so members bleed across group names. */
            int dup = 0;
            for (int j = 0; j < gn_groups; j++)
                if (ggroups[j].gid == ngid) { dup = 1; break; }
            if (!dup) {
            int slot = gn_groups++;
            ucopy(ggroups[slot].name, fld[0], 16);
            ggroups[slot].gid = ngid;
            ggroups[slot].nmem = 0;
            /* #163: a two-field line (`grp:100`, no member column) leaves
             * fld[2] == NULL; skip the member list instead of dereferencing
             * whatever stack garbage used to sit there. */
            char *m = fld[2];
            while (m && *m && ggroups[slot].nmem < GROUP_MEMBERS) {
                char *comma = m;
                while (*comma && *comma != ',') comma++;
                int eom = (*comma == ',');
                *comma = 0;
                user_rec_t *u = user_by_name(m);
                if (u) ggroups[slot].mem_uid[ggroups[slot].nmem++] = u->uid;
                m = eom ? comma + 1 : 0;
            }
            }
        }
        line = eol ? nl + 1 : 0;
    }
    return gn_groups;
}

int group_add(const char *name, const char *member) {
    if (!name || !name[0]) return -1;
    int slot = -1;
    for (int i = 0; i < gn_groups; i++)
        if (useq(ggroups[i].name, name)) { slot = i; break; }
    if (slot < 0) {
        if (gn_groups >= GROUP_MAX) return -2;  /* table full */
        slot = gn_groups++;
        ucopy(ggroups[slot].name, name, 16);
        ggroups[slot].gid = 100;
        for (int i = 0; i < gn_groups - 1; i++)
            if (ggroups[i].gid >= ggroups[slot].gid) ggroups[slot].gid = ggroups[i].gid + 1;
        ggroups[slot].nmem = 0;
    }
    if (member && member[0]) {
        user_rec_t *u = user_by_name(member);
        if (!u) return -3;                       /* unknown member */
        for (int i = 0; i < ggroups[slot].nmem; i++)
            if (ggroups[slot].mem_uid[i] == u->uid) return ggroups[slot].gid;
        if (ggroups[slot].nmem >= GROUP_MEMBERS) return -4;
        ggroups[slot].mem_uid[ggroups[slot].nmem++] = u->uid;
    }
    return ggroups[slot].gid;
}

/* #186: remove a group slot.  Used to roll back a just-created group when
 * member addition fails after the slot was created. */
void group_remove(const char *name) {
    for (int i = 0; i < gn_groups; i++) {
        if (!useq(ggroups[i].name, name)) continue;
        for (int j = i; j < gn_groups - 1; j++)
            ggroups[j] = ggroups[j + 1];
        gn_groups--;
        return;
    }
}

int user_in_group(int uid, int gid) {
    for (int i = 0; i < gn_groups; i++) {
        if (ggroups[i].gid != gid) continue;
        for (int m = 0; m < ggroups[i].nmem; m++)
            if (ggroups[i].mem_uid[m] == uid) return 1;
    }
    return 0;
}

int group_save(void) {
    static char buf[2048];
    const int cap = (int)sizeof(buf);
    int o = 0;
    for (int i = 0; i < gn_groups; i++) {
        char t[12];
        buf_str(buf, &o, cap, ggroups[i].name);
        buf_put(buf, &o, cap, ':');
        emit_uint(t, (uint32_t)ggroups[i].gid, 10); buf_num(buf, &o, cap, t);
        buf_put(buf, &o, cap, ':');
        /* #165: comma follows the PREVIOUSLY EMITTED member, not the loop
         * index.  When the first recorded member account was deleted
         * (user_by_uid -> NULL, skipped), an index-based comma still
         * prefixed the next name, leaving a malformed grp:100:,alice. */
        int emitted = 0;
        for (int m = 0; m < ggroups[i].nmem; m++) {
            user_rec_t *u = user_by_uid(ggroups[i].mem_uid[m]);
            if (!u) continue;
            if (emitted) buf_put(buf, &o, cap, ',');
            buf_str(buf, &o, cap, u->name);
            emitted = 1;
        }
        buf_put(buf, &o, cap, '\n');
    }
    buf[o] = 0;
    (void)fs_delete("/etc/group");
    if (fs_write_raw("/etc/group", buf, (uint32_t)o) != 0) return -1;
    return 0;
}

int group_info(int idx, char *name_out, int nlen, int *gid_out) {
    if (idx < 0 || idx >= gn_groups) return -1;
    ucopy(name_out, ggroups[idx].name, nlen);
    if (gid_out) *gid_out = ggroups[idx].gid;
    return ggroups[idx].nmem;
}

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
        /* v2.53: primary gid or any supplementary group of this uid */
        else if (gid == fgid || user_in_group(uid, fgid))
                                    have = (mode >> 3) & 7;   /* group */
        else                        have = mode & 7;          /* other */
        /* v2.53 fix (#157): the mode triads are UNIX-encoded (r=4, w=2,
         * x=1) while need/PERM_* use READ=1, WRITE=2, EXEC=4.  Masking them
         * together silently denied READ on any 4xx triad and allowed EXEC
         * on 4xx.  Translate the triad into PERM_* first. */
        have = ((have & 4) ? PERM_READ  : 0)
             | ((have & 2) ? PERM_WRITE : 0)
             | ((have & 1) ? PERM_EXEC  : 0);
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
