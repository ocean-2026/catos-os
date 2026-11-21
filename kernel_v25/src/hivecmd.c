/*
 * hivecmd.c -- `hive` shell command (v2.67).
 */
#include "hivecmd.h"
#include "fs.h"
#include "hive.h"
#include "palette.h"
#include "render.h"
#include "vga.h"

#define RC_OK     0
#define RC_FAIL   1
#define RC_USAGE  2
#define RC_PERM   13

static int strcmp_(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void out(const char *s) { vga_write(s); }
static void outv(const char *k, int v, int hex) {
    vga_write(k);
    if (hex) {
        char b[12];
        static const char hd[] = "0123456789ABCDEF";
        int n = 0;
        if (v == 0) b[n++] = '0';
        while (v > 0) { b[n++] = hd[v & 0xF]; v >>= 4; }
        for (int i = 0; i < n / 2; i++) { char t = b[i]; b[i] = b[n-1-i]; b[n-1-i] = t; }
        for (int i = 0; i < n; i++) con_putc(b[i]);
    } else {
        char b[12];
        int n = 0, neg = 0;
        if (v < 0) { neg = 1; v = -v; }
        if (v == 0) b[n++] = '0';
        while (v > 0) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
        if (neg) b[n++] = '-';
        for (int i = n - 1; i >= 0; i--) con_putc(b[i]);
    }
}
static int tok(char **pp, char *dst, int cap) {
    char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; return 0; }
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && n < cap - 1) dst[n++] = *p++;
    dst[n] = 0;
    *pp = p;
    return 1;
}

static int is_admin(int uid, uint32_t caps) {
    return uid == 0 || (caps & 0x0010u);        /* root or CAP_USERADM */
}

static void print_value(const char *path) {
    hive_val_t v;
    int r = hive_get(path, &v);
    if (r == HIVE_ENOENT) {
        out("hive: no such key or value: "); out(path); out("\n");
        return;
    }
    if (r == HIVE_ENOTDIR) {
        out("hive: that path is a key, not a value: "); out(path); out("\n");
        return;
    }
    out(path); out(" = ");
    switch (v.kind) {
    case HIVE_SZ:
        for (int i = 0; i < (int)v.len; i++) con_putc((char)v.data[i]);
        out("  (sz)\n");
        break;
    case HIVE_DW: {
        uint32_t d = 0;
        for (int i = 3; i >= 0; i--) d = (d << 8) | v.data[i];
        outv("", (int)d, 0);
        out("  (dw)\n");
        break;
    }
    case HIVE_BIN: {
        out("0x");
        for (int i = 0; i < (int)v.len && i < 32; i++) {
            static const char hd[] = "0123456789ABCDEF";
            con_putc(hd[(v.data[i] >> 4) & 0xF]);
            con_putc(hd[v.data[i] & 0xF]);
        }
        if (v.len > 32) out("...");
        out("  (bin ");
        outv("", v.len, 0);
        out(" bytes)\n");
        break;
    }
    default:
        out("(unknown type)\n");
    }
}

static void kind_tag(uint8_t k) {
    if (k == HIVE_KEY)     out("/");
    else if (k == HIVE_DW) out("  (dw)");
    else if (k == HIVE_BIN) out("  (bin)");
}

