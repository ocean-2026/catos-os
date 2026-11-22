#ifndef KSTRING_H
#define KSTRING_H

/*
 * Freestanding memory helpers.
 *
 * The kernel links -ffreestanding and has no libc, so a handful of the
 * usual <string.h> primitives are implemented in src/stubs.c.  Declare
 * them here so callers do not depend on implicit declarations.
 */

void *memcpy(void *dst, const void *src, unsigned int n);
void *memset(void *dst, int c, unsigned int n);
void *memmove(void *dst, const void *src, unsigned int n);

#endif
