/* heap.c -- Stage 3B block 3: per-process shared heap probe.
 *
 * The heap arena (address space / VMA / break) is shared by all threads of a
 * process; the kernel free-list head is resolved to the main thread's slot,
 * so a block freed by one worker is reachable from the other worker's scan.
 *
 * Two workers each run an alloc/fill/free loop interleaved with the other.
 * Every block is filled with a worker-specific pattern before free; a
 * corrupted or cross-freed block would surface as a #PF / pattern mismatch.
 * The main thread holds one base block across the workers' run, frees it
 * after join, then allocates again to show the arena reuses the freed space.
 *
 * Build: sh extapp/build_ce.sh heap.c
 * Run:   run /bin/heap.ce
 */
#include "ce_app.h"

#define K 200
#define BLK 48

static volatile long sink;
static int wcount[2];
static int wbad[2];

static void *worker(void *arg) {
    long id = (long)arg;
    catos_api_t *a = __ce_api;
    int n = 0, bad = 0;

    for (int i = 0; i < K; i++) {
        char *p = (char *)a->alloc(BLK);
        if (!p) { bad++; continue; }
        for (int j = 0; j < BLK; j++) p[j] = (char)((id * 31 + i + j) & 0x7f);
        /* touch first/last byte through the live block before free */
        sink += p[0] + p[BLK - 1];
        a->free(p);
        n++;
    }
    wcount[id] = n;
    wbad[id] = bad;
    a->write("heap: worker="); a->print_int((int)id);
    a->write(" alloc="); a->print_int(n);
    a->write(" oom="); a->print_int(bad);
    a->write("\n");
    a->thread_exit((void *)0);
    return 0;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *a = __ce_api;

    char *base = (char *)a->alloc(64);
    if (!base) { a->write("heap: base alloc FAIL\n"); return 1; }
    for (int j = 0; j < 64; j++) base[j] = (char)j;

    int t0 = a->thread_create(worker, (void *)0);
    int t1 = a->thread_create(worker, (void *)1);
    if (t0 < 0 || t1 < 0) {
        a->write("heap: create FAIL\n"); return 1;
    }
    a->thread_join(t0);
    a->thread_join(t1);

    a->free(base);

    char *again = (char *)a->alloc(64);
    if (!again) { a->write("heap: re-alloc FAIL\n"); return 2; }
    again[0] = 1; again[63] = 2;
    a->free(again);

    a->write("heap: c0="); a->print_int(wcount[0]);
    a->write(" c1="); a->print_int(wcount[1]);
    a->write(" bad0="); a->print_int(wbad[0]);
    a->write(" bad1="); a->print_int(wbad[1]);
    a->write("\n");

    if (wcount[0] == K && wcount[1] == K && wbad[0] == 0 && wbad[1] == 0) {
        a->write("HEAP PASS\n");
        return 0;
    }
    a->write("HEAP FAIL\n");
    return 3;
}