static int do_ls(const char *path, int recursive, int depth) {
    char name[HIVE_NAME_MAX];
    uint8_t kind;
    int n = hive_child_count(path);
    if (n == 0 && !hive_exists(path)) {
        out("hive: no such key: "); out(path); out("\n");
        return RC_FAIL;
    }
    for (int i = 0; i < n; i++) {
        if (hive_child_at(path, i, name, HIVE_NAME_MAX, &kind) != HIVE_OK) break;
        for (int d = 0; d < depth; d++) out("  ");
        if (kind == HIVE_KEY) {
            out("["); out(name); out("]\n");
            if (recursive && depth < 10) {
                char sub[HIVE_PATH_MAX];
                hive_child_path(path, i, sub, HIVE_PATH_MAX);
                do_ls(sub, 1, depth + 1);
            }
        } else {
            char sub[HIVE_PATH_MAX];
            hive_child_path(path, i, sub, HIVE_PATH_MAX);
            out(name); out(" = ");
            hive_val_t v;
            if (hive_get(sub, &v) == HIVE_OK) {
                if (v.kind == HIVE_SZ) {
                    for (int j = 0; j < (int)v.len; j++) con_putc((char)v.data[j]);
                } else if (v.kind == HIVE_DW) {
                    uint32_t d = 0;
                    for (int j = 3; j >= 0; j--) d = (d << 8) | v.data[j];
                    outv("", (int)d, 0);
                } else {
                    out("[binary ");
                    outv("", v.len, 0);
                    out(" bytes]");
                }
                kind_tag(kind);
            }
            out("\n");
        }
    }
    return RC_OK;
}

static void usage(void) {
    out("usage: hive [ls [-R] [PATH]] | get PATH | set PATH VALUE | setdw PATH N |\n"
        "            mkkey PATH | del PATH | delkey PATH | export | import | chk | save\n");
    out("       paths look like HKLM\\Software\\CatOS\\Terminal\\Colors\\Background\n");
    out("       roots: HKLM machine, HKCU this user (HKU\\<uid>), HKSEC policy\n");
}

