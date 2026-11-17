/* ttf.c - v2.55-ttf minimal TrueType outline parser.  See ttf.h for the
 * contract.  All multi-byte fields are big-endian; every access is bounds
 * checked against f->size (malformed input returns errors, never reads
 * out of range).  Fixed point only: F2Dot14 scale = raw / 16384.
 */
#include "ttf.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef short i16;
typedef unsigned u32;

/* ---- bounded readers --------------------------------------------------- */
static int in_range(const ttf_font *f, unsigned off, unsigned len) {
    return off <= f->size && len <= f->size - off;
}

static u32 rd16(const ttf_font *f, unsigned off, int *err) {
    if (!in_range(f, off, 2)) { *err = TTF_ERR_TRUNCATED; return 0; }
    return ((u32)f->buf[off] << 8) | f->buf[off + 1];
}

static u32 rd32(const ttf_font *f, unsigned off, int *err) {
    if (!in_range(f, off, 4)) { *err = TTF_ERR_TRUNCATED; return 0; }
    return ((u32)f->buf[off] << 24) | ((u32)f->buf[off + 1] << 16) |
           ((u32)f->buf[off + 2] << 8) | f->buf[off + 3];
}

static i16 srd16(const ttf_font *f, unsigned off, int *err) {
    return (i16)rd16(f, off, err);
}

/* byte at `off` (rd16's HIGH byte, not its low byte) */
static u32 rd8(const ttf_font *f, unsigned off, int *err) {
    if (!in_range(f, off, 1)) { *err = TTF_ERR_TRUNCATED; return 0; }
    return f->buf[off];
}

/* ---- sfnt directory ---------------------------------------------------- */
static int table_find(const ttf_font *f, const char tag[4],
                      unsigned *off, unsigned *len) {
    unsigned i;
    for (i = 0; i < f->num_tables; i++) {
        unsigned to = 12 + i * 16;
        if (!in_range(f, to, 16)) return TTF_ERR_TRUNCATED;
        if (f->buf[to] == (u8)tag[0] && f->buf[to + 1] == (u8)tag[1] &&
            f->buf[to + 2] == (u8)tag[2] && f->buf[to + 3] == (u8)tag[3]) {
            *off = f->tbl_off[i];
            *len = f->tbl_len[i];
            return TTF_OK;
        }
    }
    return TTF_ERR_NOTABLE;
}

