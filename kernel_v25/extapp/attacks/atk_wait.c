/* atk_wait.c -- attack vector: SYS_WAIT on a task that is NOT your child
 * (src/task.c:1618-1625 + task_wait_child at :400).
 *
 * WHY IT MIGHT WEDGE
 *   case SYS_WAIT: task_by_tid(tid) -> any live slot, no parent check, then
 *   task_wait_child(ch) blocks in T_WAIT_CHILD until ch is ZOMBIE/UNUSED.
 *   The shell (tid 0) is T_WAIT_CHILD right now -- waiting for THIS
 *   program -- so wait(0) deadlocks both tasks.  wait(self) does the same.
 *   Since shell_run() is the kernel's main loop and it never returns, the
 *   console is permanently dead even though the kernel keeps ticking.
 *
 * The two deadlocks are split into separate opt-in passes (argv[1]) so the
 * driver can test the survivable variants first and the wedging one last.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    int me = api->gettid();
    (void)argc; (void)argv;

    w("ATK-WAIT: wait() on non-children\n");
    m("me", me); m("uid", api->getuid()); nl();

    step(1); mark("wait() on zombie / unused / bogus slots\n");
    m("w999", api->wait(999));
    m("w-1", api->wait(-1));
    m("w16", api->wait(16));
    nl();

    step(2); mark("wait() on a live child (bounded, must return)\n");
    {
        int tid = api->spawn("/BIN/HELLO.CE", 0, 0);
        m("spawn", tid);
        if (tid > 0) { m("wait", api->wait(tid)); }
        nl();
    }

    step(3); mark("wait() on a live NON-child task (other tid)\n");
    {
        /* spawn one child and wait for the shell instead: the child is
         * independent, the shell is blocked on us.  Only reached when
         * argv[1] == "2" so the driver can order the tests. */
        if (argc >= 2 && streq(argv[1], "2")) {
            w("WAIT0-BEFORE\n");
            m("wait(0)", api->wait(0));
            w("WAIT0-AFTER\n");
        } else {
            w("(skipped: pass arg 2 to run the shell-deadlock pass)\n");
        }
    }

    step(4); mark("wait(self)\n");
    if (argc >= 2 && streq(argv[1], "1")) {
        w("WAITSELF-BEFORE\n");
        m("wait(me)", api->wait(me));
        w("WAITSELF-AFTER\n");
    } else {
        w("(skipped: pass arg 1 to run the self-deadlock pass)\n");
    }

    w("\nATK-DONE\n");
    return 0;
}
