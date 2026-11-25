/* preempt.c -- Stage 3 block 3: minimal preemption probe.
 *
 * Two pure-compute workers busy-loop in ring3.  The PIT tick must preempt each
 * at a tick boundary and round-robin to the other; each worker reads its own
 * scheduler switch counter (slot 93) before and after the burst and reports the
 * delta.  A non-zero delta for both proves the compute threads were actually
 * switched away and back; the probe must not crash.
 *
 * Build: sh extapp/build_ce.sh preempt.c
 * Run:   run /bin/preempt.ce
 */
#include "ce_app.h"

#define BURST 500000L

static volatile long sink;
static int deltas[2];

static void *worker(void *arg) {
    long id = (long)arg;
    catos_api_t *a = __ce_api;

    uint32_t s0 = a->thread_switches();
    long x = 0;
    for (long i = 0; i < BURST; i++) x += i;
    sink += x;
    uint32_t s1 = a->thread_switches();

    deltas[id] = (int)(s1 - s0);
    a->write("preempt: worker="); a->print_int((int)id);
    a->write(" switches="); a->print_int(deltas[id]);
    a->write("\n");
    a->thread_exit((void *)0);
    return 0;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *a = __ce_api;

    int t0 = a->thread_create(worker, (void *)0);
    int t1 = a->thread_create(worker, (void *)1);
    if (t0 < 0 || t1 < 0) {
        a->write("preempt: create FAIL\n");
        return 1;
    }
    a->thread_join(t0);
    a->thread_join(t1);

    a->write("preempt: d0="); a->print_int(deltas[0]);
    a->write(" d1="); a->print_int(deltas[1]);
    a->write("\n");

    if (deltas[0] > 0 && deltas[1] > 0) {
        a->write("PREEMPT PASS\n");
        return 0;
    }
    a->write("PREEMPT FAIL d0="); a->print_int(deltas[0]);
    a->write(" d1="); a->print_int(deltas[1]);
    a->write("\n");
    return 2;
}
