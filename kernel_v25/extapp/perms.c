#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;


/* perms.ce - security pillar proof: runs as uid 100 (user) by default.
 *   perms              -> tries to overwrite system files (must FAIL with
 *                         -13 EACCES), then writes /home/user.txt (OK)
 *   sudo perms         -> the same writes as root.  The writes to the
 *                         system files are refused HERE TOO, and that is
 *                         the point: the built-in apps are installed
 *                         read-only (FAT read-only attribute), so even
 *                         root cannot replace the code in /bin -- that
 *                         was the persistent code-replacement hole the
 *                         read-only attribute closed (H8).  Only the
 *                         /home write succeeds.
 *
 * The second case used to write the REAL /etc/passwd, so running the
 * demo as root replaced the user database with 17 bytes of payload.
 * It now writes /etc/perm_demo.txt -- a plain file in a system
 * directory -- which keeps the three-tier comparison intact (built-in
 * app = refused / plain file in a system dir = writable by root /
 * user area = writable) without touching real state.  The product
 * semantics of /etc/passwd and /etc/acl are unchanged: useradd,
 * passwd and chcaps still need to rewrite them.
 *
 * The middle tier reads "allowed" for root and "refused" for an
 * unprivileged session, and that difference comes from the ACL, NOT
 * from the read-only attribute: fs_perm_check() only lets an
 * unprivileged user write inside the world-writable scratch whitelist
 * (/home/ and /tmp/), while /etc is 0755 and owned by root.  The
 * built-in apps are refused for the opposite reason -- they carry the
 * FAT read-only attribute, which no uid can talk the FAT layer out of.
 * Both refusals print -13, so the two tiers are told apart by their
 * reason, not by their error code; this asymmetry is existing ACL
 * semantics, not something the read-only attribute introduced.  For the
 * same reason the cleanup below is refused for an unprivileged user
 * ("demo cleanup: FAILED (rc=-13)"): there is nothing to delete there.
 *
 * Also demonstrates the new getuid() extension.
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    api->write("[perms] uid=");
    api->print_int(api->getuid());
    api->write(" tid=");
    api->print_int(api->gettid());
    api->write("\n");

    static const char payload[] = "owned by a task";

    api->write("save /bin/hello.ce (system file): ");
    int r1 = api->save("/bin/hello.ce", payload, sizeof(payload));
    api->print_int(r1);
    api->write(r1 == -13 ? "  EACCES (protected!)\n" : "\n");

    api->write("save /etc/perm_demo.txt (system dir, plain file): ");
    int r2 = api->save("/etc/perm_demo.txt", payload, sizeof(payload));
    api->print_int(r2);
    api->write(r2 == -13 ? "  EACCES (not writable)\n" : "\n");

    api->write("save /home/user.txt (user area):  ");
    int r3 = api->save("/home/user.txt", payload, sizeof(payload));
    api->print_int(r3);
    api->write(r3 == 0 ? "  OK\n" : "  FAILED\n");

    api->write("mkdir /home/mine (user area):     ");
    int r4 = api->mkdir("/home/mine");
    api->print_int(r4);
    api->write(r4 == 0 ? "  OK\n" : "  FAILED\n");

    api->write("mkdir /bin/mine (system dir):     ");
    int r5 = api->mkdir("/bin/mine");
    api->print_int(r5);
    api->write(r5 == -13 ? "  EACCES (protected!)\n" : "\n");

    /* Clean up: a demo must not leave anything behind in a system
     * directory.  "Write it, then delete it" also demonstrates the
     * second half of the middle tier -- a plain file in /etc is
     * removable as well as writable, because it is NOT a built-in and
     * therefore carries no read-only attribute.  If this delete ever
     * fails for root, the read-only check has started rejecting plain
     * files and has to be fixed. */
    api->write("demo cleanup: ");
    int r6 = api->file_delete("/etc/perm_demo.txt");
    api->write(r6 == 0 ? "removed" : "FAILED");
    api->write(" (rc=");
    api->print_int(r6);
    api->write(")\n");
    return 0;
}
