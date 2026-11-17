#include "fs.h"
#include "fat.h"
#include "part.h"
#include "vga.h"
#include "hw.h"
static void fs_dbg2(const char *tag, uint32_t v) {
    (void)tag; (void)v;  /* v2.5 debug: no-op */
}

/* CatOS filesystem: thin compatibility layer over the FAT32 driver.
 * All paths are handled as absolute; relative paths are joined to the
 * current working directory and normalised (".", ".."). */

static int      fs_ready = 0;
static uint32_t g_cwd;                  /* FAT cluster of current dir */
static char     cwd_path[FS_PATH_MAX];  /* absolute path, e.g. "/etc" */
static fs_entry_t g_ent;                /* single open-file buffer */

/* P2: mount table.  Volume slot 0 is the implicit root volume (letter
 * 'A'); g_mnt[] holds the manually mounted extra partitions. */
#define VFS_MAX_MOUNTS (FAT_VOL_MAX - 1)
struct vfs_mount {
    uint8_t used;
    char    mnt[FS_PATH_MAX];   /* absolute mount point, e.g. "/docs" */
    int     vol;                /* fat volume slot */
    char    letter;             /* drive suffix, e.g. 'B' */
    char    dev[16];            /* device name, e.g. "hd0p2" */
};
static struct vfs_mount g_mnt[VFS_MAX_MOUNTS];
static const char ROOT_LETTER = 'A';

/* ---------- tiny string helpers ---------- */
static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int scmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void scpy(char *d, const char *s) { while ((*d++ = *s++)); }
static void scat(char *d, const char *s) {
    while (*d) d++;
    scpy(d, s);
}
static void memz(void *d, int n) { unsigned char *p = (unsigned char *)d; while (n--) *p++ = 0; }
static void memcpy_(void *d, const void *s, int n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}

/* ---------- number printing (for fs_list) ---------- */
static void print_int(int v) {
    char b[12]; int i = 0;
    if (v == 0) { b[i++] = '0'; }
    if (v < 0) { v = -v; }
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { i--; vga_putc(b[i]); }
}

/* ---------- path helpers ---------- */

/* v2.18: components may exceed 8.3 (VFAT LFN entries carry the long
 * name; the fat layer generates a unique BODY~N.EXT short alias).
 * name_too_long83()/path_has_long_names() are gone. */
/* normalise "/a/./b/../c" -> "/a/c"; result always starts with '/' */
static void path_norm(const char *in, char *out) {
    char stack[FS_PATH_MAX / 2][FS_NAME_MAX];
    int sp = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        int l = (int)(p - s);
        if (l == 1 && s[0] == '.') continue;
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (sp > 0) sp--;
            continue;
        }
        if (l >= FS_NAME_MAX) l = FS_NAME_MAX - 1;
        if (sp < (int)(FS_PATH_MAX / 2)) {
            int i;
            for (i = 0; i < l; i++) stack[sp][i] = s[i];
            stack[sp][l] = 0;
            sp++;
        }
    }
    int o = 0;
    for (int i = 0; i < sp && o < FS_PATH_MAX - 2; i++) {
        out[o++] = '/';
        const char *n = stack[i];
        while (*n && o < FS_PATH_MAX - 1) out[o++] = *n++;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
}

/* return the absolute path for a (possibly relative) input path */
static void abs_path(const char *p, char *out) {
    char tmp[FS_PATH_MAX * 2];
    if (!p || !*p) {
        scpy(tmp, cwd_path);
    } else if (p[0] == '/') {
        scpy(tmp, p);
    } else {
        scpy(tmp, cwd_path);
        int l = slen(tmp);
        if (l > 0 && tmp[l - 1] != '/') scat(tmp, "/");
        scat(tmp, p);
    }
    path_norm(tmp, out);
}

/* P2: find the mount entry whose mount point owns `abs` (longest prefix
 * match).  Returns the g_mnt index, or -1 when the path lives on the
 * root volume. */
static int mount_for(const char *abs) {
    int best = -1, bestlen = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mnt[i].used) continue;
        const char *m = g_mnt[i].mnt;
        int ml = slen(m);
        int match = 0;
        if (scmp(abs, m) == 0) match = 1;
        else if (abs[ml] == '/') {
            int k = 0;
            for (; k < ml; k++) if (abs[k] != m[k]) break;
            if (k == ml) match = 1;
        }
        if (match && ml > bestlen) { best = i; bestlen = ml; }
    }
    return best;
}

/* does `abs` sit inside (or equal) mount point `m`? */
static int path_under(const char *abs, const char *m) {
    int ml = slen(m);
    if (scmp(abs, m) == 0) return 1;
    if (abs[ml] != '/') return 0;
    for (int k = 0; k < ml; k++) if (abs[k] != m[k]) return 0;
    return 1;
}

/* resolve an absolute path (must start with '/'); returns FAT cluster of
 * the final component, or 0 on error.  Walks through mount points: once a
 * prefix matches a mount entry, resolution continues inside that volume. */