int hive_cmd_run(char *arg, int uid, uint32_t caps) {
    hive_stat_t st;
    char sub[24], path[HIVE_PATH_MAX], val[HIVE_DATA_MAX];
    path[0] = 0; val[0] = 0;
    char *p = arg;

    if (!tok(&p, sub, sizeof(sub)) || !*sub) {
        hive_stat(&st);
        out("CatOS hive  /etc/hive\n");
        outv("  loaded from disk: ", st.loaded, 0); out("\n");
        outv("  generation:       ", (int)st.generation, 0); out("\n");
        outv("  nodes:            ", (int)st.nodes, 0);
        outv("   keys: ", (int)st.keys, 0);
        outv("   values: ", (int)st.values, 0); out("\n");
        outv("  bytes:            ", (int)st.bytes, 0); out("\n");
        out("  crc32:            0x"); outv("", (int)st.crc, 1); out("\n");
        if (st.corrupt) out("  WARNING: the previous /etc/hive failed validation and was quarantined\n");
        if (st.migrated) out("  note: legacy /etc/reg + one-key files were imported this boot\n");
        out("  roots:\n");
        do_ls("HKLM", 0, 1);
        do_ls("HKU", 0, 1);
        do_ls("HKSEC", 0, 1);
        return RC_OK;
    }

    if (!strcmp_(sub, "help") || !strcmp_(sub, "-h") || !strcmp_(sub, "--help")) {
        usage();
        return RC_OK;
    }

    if (!strcmp_(sub, "ls") || !strcmp_(sub, "list") || !strcmp_(sub, "tree")) {
        int rec = 0;
        if (tok(&p, path, sizeof(path)) && !strcmp_(path, "-R")) {
            rec = 1;
            path[0] = 0;
            tok(&p, path, sizeof(path));
        }
        if (!path[0]) {
            out("[HKLM]\n"); do_ls("HKLM", rec, 1);
            out("[HKU]\n");  do_ls("HKU", rec, 1);
            out("[HKSEC]\n");do_ls("HKSEC", rec, 1);
            return RC_OK;
        }
        return do_ls(path, rec, 0);
    }

    if (!strcmp_(sub, "get")) {
        if (!tok(&p, path, sizeof(path)) || !path[0]) {
            out("hive: get: missing PATH\n");
            usage();
            return RC_USAGE;
        }
        print_value(path);
        return hive_exists(path) ? RC_OK : RC_FAIL;
    }

    if (!strcmp_(sub, "set") || !strcmp_(sub, "setdw")) {
        if (!is_admin(uid, caps)) {
            out("hive: permission denied (need root or CAP_USERADM)\n");
            return RC_PERM;
        }
        if (!tok(&p, path, sizeof(path)) || !path[0]) {
            out("hive: set: missing PATH\n"); usage(); return RC_USAGE;
        }
        /* the value is the REST of the line: never split a value on spaces */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { out("hive: set: missing VALUE\n"); usage(); return RC_USAGE; }
        int n = 0;
        while (*p && n < HIVE_DATA_MAX - 1) val[n++] = *p++;
        val[n] = 0;

        if (!strcmp_(sub, "setdw")) {
            int neg = 0, i = 0;
            uint32_t d = 0;
            while (val[i] == ' ') i++;
            if (val[i] == '-') { neg = 1; i++; }
            if (val[i] == '0' && (val[i+1] == 'x' || val[i+1] == 'X')) {
                i += 2;
                while (val[i]) {
                    int c = val[i];
                    int h = (c >= '0' && c <= '9') ? c - '0'
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (h < 0) { out("hive: setdw: not a number\n"); return RC_USAGE; }
                    d = d * 16 + (uint32_t)h; i++;
                }
            } else {
                while (val[i]) {
                    if (val[i] < '0' || val[i] > '9') { out("hive: setdw: not a number\n"); return RC_USAGE; }
                    d = d * 10 + (uint32_t)(val[i] - '0'); i++;
                }
            }
            if (neg) d = (uint32_t)(-(int32_t)d);
            int r = hive_set_dw(path, d);
            if (r == HIVE_E2BIG) { out("hive: value too long\n"); return RC_FAIL; }
            if (r != HIVE_OK)   { out("hive: write failed (node table full?)\n"); return RC_FAIL; }
            out("hive: "); out(path); out(" = "); outv("", (int)d, 0); out(" (dw)\n");
            hive_save();
            return RC_OK;
        }

        int r = hive_set_sz(path, val);
        if (r == HIVE_E2BIG) { out("hive: value too long (max 79 chars, NOT truncated)\n"); return RC_FAIL; }
        if (r != HIVE_OK)   { out("hive: write failed (node table full?)\n"); return RC_FAIL; }
        out("hive: "); out(path); out(" = "); out(val); out("\n");
        hive_save();
        return RC_OK;
    }

    if (!strcmp_(sub, "mkkey")) {
        if (!is_admin(uid, caps)) { out("hive: permission denied (need root or CAP_USERADM)\n"); return RC_PERM; }
        if (!tok(&p, path, sizeof(path)) || !path[0]) { out("hive: mkkey: missing PATH\n"); usage(); return RC_USAGE; }
        int r = hive_mkkey(path);
        if (r != HIVE_OK) { out("hive: cannot create key (table full)\n"); return RC_FAIL; }
        out("hive: key created: "); out(path); out("\n");
        hive_save();
        return RC_OK;
    }

    if (!strcmp_(sub, "del") || !strcmp_(sub, "delkey")) {
        if (!is_admin(uid, caps)) { out("hive: permission denied (need root or CAP_USERADM)\n"); return RC_PERM; }
        if (!tok(&p, path, sizeof(path)) || !path[0]) { out("hive: del: missing PATH\n"); usage(); return RC_USAGE; }
        if (!hive_exists(path)) { out("hive: no such key or value: "); out(path); out("\n"); return RC_FAIL; }
        int iskey = (hive_kind(path) == HIVE_KEY);
        if (iskey && !strcmp_(sub, "del")) {
            out("hive: that path is a key; use `hive delkey` (or `hive ls` to inspect)\n");
            return RC_USAGE;
        }
        int r = iskey ? hive_delkey(path) : hive_del(path);
        if (r != HIVE_OK) { out("hive: delete failed\n"); return RC_FAIL; }
        out("hive: deleted "); out(path); out("\n");
        hive_save();
        return RC_OK;
    }

    if (!strcmp_(sub, "export") || !strcmp_(sub, "dump")) {
        static char buf[16384];
        int n = hive_dump(buf, (int)sizeof(buf));
        if (n < 0) { out("hive: export buffer full (hive larger than 16 KiB of text)\n"); return RC_FAIL; }
        for (int i = 0; i < n; i++) con_putc(buf[i]);
        return RC_OK;
    }

    if (!strcmp_(sub, "import")) {
        if (!is_admin(uid, caps)) { out("hive: permission denied (need root or CAP_USERADM)\n"); return RC_PERM; }
        static char tbuf[6144];
        int n = fs_read_text("/etc/reg", tbuf, (int)sizeof(tbuf) - 1);
        if (n <= 0) { out("hive: /etc/reg not found (nothing to import)\n"); return RC_FAIL; }
        tbuf[n] = 0;
        int imported = hive_import_ini(0, tbuf, n);
        if (imported <= 0) { out("hive: no settings imported from /etc/reg\n"); return RC_FAIL; }
        hive_save();
        outv("hive: imported ", imported, 0); out(" settings from /etc/reg\n");
        return RC_OK;
    }

    if (!strcmp_(sub, "chk") || !strcmp_(sub, "check")) {
        hive_stat_t st;
        hive_stat(&st);
        out("hive: in-memory tree ");
        out(hive_verify() == 0 ? "SANE\n" : "CORRUPT\n");
        outv("  nodes=", (int)st.nodes, 0);
        outv(" keys=", (int)st.keys, 0);
        outv(" values=", (int)st.values, 0); out("\n");
        outv("  generation=", (int)st.generation, 0);
        outv("  bytes=", (int)st.bytes, 0);
        out("  crc32=0x");
        {
            char hb[9];
            uint32_t c = st.crc;
            for (int i = 7; i >= 0; i--) { hb[i] = "0123456789ABCDEF"[c & 0xF]; c >>= 4; }
            hb[8] = 0;
            out(hb);
        }
        out("\n");
        out("  on-disk state: ");
        if (st.corrupt) out("QUARANTINED (see /etc/hive.bad)\n");
        else if (st.loaded) out("loaded and CRC-verified at boot\n");
        else out("absent (defaults seeded)\n");
        outv("  dirty (unflushed): ", hive_dirty(), 0); out("\n");
        /* v2.67: validate the volume without clobbering the live tree --
         * the old hive_load() here emptied the running registry whenever
         * the re-read failed. */
        int ilen = 0;
        int r = hive_check_image(&ilen);
        if (r == HIVE_OK) {
            out("  on-disk image: CRC OK, identical to the live tree (");
            outv("", ilen, 0); out(" bytes)\n");
            return RC_OK;
        }
        if (r == HIVE_ESTALE) {
            out("  on-disk image: CRC OK but STALE vs the live tree");
            out(hive_dirty() ? " (unsaved changes)" : " (in-memory edits)");
            out(" -- run `hive save`\n");
            return RC_FAIL;
        }
        if (r == HIVE_ENOENT) {
            out("  on-disk image: none yet -- run `hive save`\n");
            return hive_dirty() ? RC_FAIL : RC_OK;
        }
        out("  on-disk image: CORRUPT (magic/CRC/length) -- quarantined copy kept\n");
        return RC_FAIL;
    }

    if (!strcmp_(sub, "save") || !strcmp_(sub, "commit") || !strcmp_(sub, "flush")) {
        if (!is_admin(uid, caps)) { out("hive: permission denied (need root or CAP_USERADM)\n"); return RC_PERM; }
        int r = hive_save();
        if (r == HIVE_OK)        { out("hive: saved and read-back verified\n"); return RC_OK; }
        if (r == HIVE_ECODE)     { out("hive: read-back mismatch -- previous image restored from /etc/hive.bak\n"); return RC_FAIL; }
        out("hive: write failed (volume not ready?)\n");
        return RC_FAIL;
    }

    out("hive: unknown subcommand '"); out(sub); out("'\n");
    usage();
    return RC_USAGE;
}
