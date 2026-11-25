/* threads.c -- Stage 3A-3 slice C: four-thread isolation probe.
 *
 * Phase 1: four workers each hammer a private 1000-entry stack array, report
 * their own tid, then exit(id+1); main joins all four and checks the sum.
 * Phase 2: a detached worker exits; main does not join and keeps running.
 * Phase 3: a worker exits(1) non-zero; main joins and observes retval 1,
 * proving a worker's non-zero exit does not take down the process space.
 *
 * Build: sh extapp/build_ce.sh threads.c
 * Run:   run /bin/threads.ce
 */
#include "ce_app.h"

#define N 1000

static void *worker(void *arg) {
    long id = (long)arg;
    int own[N];
    for (int i = 0; i < N; i++) own[i] = i;
    long s = 0;
    for (int i = 0; i < N; i++) s += own[i];

    catos_api_t *a = __ce_api;
    /* per-thread errno: write a distinct non-zero value, read it back from
     * this thread's own TLS; a crosstalk would show another worker's value. */
    int *e = __tls_errno();
    *e = (int)id + 1;
    int eback = *e;
    a->write("threads: tid="); a->print_int(a->gettid());
    a->write(" i="); a->print_int(N);
    a->write(" errno="); a->print_int(eback);
    a->write(" own="); a->print_int((int)id);
    a->write(" sum="); a->print_int((int)s);
    a->write("\n");
    (void)s;
    a->thread_exit((void *)(id + 1));
    return 0;
}

static void *fault_worker(void *arg) {
    (void)arg;
    __ce_api->write("threads: fault worker exit(1)\n");
    __ce_api->thread_exit((void *)1);
    return 0;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *a = __ce_api;

    int tids[4];
    for (int k = 0; k < 4; k++) {
        tids[k] = a->thread_create(worker, (void *)k);
        if (tids[k] < 0) {
            a->write("threads: create FAIL k="); a->print_int(k);
            a->write(" tid="); a->print_int(tids[k]); a->write("\n");
            return 1;
        }
    }

    long sum = 0;
    for (int k = 0; k < 4; k++) {
        long rv = (long)a->thread_join(tids[k]);
        a->write("join ret="); a->print_int((int)rv); a->write("\n");
        sum += rv;
    }
    a->write("sum="); a->print_int((int)sum); a->write("\n");

    /* detach case: spawn a worker, detach it, do not join; process continues. */
    int dt = a->thread_create(worker, (void *)7);
    if (dt >= 0) a->thread_detach(dt);

    /* isolation: a non-zero worker exit must not kill the process. */
    int fl = a->thread_create(fault_worker, 0);
    long frv = fl >= 0 ? (long)a->thread_join(fl) : -99;
    a->write("fault join ret="); a->print_int((int)frv); a->write("\n");

    if (sum == 10 && frv == 1) {
        a->write("THREAD PASS\n");
        return 0;
    }
    a->write("THREAD FAIL sum="); a->print_int((int)sum);
    a->write(" fault="); a->print_int((int)frv); a->write("\n");
    return 2;
}