static uint32_t resolve_path(const char *abs, fat_dirent_t *out) {
    int mi = mount_for(abs);
    const char *rel = abs;
    if (mi >= 0) {
        fat_activate(g_mnt[mi].vol);
        int ml = slen(g_mnt[mi].mnt);
        rel = abs + ml;
        if (*rel == '/') rel++;
    } else {
        fat_activate(0);
    }
    uint32_t cur = fat_root();
    int filled = 0;
    const char *p = rel;
    if (*p == '/') p++;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        int l = (int)(p - s);
        char name[FS_NAME_MAX];
        int i;
        if (l >= FS_NAME_MAX) l = FS_NAME_MAX - 1;
        for (i = 0; i < l; i++) name[i] = s[i];
        name[i] = 0;

        uint8_t n83[11];
        fat_dirent_t e;
        if (fat_lookup_name(cur, name, &e, n83) != 0) return 0;
        cur = e.cluster;
        if (out) { *out = e; filled = 1; }
    }
    if (out && !filled) {           /* root path "/" has no components */
        out->attr = FAT_ATTR_DIR;
        out->cluster = fat_root();
        out->size = 0;
    }
    return cur;
}

/* ---------- lifecycle ---------- */
void fs_init(void) {
    btime_mark("F0 fs_init enter");
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) g_mnt[i].used = 0;
    btime_mark("F1 before fat_probe");
    fs_ready = (fat_probe() == 0);
    btime_mark("F2 fat_probe done");
    if (fs_ready) part_scan();     /* enumerate MBR partitions for mount */
    btime_mark("F3 part_scan done");
    g_cwd = fat_root();
    scpy(cwd_path, "/");
    memz(&g_ent, sizeof(g_ent));
}

int fs_fs_ready(void) { return fs_ready; }

int fs_format(void) {
    if (fat_mkfs() != 0) return -1;        /* v2.32: no IDE disk */
    fs_init();
    return 0;
}

uint16_t fs_cwd_index(void) { return (uint16_t)g_cwd; }

/* ---------- chdir / mkdir / touch ---------- */
int fs_chdir(const char *path) {
    if (!fs_ready) return FS_ERR_NOENT;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus) return FS_ERR_NOENT;
    if (!(e.attr & FAT_ATTR_DIR)) return FS_ERR_NOTDIR;
    g_cwd = clus;
    scpy(cwd_path, abs);
    return FS_OK;
}

/* create a leaf (dir or file) at the end of `path` */
static int create_path(const char *path, uint8_t type) {
    if (!fs_ready) return FS_ERR_NOENT;
    if (!path || !*path) return FS_ERR_INVAL;

    char abs[FS_PATH_MAX];
    abs_path(path, abs);

    /* split parent path + leaf name */
    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) return FS_ERR_INVAL;

    char ppath[FS_PATH_MAX];
    int plen = (int)(leaf - abs);
    if (plen >= (int)sizeof(ppath)) return FS_ERR_INVAL;
    int i;
    for (i = 0; i < plen; i++) ppath[i] = abs[i];
    ppath[plen] = 0;

    fat_dirent_t pe;
    uint32_t pclus = resolve_path(ppath, &pe);
    if (!pclus) return FS_ERR_NOENT;
    if (!(pe.attr & FAT_ATTR_DIR)) return FS_ERR_NOTDIR;

    /* validate leaf name */
    if (!*leaf || !scmp(leaf, ".") || !scmp(leaf, "..")) return FS_ERR_INVAL;
    if (slen(leaf) >= FS_NAME_MAX) return FS_ERR_INVAL;
    /* v2.48: FAT reserved chars / control codes / trailing dot-space */
    if (fat_name_illegal(leaf)) return FS_ERR_INVAL;

    fat_dirent_t tmp;
    uint8_t rn[11];
    if (fat_lookup_name(pclus, leaf, &tmp, rn) == 0) return FS_ERR_EXIST;

    int rc = (type == FS_DIR) ? fat_mkdir_name(pclus, leaf)
                              : fat_mkfile_name(pclus, leaf);
    return rc == 0 ? FS_OK : FS_ERR_FULL;
}

int fs_mkdir(const char *path) {
    return create_path(path, FS_DIR);
}

int fs_touch(const char *path) {
    if (!fs_ready) return FS_ERR_NOENT;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (clus) {
        return (e.attr & FAT_ATTR_DIR) ? FS_ERR_NOTDIR : FS_OK;
    }
    return create_path(path, FS_FILE);
}

/* ---------- list / pwd / enum ---------- */

/* Enumerate regular-file entries of a directory.  Names are display
 * names: the VFAT long name when present, else 8.3 lower-case.
 * Returns the number of names found, or -1 if the path is not an
 * existing directory (e.g. the volume is not mounted). */
