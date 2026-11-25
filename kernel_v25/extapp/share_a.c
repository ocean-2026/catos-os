/* share_a.c -- 2C block ④ dual-process shared-library probe, child A.
 *
 * Opens /bin/libmath.so, reports its tid, waits on the parent's pipe
 * barrier, then exercises the read-only `version`/`hello` exports three
 * times before closing the library.  The kernel serial log records which
 * physical RO frame this task mapped; it must match child B's frame.
 *
 * Build: sh extapp/build_ce.sh share_a.c
 * argv[1] = ready pipe handle (child -> parent, decimal).
 * argv[2] = release pipe handle (parent -> child, decimal).
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api")));
#define CE_API() (__ce_api)

typedef const char *(*fn0s)(void);
typedef void       (*fn0v)(void);

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = CE_API();
    int pr = (argc > 1) ? a->atoi(argv[1]) : -1;
    int pw = (argc > 2) ? a->atoi(argv[2]) : -1;

    int h = a->dlopen("/bin/libmath.so");
    if (h <= 0) {
        a->write("share_a: dlopen failed (");
        a->print_int(h); a->write(")\n");
        return 2;
    }

    fn0s ver  = (fn0s)a->dlsym(h, "version");
    fn0v hello = (fn0v)a->dlsym(h, "hello");
    if (!ver || !hello) {
        a->write("share_a: dlsym NULL\n");
        a->dlclose(h);
        return 3;
    }

    a->write("share_a: tid="); a->print_int(a->gettid()); a->write(" ready\n");

    /* Barrier: signal the parent we are resident, then block until release. */
    char ready = 'A';
    a->pipe_write(pr, &ready, 1);
    char rel[1];
    a->pipe_read(pw, rel, 1);

    for (int i = 0; i < 3; i++) {
        a->write("share_a["); a->print_int(i); a->write("] ");
        a->write(ver()); a->write("\n");
        hello();
    }

    a->dlclose(h);
    a->write("share_a: done\n");
    return 0;
}
