/* atk_sysprot.c -- attack vector: the *system file* write surface.
 *
 * WHY IT MATTERS
 *   The kernel ships its built-in CE applications inside the kernel image
 *   and installs them onto the FAT32 volume at boot.  The installer
 *   deliberately skips files that are already present ("so it never
 *   overwrites user changes"), which means anything that can write to
 *   /bin can permanently replace a built-in application: the replacement
 *   survives a reboot, because the installer will not restore it.
 *
 *   That turns "run any .ce" into a persistence channel, and it also
 *   defeats a plain read-only attribute: an 8.3 short name ("allocpro.ce")
 *   is a *different* directory entry than the built-in's long name
 *   ("ALLOCPRO~1.CE" + LFN "allocprobe.ce"), so the read-only bit on the
 *   built-in does not cover the shadow entry -- what matters is that name
 *   resolution prefers the long name.
 *
 *   So this probe checks four things:
 *     1. can a built-in app be overwritten through file_write()?
 *     2. ... through api->save() (a different write path)?
 *     3. ... be deleted?
 *     4. does an 8.3 shadow entry change what "allocprobe.ce" resolves to?
 *   and one control case: ordinary files must still be writable.
 *
 *   Expected after the fix: 1/2/3 are refused (-13 EACCES), the built-in's
 *   size is unchanged by the shadow write, and the control write succeeds.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;

static const char payload[] = "PWNED-SHADOW\n";

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    (void)argc; (void)argv;

    w("ATK-SYSPROT: system file write surface\n");

    step(1);
    w("baseline sizes of built-in apps (must stay constant)\n");
    m("hello_size", api->fsize("/bin/hello.ce"));
    m("allocprobe_size", api->fsize("/bin/allocprobe.ce"));

    step(2);
    w("overwrite a built-in app with file_write()\n");
    m("fw_builtin", api->file_write("/bin/hello.ce", "PWNED", 5, 0));

    step(3);
    w("same through api->save()\n");
    m("save_builtin", api->save("/bin/hello.ce", "PWNED", 5));

    step(4);
    w("delete a built-in app\n");
    m("del_builtin", api->file_delete("/bin/hello.ce"));

    step(5);
    w("8.3 shadow name must not take over the long name\n");
    m("shadow_write", api->file_write("/bin/allocpro.ce", payload,
                                      (int)sizeof(payload) - 1, 0));
    m("allocprobe_size_after", api->fsize("/bin/allocprobe.ce"));
    m("hello_size_after", api->fsize("/bin/hello.ce"));

    step(6);
    w("control: ordinary files must still be writable\n");
    m("normal_write", api->file_write("/tmp/sysprot.txt", "ok", 2, 0));
    m("normal_size", api->fsize("/tmp/sysprot.txt"));

    w("\nATK-DONE\n");
    return 0;
}