struct enumf_ud { char (*out)[FS_NAME_MAX]; int maxn; int count; };
static void enumf_all_cb(const char *name, uint8_t attr,
                         uint32_t clus, uint32_t size, void *ud) {
    struct enumf_ud *u = ud;
    (void)clus; (void)size;
    if (u->count < u->maxn) {
        int k;
        for (k = 0; name[k] && k < FS_NAME_MAX - 2; k++) u->out[u->count][k] = name[k];
        if (attr & FAT_ATTR_DIR) { u->out[u->count][k++] = '/'; }
        u->out[u->count][k] = 0;
    }
    u->count++;
}
static void enumf_cb(const char *name, uint8_t attr,
                     uint32_t clus, uint32_t size, void *ud) {
    struct enumf_ud *u = (struct enumf_ud *)ud;
    if (attr & FAT_ATTR_DIR) return;
    if (u->count < u->maxn) {
        int k;
        for (k = 0; name[k] && k < FS_NAME_MAX - 1; k++) u->out[u->count][k] = name[k];
        u->out[u->count][k] = 0;
    }
    u->count++;
}

/* v2.28: like fs_enum_files but includes directories; a directory name
 * comes back with a trailing '/' so completion can offer both kinds. */
int fs_enum_entries(const char *path, char out[][FS_NAME_MAX], int maxn) {
    if (!fs_ready || !path || maxn < 1) return -1;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus || !(e.attr & FAT_ATTR_DIR)) return -1;

    struct enumf_ud u = { out, maxn, 0 };
    fat_list_dir(clus, enumf_all_cb, &u);
    return u.count;
}

int fs_enum_files(const char *path, char out[][FS_NAME_MAX], int maxn) {
    if (!fs_ready || !path || maxn < 1) return -1;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus || !(e.attr & FAT_ATTR_DIR)) return -1;

    struct enumf_ud u = { out, maxn, 0 };
    fat_list_dir(clus, enumf_cb, &u);
    return u.count;
}

/* P2: if `name` inside `parent_abs` is a mount point, fill `suf` with
 * "(X:)" (drive letter) and return 1; otherwise leave it empty. */
static int mount_suffix(const char *parent_abs, const char *name, char *suf) {
    char full[FS_PATH_MAX];
    if (!scmp(parent_abs, "/")) { scpy(full, "/"); scat(full, name); }
    else { scpy(full, parent_abs); scat(full, "/"); scat(full, name); }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mnt[i].used && !scmp(g_mnt[i].mnt, full)) {
            suf[0] = '('; suf[1] = g_mnt[i].letter; suf[2] = ':';
            suf[3] = ')'; suf[4] = 0;
            return 1;
        }
    }
    suf[0] = 0;
    return 0;
}

/* v2.45 aesthetic: collect entries first, sort dirs-first then
 * case-insensitive name, then print -- matches the ordered column look
 * of modern ls/eza instead of raw FAT directory order. */
#define LS_MAX 96
struct ls_ent { char name[FS_NAME_MAX]; uint8_t attr; uint32_t size; };
static struct ls_ent ls_buf[LS_MAX];
static int ls_n;

static char ls_lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ls_icmp(const char *a, const char *b) {
    while (*a && *b) {
        char x = ls_lc(*a), y = ls_lc(*b);
        if (x != y) return x - y;
        a++; b++;
    }
    return ls_lc(*a) - ls_lc(*b);
}
static void ls_collect(const char *name, uint8_t attr,
                        uint32_t clus, uint32_t size, void *ud) {
    (void)clus; (void)ud;
    if (ls_n >= LS_MAX) return;
    scpy(ls_buf[ls_n].name, name);
    ls_buf[ls_n].attr = attr;
    ls_buf[ls_n].size = size;
    ls_n++;
}
static void ls_sort(void) {
    for (int i = 1; i < ls_n; i++) {
        struct ls_ent k = ls_buf[i];
        int j = i - 1;
        int kdir = (k.attr & FAT_ATTR_DIR) != 0;
        while (j >= 0) {
            int jdir = (ls_buf[j].attr & FAT_ATTR_DIR) != 0;
            int swap = (jdir == kdir) ? ls_icmp(ls_buf[j].name, k.name) > 0
                                      : (!jdir && kdir);
            if (!swap) break;
            ls_buf[j + 1] = ls_buf[j];
            j--;
        }
        ls_buf[j + 1] = k;
    }
}
static int ls_fmt_size(uint32_t size, char *out) {
    char b[12]; int n = 0;
    if (size == 0) out[n++] = '0';
    else while (size > 0) { b[n++] = (char)('0' + size % 10); size /= 10; }
    int i = 0;
    while (n > 0) out[i++] = b[--n];
    out[i] = 0;
    return i;
}
static void ls_print_one(const struct ls_ent *e, const char *parent,
                         int longfmt) {
    char suf[8];
    int isdir = (e->attr & FAT_ATTR_DIR) != 0;
    int ismount = isdir && mount_suffix(parent, e->name, suf);
    char sz[12];
    int sn = ls_fmt_size(e->size, sz);
    if (longfmt) {
        vga_write(isdir ? "drwxr-xr-x  " : "-rw-r--r--  ");
        for (int p = sn; p < 8; p++) vga_putc(' ');
        vga_write(sz);
        vga_write("  ");
        vga_set_fg(isdir ? VGA_LIGHT_CYAN : VGA_LIGHT_GREY);
        vga_write(e->name);
        if (ismount) vga_write(suf);
        vga_set_fg(VGA_WHITE);
        vga_write((isdir && !ismount) ? "/\n" : "\n");
        return;
    }
    if (isdir) {
        vga_set_fg(VGA_DARK_GREY);
        vga_write("[DIR] ");
        vga_set_fg(VGA_LIGHT_CYAN);
        vga_write(e->name);
        if (ismount) vga_write(suf);
        else vga_write("/");
        vga_set_fg(VGA_WHITE);
        vga_write("\n");
    } else {
        vga_set_fg(VGA_DARK_GREY);
        vga_write("[FILE] ");
        vga_set_fg(VGA_LIGHT_GREY);
        vga_write(e->name);
        vga_set_fg(VGA_DARK_GREY);
        vga_write("  (");
        for (int p = sn; p < 8; p++) vga_putc(' ');
        vga_write(sz);
        vga_write(" bytes)\n");
        vga_set_fg(VGA_WHITE);
    }
}

