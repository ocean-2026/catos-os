#include "fs.h"
#include "fat.h"
#include "vga.h"
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

/* FAT 8.3 without LFN: a component longer than 8 chars before the dot or
 * 3 after silently truncated to a colliding 8.3 name (two long names
 * then collided as "already exists").  Reject them explicitly instead. */
static int name_too_long83(const char *comp) {
    int body = 0, ext = 0, seen_dot = 0;
    for (const char *p = comp; *p; p++) {
        if (*p == '.') {
            if (seen_dot) return 1;
            seen_dot = 1;
            continue;
        }
        if (!seen_dot) body++; else ext++;
    }
    return body > 8 || ext > 3;
}

/* check every component of an absolute path; 0 = fine, -1 = too long */
static int path_has_long_names(const char *abs) {
    const char *p = abs;
    char comp[FS_NAME_MAX + 8];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int l = 0;
        while (*p && *p != '/' && l < (int)sizeof(comp) - 1) comp[l++] = *p++;
        while (*p && *p != '/') p++;
        comp[l] = 0;
        if (name_too_long83(comp)) return -1;
    }
    return 0;
}
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

/* resolve an absolute path (must start with '/'); returns FAT cluster of
 * the final component, or 0 on error. */
static uint32_t resolve_path(const char *abs, fat_dirent_t *out) {
    uint32_t cur = fat_root();
    int filled = 0;
    const char *p = abs;
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
        fat_name83(name, n83);
        fat_dirent_t e;
        if (fat_lookup(cur, n83, &e) != 0) return 0;
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
    fs_ready = (fat_probe() == 0);
    g_cwd = fat_root();
    scpy(cwd_path, "/");
    memz(&g_ent, sizeof(g_ent));
}

int fs_fs_ready(void) { return fs_ready; }

void fs_format(void) {
    fat_mkfs();
    fs_init();
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
    for (const char *q = leaf; *q; q++)
        if (*q == '/' || *q < 32) return FS_ERR_INVAL;

    uint8_t n83[11];
    fat_name83(leaf, n83);
    fat_dirent_t tmp;
    if (fat_lookup(pclus, n83, &tmp) == 0) return FS_ERR_EXIST;

    int rc = (type == FS_DIR) ? fat_mkdir(pclus, n83) : fat_mkfile(pclus, n83);
    return rc == 0 ? FS_OK : FS_ERR_FULL;
}

int fs_mkdir(const char *path) {
    if (path_has_long_names(path)) return FS_ERR_INVAL; return create_path(path, FS_DIR); }

