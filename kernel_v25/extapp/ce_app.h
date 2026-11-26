/*
 * ce_app.h -- v2.49 shared CE application boilerplate (libc.a 雏形第一步).
 *
 * Before v2.49 every CE app repeated the same four-line entry preamble:
 *
 *     #include "ce_abi.h"
 *     catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;
 *
 * The single definition now lives in ce_app.c (linked next to every app by
 * build_ce.sh); apps include this header and use the ce_* inline wrappers
 * instead of hand-rolling putc/puts/strlen/strcmp/strcpy/atoi shims.
 *
 * The wrappers route to the kernel vsyscall table slots when present; the
 * freestanding fallbacks keep an app working against an older table that
 * lacks a given slot.  Slots are append-only, so a NULL slot check stays
 * cheap and optional.
 */
#ifndef CE_APP_H
#define CE_APP_H

#include "ce_abi.h"

/* Defined once in ce_app.c; the kernel fills the pointer before entry. */
extern catos_api_t *__ce_api;

/* ---- per-thread TLS errno --------------------------------------------
 * The kernel loads the ring3 %gs data descriptor (GDT index 6) to the thread's
 * private TLS user page on every switch.  TLS words:
 *   [0] = tid   [4] = errno   [8] = canary 0xC0FFEE   [12] = self base VA
 * Ring3 cannot read the GS_BASE MSR (rdmsr is ring0 only), so the TLS page
 * carries its own base at offset 12; read it and return the errno slot.
 * Valid only on threads that own a TLS page (worker threads); the main
 * process thread currently runs without one -- do not deref in ce_main. */
static inline int *__tls_errno(void) {
    uint32_t base;
    __asm__ __volatile__("movl %%gs:12, %0" : "=r"(base));
    return (int *)(base + 4);
}

/* Current API table shorthand (kept for apps that still write api->...). */
static inline catos_api_t *ce_api(void) { return __ce_api; }

/* ---- console ---------------------------------------------------------- */
static inline void ce_write(const char *s) { __ce_api->write(s); }
static inline void ce_puts(const char *s)  { __ce_api->write(s); }
static inline void ce_putc(char c)        { __ce_api->putc(c); }
static inline void ce_cls(void)           { __ce_api->cls(); }

/* ---- strings (vsyscall slot first, freestanding fallback second) ----- */
static inline int ce_strlen(const char *s) {
    if (__ce_api->strlen) return __ce_api->strlen(s);
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int ce_strcmp(const char *a, const char *b) {
    if (__ce_api->strcmp) return __ce_api->strcmp(a, b);
    while (*a && *a == *b) { a++; b++; }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

static inline char *ce_strcpy(char *d, const char *s) {
    if (__ce_api->strcpy) { __ce_api->strcpy(d, s); return d; }
    char *o = d;
    while ((*d++ = *s++)) ;
    return o;
}

static inline int ce_atoi(const char *s) {
    if (__ce_api->atoi) return __ce_api->atoi(s);
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

#endif /* CE_APP_H */