int ttf_load(ttf_font *f, const unsigned char *buf, unsigned size) {
    int err = TTF_OK;
    unsigned ver, num, i, off, need;
    static const char *req[] = { "head", "hhea", "maxp", "hmtx",
                                 "loca", "glyf", "cmap" };
    unsigned roff[7], rlen[7];
    unsigned k;

    if (!f || !buf || size < 12) return TTF_ERR_NOTTTF;
    f->buf = buf; f->size = size;
    f->seg_count = 0; f->off_cmap_end = 0; f->off_hmtx = 0;
    f->off_loca = 0; f->off_glyf = 0;

    ver = rd32(f, 0, &err);
    if (err) return err;
    /* 0x00010000 TrueType outlines; 0x74727565 'true' also legal sfnt */
    if (ver != 0x00010000u && ver != 0x74727565u) return TTF_ERR_NOTTTF;

    num = rd16(f, 4, &err);           if (err) return err;
    if (num == 0 || num > TTF_MAX_TABLES) return TTF_ERR_FORMAT;
    need = 12 + num * 16;
    if (need > size) return TTF_ERR_TRUNCATED;

    f->num_tables = num;
    for (i = 0; i < num; i++) {
        unsigned to = 12 + i * 16;
        f->tbl_off[i] = rd32(f, to + 8, &err);   if (err) return err;
        f->tbl_len[i] = rd32(f, to + 12, &err);  if (err) return err;
        /* table range must lie inside the buffer */
        if (f->tbl_off[i] > size || f->tbl_len[i] > size - f->tbl_off[i])
            return TTF_ERR_TRUNCATED;
    }

    for (k = 0; k < 7; k++) {
        int r = table_find(f, req[k], &roff[k], &rlen[k]);
        if (r) return r;
    }

    /* head: unitsPerEm @18, indexToLocFormat @50 */
    off = roff[0];
    if (!in_range(f, off, 54)) return TTF_ERR_TRUNCATED;
    f->units_per_em = rd16(f, off + 18, &err);   if (err) return err;
    if (f->units_per_em == 0) return TTF_ERR_FORMAT;
    f->index_to_loc = srd16(f, off + 50, &err);  if (err) return err;
    if (f->index_to_loc != 0 && f->index_to_loc != 1) return TTF_ERR_FORMAT;

    /* hhea: numberOfHMetrics @34 */
    off = roff[1];
    if (!in_range(f, off, 36)) return TTF_ERR_TRUNCATED;
    f->num_hmetrics = rd16(f, off + 34, &err);   if (err) return err;

    /* maxp: numGlyphs @4 */
    off = roff[2];
    if (!in_range(f, off, 6)) return TTF_ERR_TRUNCATED;
    f->num_glyphs = rd16(f, off + 4, &err);      if (err) return err;
    if (f->num_glyphs == 0 || f->num_glyphs > 65535) return TTF_ERR_FORMAT;

    f->off_hmtx = roff[3];
    f->off_loca = roff[4];
    f->off_glyf = roff[5];

    /* loca entry count = numGlyphs + 1; sanity: entries fit in table */
    {
        unsigned ent = (f->index_to_loc == 0) ? 2 : 4;
        if ((unsigned long)(f->num_glyphs + 1) * ent < (unsigned long)(f->num_glyphs + 1))
            return TTF_ERR_FORMAT; /* overflow guard (paranoia) */
        if ((f->num_glyphs + 1) * ent > rlen[4]) return TTF_ERR_TRUNCATED;
    }

    /* cmap: find a format 4 subtable (platform 3 encoding 1 preferred) */
    {
        unsigned co = roff[6];
        unsigned ntab, si;
        if (!in_range(f, co, 4)) return TTF_ERR_TRUNCATED;
        ntab = rd16(f, co + 2, &err);            if (err) return err;
        if (ntab == 0 || ntab > 64) return TTF_ERR_FORMAT;
        for (si = 0; si < ntab; si++) {
            unsigned se = co + 4 + si * 8;
            unsigned sub, fmt, len, seg;
            if (!in_range(f, se, 8)) return TTF_ERR_TRUNCATED;
            sub = co + rd32(f, se + 4, &err);    /* relative to cmap start */
            if (err) return err;
            if (sub > size || 8 > size - sub) continue;
            fmt = rd16(f, sub, &err);            if (err) return err;
            if (fmt != 4) continue;
            len = rd16(f, sub + 2, &err);        if (err) return err;
            if (len < 16 || sub + len > size) return TTF_ERR_TRUNCATED;
            seg = rd16(f, sub + 6, &err);        if (err) return err;
            if (seg == 0 || (seg & 1)) return TTF_ERR_FORMAT;
            f->seg_count      = seg / 2;
            f->off_cmap_end   = sub + 14;
            f->off_cmap_start = f->off_cmap_end + seg + 2;
            f->off_cmap_delta = f->off_cmap_start + seg;
            f->off_cmap_range = f->off_cmap_delta + seg;
            /* endCode[n] + reserved + startCode[n] + idDelta[n] + idRangeOffset[n] */
            if (f->off_cmap_range + seg > size) return TTF_ERR_TRUNCATED;
            break;
        }
        if (f->seg_count == 0) return TTF_ERR_FORMAT; /* no BMP format 4 */
    }
    return TTF_OK;
}