int fs_touch(const char *path) {
    if (path_has_long_names(path)) return FS_ERR_INVAL;
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

/* ---------- list / pwd ---------- */
void fs_list(const char *path) {
    if (!fs_ready) {
        vga_write("catos: no FAT32 volume. Run 'format' first.\n");
        return;
    }
    char abs[FS_PATH_MAX];
    abs_path(path, abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus) { vga_write("catos: dir: no such directory\n"); return; }
    if (!(e.attr & FAT_ATTR_DIR)) { vga_write("catos: dir: not a directory\n"); return; }

    int count = 0;
    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    uint32_t c = clus;
    char shown[16];
    while (c >= 2 && c < 0x0FFFFFF0) {
        if (fat_read_cluster(c, buf) != 0) break;
        int ended = 0;
        for (int i = 0; i < (int)(fat_cluster_size() / 32); i++) {
            uint8_t *en = buf + i * 32;
            if (en[0] == 0x00) { ended = 1; break; }  /* end of dir */
            if (en[0] == 0xE5) continue;             /* deleted */
            if (en[11] == 0x0F) continue;            /* LFN */
            if (en[11] & 0x08) continue;             /* volume label */
            fat_name_show(en, shown);
            if (en[11] & FAT_ATTR_DIR) {
                vga_set_fg(VGA_LIGHT_CYAN);
                vga_write("[DIR ] ");
                vga_write(shown);
                vga_write("/\n");
            } else {
                vga_set_fg(VGA_LIGHT_GREY);
                vga_write("[FILE] ");
                vga_write(shown);
                vga_set_fg(VGA_WHITE);
                vga_write("  (");
                uint32_t sz = (uint32_t)en[28]
                            | ((uint32_t)en[29] << 8)
                            | ((uint32_t)en[30] << 16)
                            | ((uint32_t)en[31] << 24);
                print_int((int)sz);
                vga_write(" bytes)\n");
            }
            count++;
        }
        if (ended) break;
        uint32_t n = fat_get(c);
        if (n == FAT_EOC) break;
        c = n;
    }
    vga_set_fg(VGA_WHITE);
    vga_write("--- ");
    print_int(count);
    vga_write(count == 1 ? " entry ---\n" : " entries ---\n");
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
    fat_name83(leaf, n83);
    fat_dirent_t e;
    if (fat_lookup(pclus, n83, &e) != 0) return 0;
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

    uint8_t n83[11];
    fat_name83(leaf, n83);
    fat_dirent_t e;
    if (fat_lookup(pclus, n83, &e) != 0) return -1;
    if (e.attr & FAT_ATTR_DIR) return -1;
    if (cluster) *cluster = e.cluster;
    if (size)    *size = e.size;
    return 0;
}

int fs_foreach(const char *path, fs_foreach_cb cb, void *ud) {
    if (!fs_ready || !cb) return -1;
    char abs[FS_PATH_MAX];
    abs_path(path ? path : "/", abs);
    fat_dirent_t e;
    uint32_t clus = resolve_path(abs, &e);
    if (!clus) return -1;
    if (!(e.attr & FAT_ATTR_DIR)) { fs_dbg2("[F5]",0); return -1; }

    static uint8_t buf[FAT_CLUSTER_SZ_MAX];
    char shown[16];
    uint32_t c = clus;
    while (c >= 2 && c < 0x0FFFFFF0) {
        if (fat_read_cluster(c, buf) != 0) break;
        int ended = 0;
        for (int i = 0; i < (int)(fat_cluster_size() / 32); i++) {
            uint8_t *en = buf + i * 32;
            if (en[0] == 0x00) { ended = 1; break; }   /* end of dir */
            if (en[0] == 0xE5) continue;               /* deleted */
            if (en[11] == 0x0F) continue;              /* LFN */
            if (en[11] & 0x08) continue;               /* volume label */
            fat_name_show(en, shown);
            uint32_t sz = (uint32_t)en[28]
                        | ((uint32_t)en[29] << 8)
                        | ((uint32_t)en[30] << 16)
                        | ((uint32_t)en[31] << 24);
            cb((en[11] & FAT_ATTR_DIR) ? 1 : 0, shown, sz, ud);
        }
        if (ended) break;
        uint32_t n = fat_get(c);
        if (n == FAT_EOC) break;
        c = n;
    }
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
    fat_name83(leaf, n83);
    fat_dirent_t e;
    uint32_t pclus = resolve_path(parent, &e);
    if (!pclus) { fs_dbg2("[F4]",0); return -1; }
    if (!(e.attr & FAT_ATTR_DIR)) { fs_dbg2("[F5]",0); return -1; }
    if (fat_lookup(pclus, n83, &e) == 0) return 0;      /* already installed */

    if (fat_mkfile(pclus, n83) != 0) return -1;
    if (fat_lookup(pclus, n83, &e) != 0) return -1;
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
    fat_name83(leaf, n83);
    fat_dirent_t e;
    uint32_t pclus = resolve_path(parent, &e);
    if (!pclus) { fs_dbg2("[F4]",0); return -1; }
    if (!(e.attr & FAT_ATTR_DIR)) { fs_dbg2("[F5]",0); return -1; }
    if (fat_lookup(pclus, n83, &e) != 0) {
        if (fat_mkfile(pclus, n83) != 0) { fs_dbg2("[F6]",0); return -1; }
        if (fat_lookup(pclus, n83, &e) != 0) { fs_dbg2("[F7]",0); return -1; }
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
    fat_name83(leaf, n83);
    fat_dirent_t e;
    uint32_t pclus = resolve_path(parent, &e);
    if (!pclus) { fs_dbg2("[F4]",0); return -1; }
    if (fat_lookup(pclus, n83, &e) != 0) return -1;
    if (e.attr & FAT_ATTR_DIR) return -1;
    fat_free_chain(e.cluster);
    return fat_delete_entry(pclus, n83);
}
