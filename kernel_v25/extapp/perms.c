#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;


/* perms.ce - security pillar proof: runs as uid 100 (user) by default.
 *   perms              -> tries to overwrite system files (must FAIL with
 *                         -13 EACCES), then writes /home/user.txt (OK)
 *   sudo perms         -> same writes as root (must SUCCEED)
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

    api->write("save /etc/passwd (system file):   ");
    int r2 = api->save("/etc/passwd", payload, sizeof(payload));
    api->print_int(r2);
    api->write(r2 == -13 ? "  EACCES (protected!)\n" : "\n");

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
    return 0;
}