void fs_list(const char *path) {
    if (!fs_ready) {
        vga_write("catos: no FAT32 volume. Run 'format' first.\n");
        return;
    }
    int longfmt = 0;
    if (path && path[0] == '-' && path[1] == 'l') {   /* v2.34: ls -l */
        longfmt = 1;
        path += 2;
        while (*path == ' ') path++;
        if (!*path) path = 0;
    }
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus) { vga_write("catos: dir: no such directory\n"); return; }
    if (!(e.attr & FAT_ATTR_DIR) && !longfmt) { vga_write("catos: dir: not a directory\n"); return; }

    if (longfmt && !(e.attr & FAT_ATTR_DIR)) {
        /* v2.34: ls -l <file> -- show just that entry */
        const char *nm = abs;
        for (const char *p = abs; *p; p++) if (*p == '/') nm = p + 1;
        struct ls_ent one;
        scpy(one.name, nm); one.attr = e.attr; one.size = e.size;
        vga_write("perms      size     name\n");
        ls_print_one(&one, abs, 1);
        return;
    }
    ls_n = 0;
    fat_list_dir(clus, ls_collect, 0);
    ls_sort();
    for (int i = 0; i < ls_n; i++) ls_print_one(&ls_buf[i], abs, longfmt);
    vga_set_fg(VGA_WHITE);
    vga_write("--- ");
    print_int(ls_n);
    vga_write(ls_n == 1 ? " entry ---\n" : " entries ---\n");
}

void fs_pwd(char *out, int outlen) {
    if (!out || outlen < 1) return;
    int n = slen(cwd_path);
    if (n >= outlen) n = outlen - 1;
    int i;
    for (i = 0; i < n; i++) out[i] = cwd_path[i];
    out[n] = 0;
}

/* See fs.h: absolute + dot/dotdot/slash normalisation (reusing abs_path)
 * followed by ASCII lowercase folding.  Used by the ACL layer so that
 * every path variant FAT resolves to one file maps to exactly one key.
 * NOTE: this is deliberately ASCII-only; FAT names here are 8.3 ASCII. */
