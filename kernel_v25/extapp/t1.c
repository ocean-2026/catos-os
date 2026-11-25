/* t1.c -- Stage 3A-3 slice B: single user-thread create/join smoke.
 *
 * Main thread creates one worker; the worker writes "T1", exits(0); the
 * main joins and expects retval 0, then prints T1 PASS.
 *
 * Build: sh extapp/build_ce.sh t1.c
 * Run:   run /bin/t1.ce
 */
#include "ce_app.h"

static void *worker(void *arg) {
    (void)arg;
    __ce_api->write("T1\n");
    __ce_api->thread_exit((void *)0);
    return 0;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *a = __ce_api;

    int tid = a->thread_create(worker, 0);
    a->write("t1: create tid="); a->print_int(tid); a->write("\n");
    if (tid < 0) {
        a->write("T1 FAIL create\n");
        return 1;
    }

    long rv = (long)a->thread_join(tid);
    a->write("join ret="); a->print_int((int)rv); a->write("\n");

    if (rv == 0) {
        a->write("T1 PASS\n");
        return 0;
    }
    a->write("T1 FAIL join\n");
    return 2;
}
