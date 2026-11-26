/* share_b.c -- 2C block ④ dual-process shared-library probe, child B.
 *
 * Opens /bin/libmath.so, reports its tid, waits on the parent's pipe
 * barrier, then exercises the math exports and the exported `seed` variable
 * three times before closing the library.  The kernel serial log records
 * which physical RO frame this task mapped; it must match child A's frame.
 *
 * Build: sh extapp/build_ce.sh share_b.c
 * argv[1] = ready pipe handle (child -> parent, decimal).
 * argv[2] = release pipe handle (parent -> child, decimal).
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api")));
#define CE_API() (__ce_api)

typedef int (*fn2i)(int, int);

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = CE_API();
    int pr = (argc > 1) ? a->atoi(argv[1]) : -1;
    int pw = (argc > 2) ? a->atoi(argv[2]) : -1;

    int h = a->dlopen("/bin/libmath.so");
    if (h <= 0) {
        a->write("share_b: dlopen failed (");
        a->print_int(h); a->write(")\n");
        return 2;
    }

    fn2i add = (fn2i)a->dlsym(h, "add");
    fn2i sub = (fn2i)a->dlsym(h, "sub");
    fn2i mul = (fn2i)a->dlsym(h, "mul");
    fn2i gcd = (fn2i)a->dlsym(h, "gcd");
    int *seedp = (int *)a->dlsym(h, "seed");
    if (!add || !sub || !mul || !gcd || !seedp) {
        a->write("share_b: dlsym NULL\n");
        a->dlclose(h);
        return 3;
    }

    a->write("share_b: tid="); a->print_int(a->gettid());
    a->write(" seed="); a->print_int(*seedp); a->write(" ready\n");

    /* Barrier: signal the parent we are resident, then block until release. */
    char ready = 'B';
    a->pipe_write(pr, &ready, 1);
    char rel[1];
    a->pipe_read(pw, rel, 1);

    int ok = 1;
    for (int i = 0; i < 3; i++) {
        int r_add = add(7, 5);
        int r_sub = sub(7, 5);
        int r_mul = mul(7, 5);
        int r_gcd = gcd(84, 36);
        a->write("share_b["); a->print_int(i); a->write("] ");
        a->print_int(r_add); a->write(" ");
        a->print_int(r_sub); a->write(" ");
        a->print_int(r_mul); a->write(" gcd="); a->print_int(r_gcd);
        a->write("\n");
        if (r_add != 12 || r_sub != 2 || r_mul != 35 || r_gcd != 12) ok = 0;
    }

    a->dlclose(h);
    a->write("share_b: done\n");
    return ok ? 0 : 4;
}
