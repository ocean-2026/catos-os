#include "ce.h"
#include "vga.h"
#include "fs.h"
#include "hw.h"
#include "crypto.h"
#include "catos_version.h"
#include "builtin_apps.inc"

/* Copy the built-in CE apps and developer docs (embedded in the kernel
 * image, i.e. inside catos.iso) onto the FAT32 volume.
 *
 * Called right after fs_format() (so a freshly formatted disk starts with
 * a usable /bin) and once at boot as a fallback.
 *
 * A7.5: version + content-hash dual criterion.  /etc/builtin.ver records the
 * kernel version that seeded the volume plus the SHA-256 of every built-in
 * file as last seen on disk.  At seed time a file is refreshed from the
 * kernel image ONLY when the on-disk hash matches the recorded hash (i.e.
 * the user has not edited it); a file whose hash differs is kept and logged
 * as `builtin: kept user-modified <path>`.  The manifest is rebuilt on every
 * boot with the current kernel version.
 *
 * Hash note: SHA-256 from the kernel's existing crypto implementation
 * (crypto.c), so this is a real digest -- not a non-cryptographic checksum.
 */

#define BUILTIN_VER_PATH "/etc/builtin.ver"
#define VER_BUF_SZ       7168

/* One static scratch for the old manifest; rebuilt in a second pass so no
 * per-file hash table has to live in .bss. */
static char rec_buf[VER_BUF_SZ];

/* v2.81 fix: create a missing parent directory before writing a built-in.
 * fs_write_raw() fails with [F4] when the parent is absent; the old loop
 * bailed on the first failure, so a stale volume without /appdevelop left
 * node.ce / TEST.JS uninstalled after API.TXT failed. */
static int builtin_ensure_parent(const char *path) {
    char parent[128];
    int plen = 0;
    for (const char *q = path; *q; q++) if (*q == '/') plen = (int)(q - path);
    if (plen <= 1) return 0;                /* root-level file */
    if (plen >= (int)sizeof(parent)) return -1;
    for (int i = 0; i < plen; i++) parent[i] = path[i];
    parent[plen] = 0;
    int rc = fs_mkdir(parent);
    return (rc == FS_OK || rc == FS_ERR_EXIST) ? 0 : rc;
}

static int beq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void behex32(const uint8_t d[32], char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = h[d[i] >> 4];
        out[i * 2 + 1] = h[d[i] & 15];
    }
    out[64] = 0;
}

static void hash_embedded(const unsigned char *data, uint32_t len, char hex[65]) {
    uint8_t d[32];
    sha256(d, data, len);
    behex32(d, hex);
}

/* Hash the on-disk copy.  Returns 0 on success, -1 if the file cannot be
 * read. */
static int hash_disk(const char *path, char hex[65]) {
    uint32_t cl = 0, sz = 0;
    if (fs_resolve_file(path, &cl, &sz) != 0) return -1;
    static uint8_t chunk[4096];
    sha256_stream_t c;
    sha256_stream_init(&c);
    uint32_t off = 0;
    while (off < sz) {
        uint32_t want = sz - off;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        int r = fs_read_at(path, (char *)chunk, (int)off, (int)want);
        if (r <= 0) return -1;
        sha256_stream_update(&c, chunk, (uint32_t)r);
        off += (uint32_t)r;
    }
    uint8_t d[32];
    sha256_stream_final(&c, d);
    behex32(d, hex);
    return 0;
}

/* Look up the recorded last-known hash for `path` in rec_buf.  Leaves hex[0]
 * at 0 when absent. */
static void record_lookup_old(const char *path, char hex[65]) {
    hex[0] = 0;
    if (!rec_buf[0]) return;
    char *line = rec_buf;
    while (*line) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        if (line[0] != '#' && line[0] != 'v' && line[64] == ' ') {
            char *p = line + 65;
            while (*p == ' ') p++;
            if (beq(p, path)) {
                for (int k = 0; k < 64; k++) hex[k] = line[k];
                hex[64] = 0;
            }
        }
        if (!save) break;
        line = nl + 1;
    }
}

/* 1 if the manifest's version line matches the running kernel version. */
static int record_version_current(void) {
    if (!rec_buf[0]) return 0;
    char *line = rec_buf;
    while (*line) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        if (beq(line, "version=" CATOS_VERSION)) return 1;
        if (!save) break;
        line = nl + 1;
    }
    return 0;
}

static void rec_append(char *dst, int *o, const char *s, int cap) {
    while (*s && *o < cap - 1) dst[(*o)++] = *s++;
    dst[*o] = 0;
}

/* Rebuild /etc/builtin.ver from the final on-disk state.  Second pass: each
 * file is re-hashed off disk; if it vanished, the embedded hash is recorded. */
static void record_rebuild(void) {
    int n = (int)(sizeof(builtin_files) / sizeof(builtin_files[0]));
    int o = 0;
    rec_buf[0] = 0;
    rec_append(rec_buf, &o, "# catos built-in manifest: <sha256-hex> <path>\n", VER_BUF_SZ);
    rec_append(rec_buf, &o, "version=" CATOS_VERSION "\n", VER_BUF_SZ);
    for (int i = 0; i < n; i++) {
        char emb[65], disk[65];
        hash_embedded(builtin_files[i].data, builtin_files[i].len, emb);
        if (hash_disk(builtin_files[i].path, disk) != 0)
            for (int k = 0; k < 65; k++) disk[k] = emb[k];
        rec_append(rec_buf, &o, disk, VER_BUF_SZ);
        rec_append(rec_buf, &o, " ", VER_BUF_SZ);
        rec_append(rec_buf, &o, builtin_files[i].path, VER_BUF_SZ);
        rec_append(rec_buf, &o, "\n", VER_BUF_SZ);
    }
    fs_write_raw_mode(BUILTIN_VER_PATH, rec_buf, (uint32_t)o, 0);
}

