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

uint32_t user_hash_pw(const char *pw) {
    /* djb2 -- deliberately NOT cryptographic: this is a teaching kernel
     * with no crypto primitives.  It only prevents shoulder-surfing. */
    uint32_t h = 5381;
    for (; pw && *pw; pw++) h = h * 33u + (uint8_t)*pw;
    return h;
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

int user_check_pw(user_rec_t *u, const char *pw) {
    if (!u) return -1;
    if (u->passhash == 0) return 0;            /* open account */
    return user_hash_pw(pw) == u->passhash ? 0 : -1;
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
    u->passhash = 0;
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
    u->passhash = user_hash_pw(pw);
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
        /* name:uid:gid:home:shell:caps:passhash */
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
        emit_uint(num, u->passhash, 16); for (int j = 0; num[j]; j++) buf[o++] = num[j];
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
                u->passhash = parse_uint(fld[6], 16);
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

int acl_lookup(const char *path, int *uid, int *gid, int *mode) {
    if (!path) return -1;
    for (int i = 0; i < ACL_MAX; i++) {
        if (acl_tab[i].used && useq(acl_tab[i].path, path)) {
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
    acl_entry_t *slot = 0;
    for (int i = 0; i < ACL_MAX; i++) {
        if (acl_tab[i].used && useq(acl_tab[i].path, path)) { slot = &acl_tab[i]; break; }
        if (!slot && !acl_tab[i].used) slot = &acl_tab[i];
    }
    if (!slot) return -4;
    ucopy(slot->path, path, 160);
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
    int fuid, fgid, mode;
    if (acl_lookup(path, &fuid, &fgid, &mode) == 0) {
        int have;
        if (uid == fuid)            have = (mode >> 6) & 7;   /* owner */
        else if (gid == fgid)       have = (mode >> 3) & 7;   /* group */
        else                        have = mode & 7;          /* other */
        return (have & need) == need ? 0 : -13;
    }
    /* no ACL entry: read/exec default allowed (0644/0755 semantics);
     * writes go through the world-writable scratch whitelist */
    if (need & PERM_WRITE) {
        int scratch = (uprefix(path, "/home/") || useq(path, "/home") ||
                       uprefix(path, "/tmp/") || useq(path, "/tmp"));
        if (!scratch) return -13;
    }
    return 0;
}