void fs_canon_path(const char *in, char *out, int outlen) {
    char abs[FS_PATH_MAX];
    if (!out || outlen < 1) return;
    abs_path(in, abs);
    int i = 0;
    for (; abs[i] && i < outlen - 1; i++) {
        char c = abs[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    out[i] = 0;
}

/* ---------- editor helpers ---------- */
fs_entry_t *fs_open_file(const char *path) {
    if (!fs_ready) return 0;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);

    /* split parent + leaf so we can remember the directory entry */
    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;

    char ppath[FS_PATH_MAX];
    int plen = (int)(leaf - abs);
    if (plen >= (int)sizeof(ppath)) return 0;
    int i;
    for (i = 0; i < plen; i++) ppath[i] = abs[i];
    ppath[plen] = 0;

    fat_dirent_t pe;
    uint32_t pclus = resolve_path(ppath, &pe);
    if (!pclus) return 0;

    uint8_t n83[11];
    fat_dirent_t e;
    if (fat_lookup_name(pclus, leaf, &e, n83) != 0) return 0;
    if (e.attr & FAT_ATTR_DIR) return 0;

    memz(&g_ent, sizeof(g_ent));
    g_ent.type = FS_FILE;
    g_ent.used = 1;
    g_ent.cluster = e.cluster;
    g_ent.parent_cluster = pclus;
    memcpy_(g_ent.name83, n83, 11);
    i = 0;
    while (leaf[i] && i < FS_NAME_MAX - 1) { g_ent.name[i] = leaf[i]; i++; }
    g_ent.name[i] = 0;

    uint32_t sz = e.size;
    if (sz >= FS_DATA_MAX) sz = FS_DATA_MAX - 1;   /* room for the NUL */
    g_ent.size = (uint16_t)sz;
    char tmp[FS_DATA_MAX + 1];
    if (fat_read_file(e.cluster, sz, tmp) != 0) return 0;
    memcpy_(g_ent.data, tmp, sz);
    g_ent.data[sz] = 0;
    return &g_ent;
}

fs_entry_t *fs_create_file(const char *path) {
    if (fs_touch(path) < 0) return 0;
    return fs_open_file(path);
}

int fs_file_size(const fs_entry_t *f) { return f ? (int)f->size : 0; }

int fs_append_line(fs_entry_t *f, const char *line) {
    if (!f || !line) return -1;
    int l = slen(line);
    if (f->size + l + 2 > FS_DATA_MAX) return -1;  /* text + '\n' + NUL */
    int i;
    for (i = 0; i < l; i++) f->data[f->size + i] = line[i];
    f->size = (uint16_t)(f->size + l);
    f->data[f->size++] = '\n';
    f->data[f->size] = 0;
    return 0;
}

int fs_line_count(const fs_entry_t *f) {
    if (!f) return 0;
    int n = 0;
    for (int i = 0; i < f->size; i++)
        if (f->data[i] == '\n') n++;
    if (f->size && f->data[f->size - 1] != '\n') n++;
    return n;
}

int fs_get_line(const fs_entry_t *f, int idx, char *out, int outlen) {
    if (!f || !out || outlen < 1) return -1;
    int line = 0, o = 0;
    for (int i = 0; i < f->size; i++) {
        char c = f->data[i];
        if (c == '\n') {
            if (line == idx) { out[o] = 0; return 0; }
            line++;
        } else if (line == idx) {
            if (o < outlen - 1) out[o++] = c;
        }
    }
    if (line == idx) { out[o] = 0; return 0; }
    return -1;
}

int fs_delete_line(fs_entry_t *f, int idx) {
    if (!f) return -1;
    int line = 0, start = -1, end = -1;
    for (int i = 0; i <= f->size; i++) {
        if (i == f->size || f->data[i] == '\n') {
            if (line == idx) { start = (end < 0) ? 0 : end + 1; end = i; break; }
            end = i;
            line++;
        }
    }
    if (start < 0) return -1;
    int rest = f->size - end - 1;
    if (rest < 0) rest = 0;
    for (int i = 0; i < rest; i++) f->data[start + i] = f->data[end + 1 + i];
    f->size = (uint16_t)(start + rest);
    f->data[f->size] = 0;
    return 0;
}

void fs_clear_file(fs_entry_t *f) {
    if (!f) return;
    f->size = 0;
    f->data[0] = 0;
}

int fs_save(fs_entry_t *f) {
    if (!fs_ready || !f) return -1;
    if (fat_write_file(f->cluster, f->data, f->size) != 0) return -1;
    fat_update_entry(f->parent_cluster, f->name83, f->cluster, f->size);
    return 0;
}

/* ---------- raw access (CE loader / CE system calls) ---------- */

int fs_resolve_file(const char *path, uint32_t *cluster, uint32_t *size) {
    if (!fs_ready || !path || !*path) return -1;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);

    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) { fs_dbg2("[F2]",0); return -1; }

    char ppath[FS_PATH_MAX];
    int plen = (int)(leaf - abs);
    if (plen >= (int)sizeof(ppath)) return -1;
    int i;
    for (i = 0; i < plen; i++) ppath[i] = abs[i];
    ppath[plen] = 0;

    fat_dirent_t pe;
    uint32_t pclus = resolve_path(ppath, &pe);
    if (!pclus) return -1;

    fat_dirent_t e;
    if (fat_lookup_name(pclus, leaf, &e, 0) != 0) return -1;
    if (e.attr & FAT_ATTR_DIR) return -1;
    if (cluster) *cluster = e.cluster;
    if (size)    *size = e.size;
    return 0;
}

struct fe_ud { fs_foreach_cb cb; void *ud; };
static void fe_cb(const char *name, uint8_t attr,
                  uint32_t clus, uint32_t size, void *ud) {
    struct fe_ud *u = (struct fe_ud *)ud;
    u->cb((attr & FAT_ATTR_DIR) ? 1 : 0, name, size, u->ud);
}

int fs_foreach(const char *path, fs_foreach_cb cb, void *ud) {
    if (!fs_ready || !cb) return -1;
    char abs[FS_PATH_MAX];
    abs_path(path ? path : "/", abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus) return -1;
    if (!(e.attr & FAT_ATTR_DIR)) { fs_dbg2("[F5]",0); return -1; }

    struct fe_ud fe = { cb, ud };
    fat_list_dir(clus, fe_cb, &fe);
    return 0;
}

int fs_read_text(const char *path, char *buf, int max) {
    if (!fs_ready || !buf || max < 1) return -1;
    uint32_t clus = 0, size = 0;
    if (fs_resolve_file(path, &clus, &size) != 0) return -1;
    uint32_t n = size;
    if (n >= (uint32_t)max) n = (uint32_t)max - 1;
    if (fat_read_file(clus, n, buf) != 0) return -1;
    return (int)n;
}

int fs_write_text(const char *path, const char *data, uint32_t size) {
    if (!fs_ready || !path || !*path || !data) { fs_dbg2("[F0]",0); return -1; }
    if (size >= FS_DATA_MAX) return -1;          /* room for the NUL */
    fs_entry_t *f = fs_create_file(path);
    if (!f) return -1;
    int i;
    for (i = 0; i < (int)size; i++) f->data[i] = data[i];
    f->size = (uint16_t)size;
    f->data[size] = 0;
    return fs_save(f);
}

