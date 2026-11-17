/* ttf.h - v2.55-ttf: minimal TrueType (sfnt/glyf) outline parser.
 *
 * Scope (v2.55-ttf minimal):
 *   - sfnt table directory (version 0x00010000), required tables:
 *     head / hhea / maxp / hmtx / loca / glyf / cmap
 *   - cmap format 4 (BMP range) unicode lookup
 *   - glyf simple glyph outlines (all flag variants incl. REPEAT)
 *   - glyf composite glyphs (translation + F2Dot14 scale / 2x2 matrix,
 *     depth-capped recursion)
 *   - head/hhea/hmtx metrics: unitsPerEm, advance width per gid
 *
 * Constraints: freestanding C (no kernel or libc dependency beyond
 * <stddef.h>), fixed point only (F2Dot14: value = raw / 16384), every read
 * is bounds-checked -- malformed fonts return TTF_ERR_* instead of
 * dereferencing out of range.  The caller owns all buffers.
 */
#ifndef CATOS_TTF_H
#define CATOS_TTF_H

#include <stddef.h>

#define TTF_MAX_TABLES    32
#define TTF_MAX_COMPONENT_DEPTH 8

typedef struct { int x, y; unsigned char on_curve; } ttf_point;

typedef struct {
    const unsigned char *buf;
    unsigned             size;
    /* sfnt directory */
    unsigned             num_tables;
    unsigned             tbl_off[TTF_MAX_TABLES];
    unsigned             tbl_len[TTF_MAX_TABLES];
    /* cached header fields */
    unsigned             units_per_em;   /* head.unitsPerEm            */
    int                  index_to_loc;   /* head.indexToLocFormat 0|1  */
    unsigned             num_glyphs;     /* maxp.numGlyphs             */
    unsigned             num_hmetrics;   /* hhea.numberOfHMetrics      */
    unsigned             off_hmtx;
    unsigned             off_loca;
    unsigned             off_glyf;
    /* cmap format 4 (first BMP subtable) */
    unsigned             seg_count;
    unsigned             off_cmap_end;   /* endCode[]                  */
    unsigned             off_cmap_start; /* startCode[]                */
    unsigned             off_cmap_delta; /* idDelta[]                  */
    unsigned             off_cmap_range; /* idRangeOffset[]            */
} ttf_font;

#define TTF_OK            0
#define TTF_ERR_TRUNCATED (-1)  /* font shorter than declared structures */
#define TTF_ERR_FORMAT    (-2)  /* version/encoding/segment mismatch     */
#define TTF_ERR_NOTTTF    (-3)  /* not a TrueType sfnt (glyf outlines)   */
#define TTF_ERR_NOTABLE   (-4)  /* a required table is missing           */
#define TTF_ERR_RANGE     (-5)  /* gid / code point out of font range    */
#define TTF_ERR_NOMEM     (-6)  /* caller buffer too small               */

/* Parse the sfnt directory and cache table offsets + header fields.
 * buf must remain valid for the lifetime of ttf_font. */
int ttf_load(ttf_font *f, const unsigned char *buf, unsigned size);

/* Unicode (BMP) -> glyph id via cmap format 4. */
int ttf_char_gid(const ttf_font *f, unsigned unicode, unsigned *gid);

/* Extract the full outline (composite resolved) of glyph `gid` in font
 * units.  Returns TTF_OK and fills *npts / *advance (font units), or an
 * TTF_ERR_*.  A blank glyph yields *npts = 0 and TTF_OK. */
int ttf_glyph_outline(const ttf_font *f, unsigned gid,
                      ttf_point *pts, unsigned max_pts,
                      unsigned *npts, int *advance);

/* Advance width of `gid` in font units (hmtx, last-metric clamping). */
int ttf_glyph_advance(const ttf_font *f, unsigned gid, int *advance);

unsigned ttf_units_per_em(const ttf_font *f);

#endif /* CATOS_TTF_H */