void ce_install_builtins(void) {
    if (!fs_fs_ready()) return;
    btime_mark("B0 builtins resolve+seed");
    int n = (int)(sizeof(builtin_files) / sizeof(builtin_files[0]));

    /* Load the previous manifest (absent/legacy volume => empty). */
    rec_buf[0] = 0;
    int r = fs_read_text(BUILTIN_VER_PATH, rec_buf, (int)sizeof(rec_buf) - 1);
    if (r > 0) rec_buf[r] = 0; else rec_buf[0] = 0;
    record_version_current();   /* version mismatch: manifest rebuilt below */

    int installed = 0, upgraded = 0, kept = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        const char *path = builtin_files[i].path;
        const unsigned char *data = builtin_files[i].data;
        uint32_t len = builtin_files[i].len;

        uint32_t c = 0, s = 0;
        if (fs_resolve_file(path, &c, &s) != 0) {
            /* Missing on disk: install from the kernel image. */
            if (builtin_ensure_parent(path) != 0 ||
                fs_write_raw(path, (const char *)data, len) != 0) {
                vga_write("builtin: install FAILED for ");
                vga_write(path);
                vga_write(" (disk full?)\n");
                failed++;
                continue;
            }
            installed++;
            continue;
        }

        char disk_hex[65];
        if (hash_disk(path, disk_hex) != 0) {
            /* Unreadable on disk: leave it alone, do not clobber. */
            vga_write("builtin: kept unreadable ");
            vga_write(path);
            vga_write("\n");
            kept++;
            continue;
        }

        char old_hex[65];
        record_lookup_old(path, old_hex);

        if (old_hex[0] && beq(old_hex, disk_hex)) {
            /* User never touched it: refresh with the kernel image copy. */
            if (builtin_ensure_parent(path) != 0 ||
                fs_write_raw_mode(path, (const char *)data, len, 0) != 0) {
                vga_write("builtin: refresh FAILED for ");
                vga_write(path);
                vga_write(" (disk full?)\n");
                failed++;
            } else {
                upgraded++;
            }
        } else {
            /* Hash differs from the recorded last-known state: user edited
             * it -- keep the on-disk copy. */
            vga_write("builtin: kept user-modified ");
            vga_write(path);
            vga_write("\n");
            kept++;
        }
    }

    record_rebuild();

    /* Quiet on the common path: boot must not claim it installed anything
     * when /bin came from the disk.  Only report real work. */
    if (installed) vga_write("Installed missing built-in app(s)/doc(s) from kernel image.\n");
    if (upgraded)  vga_write("builtin: refreshed unmodified built-in(s) from kernel image.\n");
    if (kept)      vga_write("builtin: kept user-modified built-in(s); see lines above.\n");
    if (failed)    vga_write("builtin: some install(s) FAILED; see lines above\n");
    btime_mark("B1 builtins done");
}

/* NIGHT-P4: seed the local package repository (/pkg/) on the FAT volume.
 *
 * The kernel image embeds the packaged .catpkg containers (pkg_files[])
 * and their metadata (pkg_meta[]).  At boot (and after format) we drop
 * the containers into /pkg/ and write the line-style index PKGS.TXT
 * (`name|version|desc|file`) if it is absent.  `pkg install` then copies
 * the selected payload into /bin; `pkg remove` deletes that copy.
 *
 * Like ce_install_builtins this never overwrites existing files, so a
 * package set already customized on the volume wins over the kernel copy.
 */
void ce_install_pkg_repo(void) {
    if (!fs_fs_ready()) return;
    fs_mkdir("/pkg");
    btime_mark("P0 pkg repo resolve+seed");

    int np = (int)(sizeof(pkg_files) / sizeof(pkg_files[0]));
    for (int i = 0; i < np; i++) {
        uint32_t c = 0, s = 0;
        if (fs_resolve_file(pkg_files[i].path, &c, &s) == 0)
            continue;                       /* already seeded */
        if (fs_write_raw(pkg_files[i].path,
                          (const char *)pkg_files[i].data,
                          pkg_files[i].len) != 0) {
            vga_write("pkg: seed FAILED for ");
            vga_write(pkg_files[i].path);
            vga_write(" (disk full?)\n");
            return;
        }
    }

    /* Seed /pkg/PKGS.TXT from the embedded metadata when absent. */
    uint32_t c = 0, s = 0;
    if (fs_resolve_file("/pkg/PKGS.TXT", &c, &s) == 0) return;

    static char idx[2048];
    int len = 0;
    int nm = (int)(sizeof(pkg_meta) / sizeof(pkg_meta[0]));
    for (int i = 0; i < nm; i++) {
        const pkg_meta_t *m = &pkg_meta[i];
        const char *fields[4] = { m->name, m->version, m->desc, m->file };
        for (int f = 0; f < 4; f++) {
            const char *q = fields[f];
            while (*q && len < (int)sizeof(idx) - 2) idx[len++] = *q++;
            idx[len++] = (f < 3) ? '|' : '\n';
        }
    }
    idx[len] = 0;
    if (fs_write_raw("/pkg/PKGS.TXT", idx, (uint32_t)len) != 0)
        vga_write("pkg: could not write /pkg/PKGS.TXT\n");
    btime_mark("P1 pkg repo done");
}
