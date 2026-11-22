/* stubs.c - minimal runtime stubs for freestanding i386 builds */
#include "../include/kstring.h"
/* These replace compiler-inserted runtime calls that we don't need. */

/* Stack check stub: GCC may insert __chkstk_ms for large stack frames.
 * Our kernels don't use dynamic stack probing, so this is a no-op. */
void __chkstk_ms(void) { }

/* Similarly for any other missing runtime symbols */
void __main(void) { }

/* Freestanding memory primitives referenced by the vendored TLS crypto
 * (micro-ecc / tiny-AES-c) and by tls.c.  Lengths are plain ints here. */
void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return *x < *y ? -1 : 1;
        x++; y++;
    }
    return 0;
}

void *memmove(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
