/* mmaptest.c -- CE regression for the v2.10 mmap/munmap ABI.
 *
 *   p = mmap(0, 8192, PROT_RW=1, 0)   -> non-NULL, 8 KiB guard behind it
 *   write/read roundtrip on every page
 *   munmap(p, 8192) == 0
 *   q = mmap(...) again -> same address (first-fit reuses the hole)
 *   munmap on a bogus range must fail (-22)
 * prints PASS / FAIL lines; keep every string ASCII so it runs anywhere.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static void putline(const char *s) { __ce_api->write(s); }

int ce_main(void) {
    int fails = 0;

    unsigned char *p = (unsigned char *)__ce_api->mmap(0, 8192, 1, 0);
    if (p == 0) { putline("FAIL mmap returned NULL\n"); return 1; }
    putline("ok   mmap 8k\n");

    /* touch both pages (lazy mapping), write a pattern, read it back */
    for (int i = 0; i < 4096; i++) p[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < 4096; i++) {
        if (p[i] != (unsigned char)(i * 7 + 1)) {
            putline("FAIL pattern page0\n"); fails++; break;
        }
    }
    if (!fails) putline("ok   rw pattern page0\n");
    for (int i = 0; i < 4096; i++) p[4096 + i] = (unsigned char)(i ^ 0x5A);
    int ok2 = 1;
    for (int i = 0; i < 4096; i++)
        if (p[4096 + i] != (unsigned char)(i ^ 0x5A)) { ok2 = 0; break; }
    if (ok2) putline("ok   rw pattern page1\n");
    else { putline("FAIL pattern page1\n"); fails++; }

    int r = __ce_api->munmap(p, 8192);
    if (r == 0) putline("ok   munmap\n");
    else { putline("FAIL munmap\n"); fails++; }

    unsigned char *q = (unsigned char *)__ce_api->mmap(0, 8192, 1, 0);
    if (q == p) putline("ok   re-mmap reuses hole\n");
    else { putline("FAIL re-mmap address\n"); fails++; }

    int bad = __ce_api->munmap((void *)0x12345678u, 4096);
    if (bad < 0) putline("ok   munmap rejects bogus\n");
    else { putline("FAIL munmap bogus accepted\n"); fails++; }

    if (fails == 0) { putline("MMAPTEST PASS\n"); return 0; }
    putline("MMAPTEST FAIL\n");
    return 1;
}