/* ---- cmap format 4 lookup ---------------------------------------------- */
int ttf_char_gid(const ttf_font *f, unsigned unicode, unsigned *gid) {
    int err = TTF_OK;
    unsigned i;
    if (!f || f->seg_count == 0) return TTF_ERR_FORMAT;
    if (unicode > 0xFFFF) return TTF_ERR_RANGE;
    for (i = 0; i < f->seg_count; i++) {
        u32 end = rd16(f, f->off_cmap_end + i * 2, &err);
        if (err) return err;
        if (unicode > end) continue;
        {
            u32 start = rd16(f, f->off_cmap_start + i * 2, &err);
            if (err) return err;
            if (unicode < start) return TTF_ERR_RANGE; /* sorted segments */
            {
                u32 delta = rd16(f, f->off_cmap_delta + i * 2, &err);
                u32 roff = rd16(f, f->off_cmap_range + i * 2, &err);
                if (err) return err;
                if (roff == 0) {
                    *gid = (unsigned)((int)unicode + (i16)delta) & 0xFFFF;
                    return (*gid == 0) ? TTF_ERR_RANGE : TTF_OK;
                }
                {
                    unsigned addr = f->off_cmap_range + i * 2 + roff +
                                    (unicode - start) * 2;
                    u32 g = rd16(f, addr, &err);
                    if (err) return err;
                    if (g == 0) return TTF_ERR_RANGE;
                    *gid = (unsigned)((int)g + (i16)delta) & 0xFFFF;
                    return (*gid == 0) ? TTF_ERR_RANGE : TTF_OK;
                }
            }
        }
    }
    return TTF_ERR_RANGE;
}

/* ---- hmtx metrics ------------------------------------------------------ */
int ttf_glyph_advance(const ttf_font *f, unsigned gid, int *advance) {
    int err = TTF_OK;
    unsigned idx, off;
    if (!f || gid >= f->num_glyphs) return TTF_ERR_RANGE;
    idx = (gid < f->num_hmetrics) ? gid : (f->num_hmetrics ? f->num_hmetrics - 1 : 0);
    if (f->num_hmetrics == 0) return TTF_ERR_FORMAT;
    off = f->off_hmtx + idx * 4;
    if (!in_range(f, off, 2)) return TTF_ERR_TRUNCATED;
    *advance = (int)rd16(f, off, &err);
    return err;
}

/* ---- glyf: simple glyph contour extraction ----------------------------- */
/* Flags: C01 ON_CURVE, C02 X_SHORT, C04 Y_SHORT, C08 REPEAT,
 *        C10 X_SAME/POS, C20 Y_SAME/POS */
#define F_ON   0x01
#define F_XS   0x02
#define F_YS   0x04
#define F_REP  0x08
#define F_XSAMED 0x10
#define F_YSAMED 0x20

static int read_simple(const ttf_font *f, unsigned goff, unsigned gsize,
                       ttf_point *pts, unsigned max_pts, unsigned *npts) {
    int err = TTF_OK;
    unsigned nc, p = 0, npts_total = 0;
    unsigned end_off, flags_off, xoff;
    i16 ncon;

    if (gsize < 10) return TTF_ERR_TRUNCATED;
    ncon = srd16(f, goff, &err);            if (err) return err;
    if (ncon <= 0) { *npts = 0; return TTF_OK; } /* empty or composite */
    nc = (unsigned)ncon;
    if (nc > 256) return TTF_ERR_FORMAT;    /* sanity cap */

    end_off = goff + 10 + nc * 2;
    if (!in_range(f, end_off, 2)) return TTF_ERR_TRUNCATED;
    {
        unsigned ins_len = rd16(f, end_off, &err);
        if (err) return err;
        flags_off = end_off + 2 + ins_len;
    }

    /* total points = last end point + 1 */
    {
        unsigned last = rd16(f, end_off - 2, &err);
        if (err) return err;
        npts_total = last + 1;
        if (npts_total == 0 || npts_total > 512) return TTF_ERR_FORMAT;
        if (npts_total > max_pts) return TTF_ERR_NOMEM;
    }

    /* flags with REPEAT expansion */
    {
        u8 fl[512];
        unsigned fi = 0;
        p = 0;
        while (fi < npts_total) {   /* fi = expanded point count, not bytes */
            u32 b = rd8(f, flags_off + p, &err);
            if (err) return err;
            fl[fi++] = (u8)b;
            p++;
            if (b & F_REP) {
                u32 rep = rd8(f, flags_off + p, &err);
                if (err) return err;
                p++;
                if (fi + rep > npts_total) return TTF_ERR_FORMAT;
                while (rep--) fl[fi++] = (u8)b;
            }
            if (p > gsize) return TTF_ERR_TRUNCATED;
        }

        /* x deltas */
        xoff = flags_off + p;
        {
            int x = 0;
            unsigned q;
            for (q = 0; q < npts_total; q++) {
                u32 b = fl[q];
                if (b & F_XS) {
                    u32 d = rd8(f, xoff, &err);
                    if (err) return err;
                    x += (b & F_XSAMED) ? (int)d : -(int)d;
                    xoff += 1;
                } else if (b & F_XSAMED) {
                    /* X_SAME without X_SHORT: delta is zero, 0 bytes */
                } else {
                    u32 d = rd16(f, xoff, &err);
                    if (err) return err;
                    x += (int)(i16)d;
                    xoff += 2;
                }
                if (xoff > f->size) return TTF_ERR_TRUNCATED;
                pts[q].x = x;
            }
            /* y deltas */
            {
                int y = 0;
                for (q = 0; q < npts_total; q++) {
                    u32 b = fl[q];
                    if (b & F_YS) {
                        u32 d = rd8(f, xoff, &err);
                        if (err) return err;
                        y += (b & F_YSAMED) ? (int)d : -(int)d;
                        xoff += 1;
                    } else if (b & F_YSAMED) {
                        /* Y_SAME without Y_SHORT: delta is zero, 0 bytes */
                    } else {
                        u32 d = rd16(f, xoff, &err);
                        if (err) return err;
                        y += (int)(i16)d;
                        xoff += 2;
                    }
                    if (xoff > f->size) return TTF_ERR_TRUNCATED;
                    pts[q].y = y;
                    pts[q].on_curve = (b & F_ON) ? 1 : 0;
                }
            }
        }
    }
    *npts = npts_total;
    return TTF_OK;
}

