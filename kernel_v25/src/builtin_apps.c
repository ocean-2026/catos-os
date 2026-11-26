#include "ce.h"
#include "vga.h"
#include "fs.h"
#include "hw.h"
#include "builtin_apps.inc"

/* Copy the built-in CE apps and developer docs (embedded in the kernel
 * image, i.e. inside catos.iso) onto the FAT32 volume.
 *
 * Called right after fs_format() (so a freshly formatted disk starts with
 * a usable /bin) and once at boot as a fallback.  Skips files that are
 * already present, so it never overwrites user changes.
 */
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

void ce_install_builtins(void) {
    if (!fs_fs_ready()) return;
    btime_mark("B0 builtins resolve+seed");
    int n = (int)(sizeof(builtin_files) / sizeof(builtin_files[0]));
    int installed = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = 0, s = 0;
        if (fs_resolve_file(builtin_files[i].path, &c, &s) == 0)
            continue;                       /* already installed */
        if (builtin_ensure_parent(builtin_files[i].path) != 0 ||
            fs_write_raw(builtin_files[i].path,
                         (const char *)builtin_files[i].data,
                         builtin_files[i].len) != 0) {
            vga_write("builtin: install FAILED for ");
            vga_write(builtin_files[i].path);
            vga_write(" (disk full?)\n");
            failed++;                       /* keep going: one bad file must not
                                             * strand the rest */
            continue;
        }
        installed++;
    }
    /* Quiet on the common path: boot must not claim it installed anything
     * when /bin came from the disk.  Only report real work. */
    if (installed) vga_write("Installed missing built-in app(s)/doc(s) from kernel image.\n");
    if (failed) vga_write("builtin: some install(s) FAILED; see lines above\n");
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