/* raw write of arbitrary-size data (no FS_DATA_MAX limit); skips if the
 * file already exists.  Used to install built-in .ce apps and docs. */
int fs_write_raw(const char *path, const char *data, uint32_t size) {
    if (!fs_ready || !path || !*path || !data) { fs_dbg2("[F0]",0); return -1; }
    char abs[FS_PATH_MAX];
    abs_path(path, abs);

    /* split parent path + leaf name */
    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) { fs_dbg2("[F2]",0); return -1; }

    char parent[FS_PATH_MAX];
    int plen = (int)(leaf - abs);
    if (plen <= 0) { fs_dbg2("[F3]",0); return -1; }
    if (plen == 1) scpy(parent, "/");
    else {
        for (int i = 0; i < plen - 1; i++) parent[i] = abs[i];
        parent[plen - 1] = 0;
    }

    uint8_t n83[11];
    fat_dirent_t e;
    uint32_t pclus = resolve_path(parent, &e);
    if (!pclus) { fs_dbg2("[F4]",0); return -1; }
    if (!(e.attr & FAT_ATTR_DIR)) { fs_dbg2("[F5]",0); return -1; }
    if (fat_lookup_name(pclus, leaf, &e, n83) == 0) return 0;  /* already installed */

    if (fat_mkfile_name(pclus, leaf) != 0) return -1;
    if (fat_lookup_name(pclus, leaf, &e, n83) != 0) return -1;
    if (fat_write_file(e.cluster, data, size) != 0) return -1;
    return fat_update_entry(pclus, n83, e.cluster, size);
}

/* ---- v2.5: offset read / mode write / delete (big files) ---------------- */

int fs_read_at(const char *path, char *buf, int offset, int max) {
    /* read min(max, size-offset) bytes starting at `offset`; >=0 bytes,
     * 0 = offset past EOF, -1 = error.  Walks the FAT chain to the right
     * cluster so files larger than any single buffer can be read in
     * chunks. */
    if (!fs_ready || !buf || max < 1) return -1;
    uint32_t clus = 0, size = 0;
    if (fs_resolve_file(path, &clus, &size) != 0) return -1;
    if (offset < 0) offset = 0;
    if ((uint32_t)offset >= size) return 0;
    uint32_t n = size - (uint32_t)offset;
    if (n > (uint32_t)max) n = (uint32_t)max;
    uint32_t csz = fat_cluster_size();
    uint32_t skip = (uint32_t)offset / csz;
    uint32_t c = clus;
    for (uint32_t i = 0; i < skip; i++) {
        uint32_t nxt = fat_get(c);
        if (nxt < 2 || nxt >= 0x0FFFFFF0) return -1;
        c = nxt;
    }
    uint32_t within = (uint32_t)offset % csz;
    uint32_t got = 0;
    static uint8_t rbuf[FAT_CLUSTER_SZ_MAX];
    while (c >= 2 && c < 0x0FFFFFF0 && got < n) {
        if (fat_read_cluster(c, rbuf) != 0) return -1;
        uint32_t take = csz - within; if (take > n - got) take = n - got;
        for (uint32_t i = 0; i < take; i++) buf[got + i] = (char)rbuf[within + i];
        got += take;
        within = 0;
        uint32_t nxt = fat_get(c);
        if (nxt == FAT_EOC || nxt < 2) break;
        c = nxt;
    }
    return (int)got;
}


int fs_write_raw_mode(const char *path, const char *data, uint32_t size, int mode) {
    /* v2.5 big-file write.  mode 0 = truncate-replace, 1 = append; the
     * file is created when absent.  Returns 0 ok / negative error. */
    if (!fs_ready || !path || !*path || !data) { fs_dbg2("[F0]",0); return -1; }
    if (mode != 0 && mode != 1) { fs_dbg2("[F1]",0); return -1; }
    char abs[FS_PATH_MAX];
    abs_path(path, abs);

    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) { fs_dbg2("[F2]",0); return -1; }

    char parent[FS_PATH_MAX];
    int plen = (int)(leaf - abs);
    if (plen <= 0) { fs_dbg2("[F3]",0); return -1; }
    if (plen == 1) scpy(parent, "/");
    else {
        for (int i = 0; i < plen - 1; i++) parent[i] = abs[i];
        parent[plen - 1] = 0;
    }

    uint8_t n83[11];
    fat_dirent_t e;
    uint32_t pclus = resolve_path(parent, &e);
    if (!pclus) { fs_dbg2("[F4]",0); return -1; }
    if (!(e.attr & FAT_ATTR_DIR)) { fs_dbg2("[F5]",0); return -1; }
    if (fat_lookup_name(pclus, leaf, &e, n83) != 0) {
        if (fat_mkfile_name(pclus, leaf) != 0) { fs_dbg2("[F6]",0); return -1; }
        if (fat_lookup_name(pclus, leaf, &e, n83) != 0) { fs_dbg2("[F7]",0); return -1; }
    }
    fs_dbg2("[F8 c=", e.cluster); fs_dbg2("F9 sz=", e.size);
    if (mode == 1) {
        if (fat_append_file(e.cluster, e.size, data, size) != 0) { fs_dbg2("[F10]",e.cluster); return -1; }
    } else {
        if (fat_write_file(e.cluster, data, size) != 0) { fs_dbg2("[F11]",e.cluster); return -1; }
    }
    uint32_t new_size = (mode == 1) ? e.size + size : size;
    int ur = fat_update_entry(pclus, n83, e.cluster, new_size);
    fs_dbg2("[F12 u=", ur);
    return ur;
}


