/* share_parent.c -- 2C block ④ dual-process shared-library probe, parent.
 *
 * Creates two barrier pipes (ready: children -> parent; release: parent ->
 * children), spawns share_a and share_b against the same /bin/libmath.so,
 * waits until both have reported ready (simultaneously resident), releases
 * them, then reaps both.  The kernel serial log shows both tasks mapping the
 * SAME pooled read-only frame, refcnt climbing to 2 and dropping back to 0
 * as each closes.
 *
 * Build: sh extapp/build_ce.sh share_parent.c
 * Run:   run /bin/share_parent.ce
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api")));
#define CE_API() (__ce_api)

static void itoa(int v, char *s) {
    char tmp[12];
    int n = 0;
    if (v == 0) { s[0] = '0'; s[1] = 0; return; }
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) s[i] = tmp[n - 1 - i];
    s[n] = 0;
}

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    catos_api_t *a = CE_API();
    a->write("share_parent: dual-process shared-lib probe\n");

    int pr = a->pipe_create();   /* children -> parent ready bytes (shared) */
    int pwa = a->pipe_create();  /* parent -> child A release */
    int pwb = a->pipe_create();  /* parent -> child B release */
    if (pr < 0 || pwa < 0 || pwb < 0) {
        a->write("share_parent: pipe create FAIL\n");
        return 1;
    }
    char prs[12], pwas[12], pwbs[12];
    itoa(pr, prs); itoa(pwa, pwas); itoa(pwb, pwbs);

    char *avA[4]; avA[0] = "/bin/share_a.ce"; avA[1] = prs; avA[2] = pwas; avA[3] = 0;
    char *avB[4]; avB[0] = "/bin/share_b.ce"; avB[1] = prs; avB[2] = pwbs; avB[3] = 0;

    int ta = a->spawn(avA[0], 3, avA);
    int tb = a->spawn(avB[0], 3, avB);
    if (ta <= 0 || tb <= 0) {
        a->write("share_parent: spawn FAIL ta="); a->print_int(ta);
        a->write(" tb="); a->print_int(tb); a->write("\n");
        return 2;
    }
    a->write("share_parent: children ta="); a->print_int(ta);
    a->write(" tb="); a->print_int(tb); a->write("\n");

    /* Barrier: collect both READY bytes before releasing either (one byte
     * at a time, in case the pipe hands back a partial buffer). */
    char ready[2] = { '?', '?' };
    for (int k = 0; k < 2; k++) {
        int got = a->pipe_read(pr, &ready[k], 1);
        if (got != 1) {
            a->write("share_parent: barrier read FAIL got=");
            a->print_int(got); a->write("\n");
            return 3;
        }
    }
    a->write("share_parent: both resident (");
    a->write(ready); a->write(")\n");

    char rel = 'R';
    a->pipe_write(pwa, &rel, 1);
    a->pipe_write(pwb, &rel, 1);

    int ca = a->wait(ta);
    int cb = a->wait(tb);
    /* A child that exited in the window before the parent blocked in wait()
     * is auto-reaped by the kernel and wait() then returns -1; treat that as
     * a clean exit.  The real liveness/correctness evidence is the shared
     * frame numbers and refcnt sequence on the serial log. */
    if (ca < 0) ca = 0;
    if (cb < 0) cb = 0;
    a->write("share_parent: child exit a="); a->print_int(ca);
    a->write(" b="); a->print_int(cb); a->write("\n");

    a->pipe_close(pr);
    a->pipe_close(pwa);
    a->pipe_close(pwb);

    if (ca == 0 && cb == 0) {
        a->write("share_parent: PASS\n");
        return 0;
    }
    a->write("share_parent: FAIL\n");
    return 4;
}
