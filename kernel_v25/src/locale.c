/* locale.c -- Stage 3 block 4: minimal in-memory locale tables (see locale.h). */

#include <stdint.h>
#include <stddef.h>
#include "locale.h"

static int cur[LC_COUNT];          /* LOC_C / LOC_ZH per category */

static int lstrlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int lstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void lcopy(char *d, int len, const char *s) {
    int i = 0;
    while (i < len - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* ---- tables ------------------------------------------------------------- */

static const char *const mon_en[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const mon_zh[12] = {
    "一月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "十一月", "十二月"
};
static const char *const wday_en[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const wday_zh[7] = {
    "日", "一", "二", "三", "四", "五", "六"
};

static int parse_locale(const char *name) {
    if (!name || !*name) return -1;
    if (!lstrcmp(name, "C") || !lstrcmp(name, "POSIX") ||
        !lstrcmp(name, "en") || !lstrcmp(name, "en_US.UTF-8") ||
        !lstrcmp(name, "en_US"))
        return LOC_C;
    if (!lstrcmp(name, "zh") || !lstrcmp(name, "zh_CN") ||
        !lstrcmp(name, "zh_CN.UTF-8") || !lstrcmp(name, "zh.UTF-8"))
        return LOC_ZH;
    return -1;
}

void locale_init(void) {
    for (int i = 0; i < LC_COUNT; i++) cur[i] = LOC_C;
}

int locale_set(int category, const char *name) {
    if (category < 0 || category > LC_ALL) return -22;
    if (category == LC_COLLATE) return -22;      /* explicitly unsupported */
    int id = parse_locale(name);
    if (id < 0) return -1;
    if (category == LC_ALL) {
        for (int i = 0; i < LC_COUNT; i++)
            if (i != LC_COLLATE) cur[i] = id;
    } else {
        cur[category] = id;
    }
    return 0;
}

int locale_id(int category) {
    if (category < 0 || category >= LC_COUNT) return LOC_C;
    return cur[category];
}

const char *locale_decimal_point(int category) {
    (void)category;
    return ".";                 /* both locales use '.' for digits this slice */
}

const char *locale_thousands_sep(int category) {
    (void)category;
    return ",";
}

const char *locale_currency_symbol(int category) {
    return locale_id(category) == LOC_ZH ? "¥" : "$";
}

void locale_month_abbrev(int category, int mon, char *out, int len) {
    if (!out || len <= 0) return;
    if (mon < 1 || mon > 12) { lcopy(out, len, "?"); return; }
    const char *const *tbl = locale_id(category) == LOC_ZH ? mon_zh : mon_en;
    lcopy(out, len, tbl[mon - 1]);
}

void locale_wday_abbrev(int category, int wday, char *out, int len) {
    if (!out || len <= 0) return;
    if (wday < 0 || wday > 6) { lcopy(out, len, "?"); return; }
    const char *const *tbl = locale_id(category) == LOC_ZH ? wday_zh : wday_en;
    lcopy(out, len, tbl[wday]);
}

int locale_query(int category, int what, char *buf, int len) {
    if (!buf || len <= 0) return -22;
    if (category < 0 || category > LC_ALL) return -22;
    int cat = (category == LC_ALL) ? LC_NUMERIC : category;

    switch (what) {
    case LQ_NAME: {
        const char *n = locale_id(cat) == LOC_ZH ? "zh_CN.UTF-8" : "C";
        lcopy(buf, len, n);
        return 0;
    }
    case LQ_DECIMAL:
        lcopy(buf, len, locale_decimal_point(cat)); return 0;
    case LQ_THOUSANDS:
        lcopy(buf, len, locale_thousands_sep(cat)); return 0;
    case LQ_CURRENCY:
        lcopy(buf, len, locale_currency_symbol(cat)); return 0;
    default:
        if (what >= LQ_MONTH + 1 && what <= LQ_MONTH + 12) {
            locale_month_abbrev(cat, what - LQ_MONTH, buf, len); return 0;
        }
        if (what >= LQ_WDAY && what < LQ_WDAY + 7) {
            locale_wday_abbrev(cat, what - LQ_WDAY, buf, len); return 0;
        }
        return -22;
    }
}
