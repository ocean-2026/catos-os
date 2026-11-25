/* locale.c -- Stage 3 block 4: minimal locale round-trip probe.
 *
 * Exercises the two new syscall slots:
 *   slot 94 setlocale(category, name)
 *   slot 95 locale_query(category, what, buf, len)
 *
 * Flow:
 *   1. default C locale: currency "$", month-1 "Jan", wday-0 "Sun".
 *   2. setlocale(LC_ALL, "zh_CN.UTF-8") -> 0; name "zh_CN.UTF-8", month/wday
 *      come back as non-ASCII (UTF-8 CJK), currency non-ASCII.
 *   3. setlocale(LC_COLLATE, "zh") must be rejected (negative; unsupported).
 *   4. setlocale with an unknown name must be rejected (negative).
 *   5. switch back to C and re-check English month.
 *
 * Build: sh extapp/build_ce.sh locale.c
 * Run:   run /bin/locale.ce
 */
#include "ce_app.h"

static int sstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *a = __ce_api;
    char b[32];
    int fail = 0;

    /* 1. default C locale. */
    a->locale_query(LC_MONETARY_CAT, LQ_CURRENCY_Q, b, sizeof(b));
    if (b[0] != '$' || b[1] != 0) { fail = 1; a->write("locale: C currency bad\n"); }

    a->locale_query(LC_TIME_CAT, LQ_MONTH_Q + 1, b, sizeof(b));
    if (sstrcmp(b, "Jan") != 0) { fail = 1; a->write("locale: C month bad\n"); }

    a->locale_query(LC_TIME_CAT, LQ_WDAY_Q + 0, b, sizeof(b));
    if (sstrcmp(b, "Sun") != 0) { fail = 1; a->write("locale: C wday bad\n"); }

    /* 2. switch to zh. */
    int rc = a->setlocale(LC_ALL_CAT, "zh_CN.UTF-8");
    if (rc != 0) {
        fail = 1; a->write("locale: setlocale zh rc="); a->print_int(rc); a->write("\n");
    }

    a->locale_query(LC_ALL_CAT, LQ_NAME_Q, b, sizeof(b));
    if (sstrcmp(b, "zh_CN.UTF-8") != 0) { fail = 1; a->write("locale: zh name bad\n"); }

    a->locale_query(LC_TIME_CAT, LQ_MONTH_Q + 1, b, sizeof(b));
    if ((unsigned char)b[0] < 0x80) { fail = 1; a->write("locale: zh month ascii\n"); }

    a->locale_query(LC_TIME_CAT, LQ_WDAY_Q + 0, b, sizeof(b));
    if ((unsigned char)b[0] < 0x80) { fail = 1; a->write("locale: zh wday ascii\n"); }

    a->locale_query(LC_MONETARY_CAT, LQ_CURRENCY_Q, b, sizeof(b));
    if ((unsigned char)b[0] < 0x80) { fail = 1; a->write("locale: zh currency ascii\n"); }

    /* 3. LC_COLLATE must be explicitly rejected. */
    rc = a->setlocale(LC_COLLATE_CAT, "zh_CN.UTF-8");
    if (rc >= 0) { fail = 1; a->write("locale: collate not rejected rc=");
                   a->print_int(rc); a->write("\n"); }

    /* 4. unknown locale name rejected. */
    rc = a->setlocale(LC_NUMERIC_CAT, "xx_XX.FOO");
    if (rc >= 0) { fail = 1; a->write("locale: unknown not rejected rc=");
                   a->print_int(rc); a->write("\n"); }

    /* 5. back to C. */
    rc = a->setlocale(LC_ALL_CAT, "C");
    if (rc != 0) { fail = 1; a->write("locale: setlocale C rc="); a->print_int(rc); a->write("\n"); }
    a->locale_query(LC_TIME_CAT, LQ_MONTH_Q + 12, b, sizeof(b));
    if (sstrcmp(b, "Dec") != 0) { fail = 1; a->write("locale: C month12 bad\n"); }

    if (fail) { a->write("LOCALE FAIL\n"); return 2; }
    a->write("LOCALE PASS\n");
    return 0;
}
