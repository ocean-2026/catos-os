/*
 * clip.c -- v2.57 kernel-internal UTF-8 clipboard (see clip.h).
 */
#include "clip.h"
#include <stdint.h>

static uint8_t clip_buf[CLIP_CAP];
static int     clip_len_v;

void clip_clear(void) {
    clip_len_v = 0;
    clip_buf[0] = 0;
}

int clip_empty(void) { return clip_len_v == 0; }
int clip_len(void)   { return clip_len_v; }
const char *clip_get(void) { return (const char *)clip_buf; }

void clip_set(const uint8_t *data, int len) {
    if (!data || len <= 0) { clip_clear(); return; }
    if (len > CLIP_CAP - 1) len = CLIP_CAP - 1;

    /* Trim a dangling trailing UTF-8 continuation sequence: a lead byte
     * with no followers must not survive in the buffer. */
    int n = len;
    while (n > 0 && (data[n - 1] & 0xC0u) == 0x80u) n--;
    if (n > 0 && (data[n - 1] & 0x80u) != 0) {
        /* lead byte left without its followers: back it out too */
        while (n > 0 && (data[n - 1] & 0xC0u) == 0x80u) n--;
        if (n > 0 && (data[n - 1] & 0x80u) != 0) n--;
    }
    if (n <= 0) { clip_clear(); return; }

    for (int i = 0; i < n; i++) clip_buf[i] = data[i];
    clip_buf[n] = 0;
    clip_len_v = n;
}