int fs_is_dir(const char *path) {
    if (!fs_ready || !path || !*path) return 0;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    return (clus && (e.attr & FAT_ATTR_DIR)) ? 1 : 0;
}

/* v2.24: remove an EMPTY directory.  The recursive emptier lives in the
 * shell (rm -r); the kernel API stays minimal and refuses anything that
 * still holds entries (besides the always-present "." / ".."). */
struct rmdir_ud { int count; };
static void rmdir_cb(const char *name, uint8_t attr,
                     uint32_t clus, uint32_t size, void *ud) {
    (void)attr; (void)clus; (void)size;
    struct rmdir_ud *u = (struct rmdir_ud *)ud;
    /* v2.50: count real entries only; skip the "." / ".." dots.  The old
     * condition required the name to be BOTH "." AND ".." at once, so the
     * count stayed 0 and non-empty directories passed the emptiness check
     * as if empty. */
    if (scmp(name, ".") != 0 && scmp(name, "..") != 0) u->count++;
}

static int fs_rmdir_impl(const char *path, int strict) {
    if (!fs_ready || !path || !*path) return -1;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) return -2;                    /* root: never deletable */
    char parent[FS_PATH_MAX];
    int plen = (int)(leaf - abs);
    if (plen == 1) scpy(parent, "/");
    else {
        for (int i = 0; i < plen - 1; i++) parent[i] = abs[i];
        parent[plen - 1] = 0;
    }
    fat_dirent_t e;
    uint32_t pclus = resolve_path(parent, &e);
    if (!pclus || !(e.attr & FAT_ATTR_DIR)) return -1;
    uint8_t n83[11];
    if (fat_lookup_name(pclus, leaf, &e, n83) != 0) return -1;
    if (!(e.attr & FAT_ATTR_DIR)) return -2;
    struct rmdir_ud u = { 0 };
    fat_list_dir(e.cluster, rmdir_cb, &u);
    if (strict && u.count) return -3;         /* still has entries */
    fat_free_chain(e.cluster);
    return fat_delete_name(pclus, leaf);
}

/* strict: refuse a non-empty directory (used by the `rmdir` shell command,
 * POSIX rmdir(2) semantics). */
int fs_rmdir(const char *path) { return fs_rmdir_impl(path, 1); }

/* v2.50: force variant for the shell's own recursive `rm -r` emptier.  The
 * caller has already walked and deleted every descendant post-order, so the
 * directory is expected empty; this skips the re-check.  The latent nested
 * delete-name reentrancy in the fat layer is tracked separately in the
 * kernel issue list; keeping this path identical to the pre-v2.50 behavior
 * avoids changing rm -r semantics in this batch. */
int fs_rmdir_force(const char *path) { return fs_rmdir_impl(path, 0); }

int fs_delete(const char *path) {
    /* remove a file: free its cluster chain and mark the directory entry
     * deleted.  Directories are not deletable through this API (v2.5). */
    if (!fs_ready || !path || !*path) return -1;
    char abs[FS_PATH_MAX];
    abs_path(path, abs);

    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) { fs_dbg2("[F2]",0); return -1; }

    char parent[FS_PATH_MAX];
    int plen = (int)(leaf - abs);
    if (plen <= 0) { fs_dbg2("[F3]",0); return -1; }
    if (plen == 1) scpy(parent, "/");
    else {
        for (int i = 0; i < plen - 1; i++) parent[i] = abs[i];
        parent[plen - 1] = 0;
    }

    uint8_t n83[11];
    fat_dirent_t e;
    uint32_t pclus = resolve_path(parent, &e);
    if (!pclus) { fs_dbg2("[F4]",0); return -1; }
    if (fat_lookup_name(pclus, leaf, &e, n83) != 0) return -1;
    if (e.attr & FAT_ATTR_DIR) return -1;
    fat_free_chain(e.cluster);
    return fat_delete_name(pclus, leaf);
}

/* ---------- P2: mount table (Linux-style bind of a partition) ---------- */

struct empty_ud { int count; };
static void empty_cb(const char *name, uint8_t attr,
                     uint32_t clus, uint32_t size, void *ud) {
    (void)attr; (void)clus; (void)size;
    struct empty_ud *u = (struct empty_ud *)ud;
    if (scmp(name, ".") && scmp(name, "..")) u->count++;
}

