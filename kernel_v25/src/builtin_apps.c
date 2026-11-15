#include "ce.h"
#include "vga.h"
#include "fs.h"
#include "builtin_apps.inc"

/* Copy the built-in CE apps and developer docs (embedded in the kernel
 * image, i.e. inside catos.iso) onto the FAT32 volume.
 *
 * Called right after fs_format() (so a freshly formatted disk starts with
 * a usable /bin) and once at boot as a fallback.  Skips files that are
 * already present, so it never overwrites user changes.
 */
void ce_install_builtins(void) {
    if (!fs_fs_ready()) return;
    int n = (int)(sizeof(builtin_files) / sizeof(builtin_files[0]));
    int installed = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = 0, s = 0;
        if (fs_resolve_file(builtin_files[i].path, &c, &s) == 0)
            continue;                       /* already installed */
        if (fs_write_raw(builtin_files[i].path,
                         (const char *)builtin_files[i].data,
                         builtin_files[i].len) != 0) {
            vga_write("builtin: install FAILED for ");
            vga_write(builtin_files[i].path);
            vga_write(" (disk full?)\n");
            return;
        }
        installed++;
    }
    /* Quiet on the common path: boot must not claim it installed anything
     * when /bin came from the disk.  Only report real work. */
    if (installed) vga_write("Installed missing built-in app(s)/doc(s) from kernel image.\n");
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
}
