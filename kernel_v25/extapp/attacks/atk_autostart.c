/* atk_autostart.c -- attack vector: the boot-time auto-execution surface.
 *
 * WHY IT MATTERS
 *   /etc/autoexec is a directory of shell scripts that the kernel runs
 *   *automatically at every boot* (src/shell.c: boot_autostart() reads
 *   each file and hands every line to shell_exec()).  The directory is
 *   writable by the session user, so any program that gets run once can
 *   leave a script behind that will execute on the next boot -- no user
 *   interaction, and it survives reboots because nothing restores that
 *   directory.
 *
 *   This probe drops such a script and reports what the kernel says.
 *   The verification is done by the harness, not by this program: after
 *   the attack the image is rebooted and the harness greps the boot
 *   screen for the marker below.  If the marker shows up, the system
 *   executed an attacker-supplied file on its own.
 *
 *   Expected after the fix: the write succeeds (the directory stays
 *   writable -- users are meant to put scripts there), but the boot
 *   screen must NOT contain the marker, and the kernel banner must
 *   still be printed.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;

static const char marker[] = "echo PWNED_MARKER\n";

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    (void)argc; (void)argv;

    w("ATK-AUTOS: boot-time auto-execution surface\n");

    step(1);
    w("drop a script into /etc/autoexec\n");
    m("write", api->file_write("/etc/autoexec/pwn.sh", marker,
                                (int)sizeof(marker) - 1, 0));

    step(2);
    w("the script must exist on disk (the directory stays writable)\n");
    m("fsize_pwn", api->fsize("/etc/autoexec/pwn.sh"));

    step(3);
    w("the kernel banner file must still be there\n");
    m("fsize_banner", api->fsize("/etc/autoexec/banner.sh"));

    w("\nATK-DONE\n");
    return 0;
}
