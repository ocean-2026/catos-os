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