/* ---- glyf: composite resolution ---------------------------------------- */
#define CF_WORDS      0x0001u
#define CF_XY_VALUES  0x0002u
#define CF_MORE       0x0020u
#define CF_SCALE      0x0040u
#define CF_XY_SCALE   0x0080u
#define CF_2X2        0x0100u

/* F2Dot14 multiply: a * (b/16384), rounding toward nearest */
static int fmul(int a, int b) {
    int v = (int)(((long)a * b + 8192) >> 14);
    return v;
}

static int read_glyph(const ttf_font *f, unsigned gid,
                      ttf_point *pts, unsigned max_pts, unsigned *npts,
                      int dx, int dy, int m00, int m01, int m10, int m11,
                      int depth);

static int read_composite(const ttf_font *f, unsigned goff, unsigned gsize,
                          ttf_point *pts, unsigned max_pts, unsigned *npts,
                          int dx, int dy, int m00, int m01, int m10, int m11,
                          int depth) {
    int err = TTF_OK;
    unsigned p = goff + 10;
    unsigned produced = 0;
    u32 flags;

    if (depth >= TTF_MAX_COMPONENT_DEPTH) return TTF_ERR_FORMAT;
    do {
        unsigned child, cdx = 0, cdy = 0;
        int cm00 = 16384, cm01 = 0, cm10 = 0, cm11 = 16384;
        if (!in_range(f, p, 4)) return TTF_ERR_TRUNCATED;
        flags = rd16(f, p, &err);           if (err) return err;
        child = rd16(f, p + 2, &err);       if (err) return err;
        p += 4;
        if (child >= f->num_glyphs) return TTF_ERR_FORMAT;
        if (flags & CF_WORDS) {
            cdx = rd16(f, p, &err);  if (err) return err;
            cdy = rd16(f, p + 2, &err); if (err) return err;
            p += 4;
            if (flags & CF_XY_VALUES) { cdx = (unsigned)(i16)cdx; cdy = (unsigned)(i16)cdy; }
        } else {
            u32 a = rd16(f, p, &err);   if (err) return err;
            u32 b = rd16(f, p + 2, &err); if (err) return err;
            p += 4;
            if (flags & CF_XY_VALUES) { cdx = (unsigned)(signed char)a; cdy = (unsigned)(signed char)b; }
            else { cdx = a; cdy = b; } /* point-matching: args are point indices -- reject */
        }
        if (!(flags & CF_XY_VALUES)) return TTF_ERR_FORMAT; /* minimal: XY only */
        if (flags & CF_SCALE) {
            int s = (int)srd16(f, p, &err);
            if (err) return err;
            cm00 = cm11 = s;
            p += 2;
        } else if (flags & CF_XY_SCALE) {
            cm00 = (int)srd16(f, p, &err);     if (err) return err;
            cm11 = (int)srd16(f, p + 2, &err); if (err) return err;
            p += 4;
        } else if (flags & CF_2X2) {
            cm00 = (int)srd16(f, p, &err);     if (err) return err;
            cm01 = (int)srd16(f, p + 2, &err); if (err) return err;
            cm10 = (int)srd16(f, p + 4, &err); if (err) return err;
            cm11 = (int)srd16(f, p + 6, &err); if (err) return err;
            p += 8;
        }
        {
            unsigned cn = 0;
            int tdx, tdy;
            /* outer transform then component offset (glyph coords) */
            tdx = fmul((int)cdx, m00) + fmul((int)cdy, m10) + dx;
            tdy = fmul((int)cdx, m01) + fmul((int)cdy, m11) + dy;
            {
                int nm00 = fmul(cm00, m00) + fmul(cm01, m10);
                int nm01 = fmul(cm00, m01) + fmul(cm01, m11);
                int nm10 = fmul(cm10, m00) + fmul(cm11, m10);
                int nm11 = fmul(cm10, m01) + fmul(cm11, m11);
                int r = read_glyph(f, child,
                                   pts ? pts + produced : 0,
                                   max_pts ? max_pts - produced : 0, &cn,
                                   tdx, tdy, nm00, nm01, nm10, nm11,
                                   depth + 1);
                if (r) return r;
            }
            produced += cn;
        }
    } while (flags & CF_MORE);
    *npts = produced;
    return TTF_OK;
}

