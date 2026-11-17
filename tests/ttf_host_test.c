/* ttf_host_test.c -- v2.55-ttf host-side unit test.
 * Loads the two reference fonts, runs the minimal parser and compares
 * against the fontTools ground truth in ttf_expect.h, point by point.
 *
 * build: gcc -std=c99 -I kernel_v25/include -o /tmp/ttf_host_test
 *        tests/ttf_host_test.c kernel_v25/src/ttf.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ttf.h"
#include "ttf_expect.h"

static unsigned char *load_file(const char *path, unsigned *size) {
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return NULL; }
    buf = malloc((size_t)n);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *size = (unsigned)n;
    return buf;
}

static int fails = 0;

static void check(const char *what, int cond) {
    printf("  %-38s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

static int test_font(const expect_font *ef) {
    ttf_font font;
    unsigned size = 0;
    unsigned char *buf = load_file(ef->path, &size);
    int r, i, ok_all = 1;
    char label[96];

    if (!buf) { printf("  load %s FAIL (file missing)\n", ef->path); fails++; return 1; }
    r = ttf_load(&font, buf, size);
    snprintf(label, sizeof(label), "ttf_load %.28s", strrchr(ef->path, '/') + 1);
    check(label, r == TTF_OK);
    if (r != TTF_OK) { free(buf); return 1; }

    snprintf(label, sizeof(label), "upem=%d numGlyphs=%d", ef->upem, ef->num_glyphs);
    check(label, (int)ttf_units_per_em(&font) == ef->upem && font.num_glyphs == (unsigned)ef->num_glyphs);

    for (i = 0; i < 2; i++) {
        const expect_glyph *eg = ef->g[i];
        unsigned gid = 0, npts = 0;
        int advance = 0;
        ttf_point pts[1024];
        int k, ok = 1;

        r = ttf_char_gid(&font, (i == 0) ? 0x41 : 0x67, &gid);
        snprintf(label, sizeof(label), "cmap '%c' -> gid %d", (i == 0) ? 'A' : 'g', eg->gid);
        check(label, r == TTF_OK && gid == (unsigned)eg->gid);
        if (r != TTF_OK) ok_all = 0;

        r = ttf_glyph_outline(&font, (unsigned)eg->gid, pts, 1024, &npts, &advance);
        snprintf(label, sizeof(label), "glyph %d outline r=%d npts=%d", eg->gid, r, eg->npts);
        check(label, r == TTF_OK && (int)npts == eg->npts);
        if (r != TTF_OK) { ok_all = 0; continue; }

        snprintf(label, sizeof(label), "advance %d", eg->advance);
        check(label, advance == eg->advance);

        for (k = 0; k < (int)npts; k++) {
            if (pts[k].x != eg->pts[k].x || pts[k].y != eg->pts[k].y ||
                (int)pts[k].on_curve != eg->pts[k].on_curve) {
                printf("      point %d mismatch: got (%d,%d,%d) want (%d,%d,%d)\n",
                       k, pts[k].x, pts[k].y, pts[k].on_curve,
                       eg->pts[k].x, eg->pts[k].y, eg->pts[k].on_curve);
                ok = 0;
                break;
            }
        }
        snprintf(label, sizeof(label), "points %d/%d byte-exact", npts, eg->npts);
        check(label, ok);
        if (!ok) ok_all = 0;
    }
    free(buf);
    return ok_all ? 0 : 1;
}

/* malformed-font robustness: truncated / garbage buffers must not crash */
static void test_malformed(void) {
    unsigned char junk[64];
    ttf_font font;
    memset(junk, 0xAB, sizeof(junk));
    check("garbage buffer rejected", ttf_load(&font, junk, sizeof(junk)) != TTF_OK);
    check("null buffer rejected", ttf_load(&font, NULL, 0) != TTF_OK);
    {
        /* truncated real header: copy first 200 bytes of dejavu */
        unsigned size = 0;
        unsigned char *buf = load_file(expect_dejavu.path, &size);
        if (buf) {
            unsigned small = 200;
            check("truncated font rejected", ttf_load(&font, buf, small) != TTF_OK);
            free(buf);
        }
    }
}

int main(void) {
    printf("== ttf host unit test ==\n");
    test_font(&expect_dejavu);
    test_font(&expect_liberation);
    test_malformed();
    printf("== %s ==\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