int fs_mount(const char *spec, const char *dir) {
    if (!fs_ready) return FS_ERR_NOENT;
    if (!spec || !*spec || !dir || !*dir) return FS_ERR_INVAL;

    int pi = part_lookup(spec);
    if (pi < 0) return FS_ERR_INVAL;
    const part_t *p = part_get(pi);
    if (p->extended) return -10;  /* EBR chains unsupported */
    if (!p->fat32)  return -10;

    char abs[FS_PATH_MAX];
    abs_path(dir, abs);
    if (!scmp(abs, "/")) return FS_ERR_INVAL;    /* root is never a mountpoint */

    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus) return FS_ERR_NOENT;
    if (!(e.attr & FAT_ATTR_DIR)) return FS_ERR_NOTDIR;
    if (mount_for(abs) >= 0) return FS_ERR_EXIST;

    struct empty_ud u = { 0 };
    fat_list_dir(clus, empty_cb, &u);
    if (u.count) return -13;                      /* target directory not empty */

    int mi = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (!g_mnt[i].used) { mi = i; break; }
    if (mi < 0) return -11;

    /* #142: refuse mounting the same partition a second time at another
     * point -- two volume slots over one LBA would keep diverging FAT caches.
     * The check covers the root volume slot 0 as well: re-binding the boot
     * partition under a second mount point is the same cache-divergence risk. */
    char dev[16];
    part_devname(p, dev, sizeof(dev));
    if (fat_volume_owns(p->drive, p->start)) return -15;

    int vol = -1;
    if (fat_mount_partition(p->drive, p->start, p->size, &vol) != 0)
        return -12;

    int n = 0;
    for (int j = 0; j < VFS_MAX_MOUNTS; j++) if (g_mnt[j].used) n++;
    g_mnt[mi].used = 1;
    g_mnt[mi].vol = vol;
    g_mnt[mi].letter = (char)(ROOT_LETTER + 1 + n);
    scpy(g_mnt[mi].mnt, abs);
    scpy(g_mnt[mi].dev, dev);

    fat_activate(0);                             /* back to the root volume */
    return FS_OK;
}

int fs_umount(const char *dir) {
    if (!fs_ready) return FS_ERR_NOENT;
    if (!dir || !*dir) return FS_ERR_INVAL;
    char abs[FS_PATH_MAX];
    abs_path(dir, abs);

    int mi = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (g_mnt[i].used && !scmp(g_mnt[i].mnt, abs)) { mi = i; break; }
    if (mi < 0) return FS_ERR_NOENT;

    if (path_under(cwd_path, abs)) return -14;    /* cwd is inside: busy */

    if (fat_active_slot() == g_mnt[mi].vol) fat_activate(0);
    fat_release_slot(g_mnt[mi].vol);
    g_mnt[mi].used = 0;
    return FS_OK;
}

static void df_put_k(uint32_t k) {
    char b[12]; int n = 0;
    if (k == 0) b[n++] = '0';
    while (k > 0) { b[n++] = (char)('0' + k % 10); k /= 10; }
    for (int p = n; p < 7; p++) vga_putc(' ');
    while (n > 0) vga_putc(b[--n]);
    vga_write("K");
}

void fs_mount_list(void) {
    vga_write("Filesystem  Mounted on\n");
    vga_write("hd0");
    for (int p = 3; p < 12; p++) vga_putc(' ');
    vga_write("/\n");
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mnt[i].used) continue;
        vga_write(g_mnt[i].dev);
        for (int p = slen(g_mnt[i].dev); p < 12; p++) vga_putc(' ');
        vga_write(g_mnt[i].mnt);
        vga_write("  (");
        vga_putc(g_mnt[i].letter);
        vga_write(":)\n");
    }
}

/* one df row for volume `vol` mounted at `mnt` (device `dev`) */
static void df_row(const char *dev, const char *mnt, int vol) {
    fat_activate(vol);
    uint32_t cs  = fat_cluster_size();
    uint32_t tot = fat_total_clusters();
    uint32_t fre = fat_free_clusters();
    uint32_t used = tot > fre ? tot - fre : 0;
    uint32_t tot_k = (uint32_t)(((uint64_t)tot * cs) / 1024);
    uint32_t use_k = (uint32_t)(((uint64_t)used * cs) / 1024);
    uint32_t fre_k = (uint32_t)(((uint64_t)fre * cs) / 1024);
    vga_write(dev);
    for (int p = slen(dev); p < 12; p++) vga_putc(' ');
    df_put_k(tot_k); vga_write("  ");
    df_put_k(use_k); vga_write("  ");
    df_put_k(fre_k); vga_write("  ");
    int pct = tot ? (int)(used * 100 / tot) : 0;
    char pc[4]; int pn = 0;
    if (pct == 0) pc[pn++] = '0';
    while (pct > 0) { pc[pn++] = (char)('0' + pct % 10); pct /= 10; }
    for (int p = pn; p < 3; p++) vga_putc(' ');
    while (pn > 0) vga_putc(pc[--pn]);
    vga_write("%  ");
    vga_write(mnt);
    vga_write("\n");
}

void fs_df(void) {
    if (!fs_ready) { vga_write("df: no FAT32 volume\n"); return; }
    vga_write("Filesystem   total    used    free   use%  Mounted on\n");
    df_row("hd0", "/", 0);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mnt[i].used) continue;
        df_row(g_mnt[i].dev, g_mnt[i].mnt, g_mnt[i].vol);
    }
    fat_activate(0);
}