static int read_glyph(const ttf_font *f, unsigned gid,
                      ttf_point *pts, unsigned max_pts, unsigned *npts,
                      int dx, int dy, int m00, int m01, int m10, int m11,
                      int depth) {
    int err = TTF_OK;
    unsigned start, end, gsize, goff;
    i16 ncon;
    ttf_point raw[512];
    unsigned nraw = 0, i;

    if (gid >= f->num_glyphs) return TTF_ERR_RANGE;
    if (f->index_to_loc == 0) {
        start = rd16(f, f->off_loca + gid * 2, &err) * 2u;
        end   = rd16(f, f->off_loca + gid * 2 + 2, &err) * 2u;
    } else {
        start = rd32(f, f->off_loca + gid * 4, &err);
        end   = rd32(f, f->off_loca + gid * 4 + 4, &err);
    }
    if (err) return err;
    if (start > end) return TTF_ERR_FORMAT;
    if (start == end) { *npts = 0; return TTF_OK; } /* blank glyph */
    goff = f->off_glyf + start;
    if (goff > f->size || end - start > f->size - f->off_glyf - start)
        return TTF_ERR_TRUNCATED;
    gsize = end - start;
    if (gsize < 10) return TTF_ERR_TRUNCATED;
    ncon = srd16(f, goff, &err);
    if (err) return err;

    if (ncon >= 0) {
        int r = read_simple(f, goff, gsize, raw, 512, &nraw);
        if (r) return r;
        for (i = 0; i < nraw; i++) {
            if (pts && i < max_pts) {
                pts[i].x = fmul(raw[i].x, m00) + fmul(raw[i].y, m10) + dx;
                pts[i].y = fmul(raw[i].x, m01) + fmul(raw[i].y, m11) + dy;
                pts[i].on_curve = raw[i].on_curve;
            }
        }
        *npts = nraw;
        return (nraw > max_pts) ? TTF_ERR_NOMEM : TTF_OK;
    }
    return read_composite(f, goff, gsize, pts, max_pts, npts,
                          dx, dy, m00, m01, m10, m11, depth);
}

int ttf_glyph_outline(const ttf_font *f, unsigned gid,
                      ttf_point *pts, unsigned max_pts,
                      unsigned *npts, int *advance) {
    int err;
    if (!f || !npts) return TTF_ERR_FORMAT;
    err = ttf_glyph_advance(f, gid, advance);
    if (err) return err;
    return read_glyph(f, gid, pts, max_pts, npts, 0, 0, 16384, 0, 0, 16384, 0);
}

unsigned ttf_units_per_em(const ttf_font *f) {
    return f ? f->units_per_em : 0;
}
