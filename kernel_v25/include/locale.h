#ifndef LOCALE_H
#define LOCALE_H

/* locale.h -- Stage 3 block 4: minimal in-memory locale state.
 *
 * Only the categories that have a real table behind them are wired this slice:
 *   LC_CTYPE   : UTF-8<->GBK conversion table (stub; full table deferred)
 *   LC_NUMERIC : decimal point / thousands separator / grouping
 *   LC_TIME    : localized month / weekday abbreviations
 *   LC_MONETARY: currency symbol / fraction digits
 *   LC_COLLATE  : explicitly NOT supported (setlocale returns -22)
 *
 * Tables live in memory only; an on-disk mmap data table is deferred.  The
 * kernel has no libc printf/strftime yet, so the tables are exposed through
 * query syscalls for CE probes/apps to consume; formatter integration is a
 * follow-up. */

#include <stdint.h>

#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_MONETARY 3
#define LC_COLLATE  4
#define LC_ALL      5
#define LC_COUNT    6

/* locale ids (internal) */
#define LOC_C   0    /* default / en_US.UTF-8 */
#define LOC_ZH  1    /* zh_CN.UTF-8 */

/* Query selectors for locale_query(). */
#define LQ_NAME        0   /* current locale name for the category        */
#define LQ_DECIMAL    1   /* LC_NUMERIC decimal point                    */
#define LQ_THOUSANDS  2   /* LC_NUMERIC thousands separator              */
#define LQ_CURRENCY   3   /* LC_MONETARY currency symbol                 */
/* month abbrev: LQ_MONTH + month(1..12); weekday: LQ_WDAY + wday(0..6) */
#define LQ_MONTH      100
#define LQ_WDAY       200

void        locale_init(void);
/* 0 = ok; -22 = bad category; -22 for LC_COLLATE (unsupported); -1 = unknown
 * locale name.  LC_ALL sets every supported category at once. */
int         locale_set(int category, const char *name);
/* Copy the requested locale string into buf (NUL-terminated, capped at len).
 * Returns 0 ok, -22 bad category/selector. */
int         locale_query(int category, int what, char *buf, int len);
/* Direct accessors (kernel side), never NULL. */
int         locale_id(int category);
const char *locale_decimal_point(int category);
const char *locale_thousands_sep(int category);
const char *locale_currency_symbol(int category);
void        locale_month_abbrev(int category, int mon, char *out, int len);
void        locale_wday_abbrev(int category, int wday, char *out, int len);

#endif /* LOCALE_H */
