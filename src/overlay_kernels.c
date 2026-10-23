/** \file
 * Pixel kernels for the LiveView overlays - see overlay_kernels.h.
 *
 * Tuned for the ARM926 (ARMv5TE: in-order, single issue, no SIMD, no
 * bitfield extract, 13 usable registers, no branch prediction):
 * - each hot loop is a small non-inlined function, so its invariants stay in
 *   registers instead of being spilled by the surrounding frame loop;
 * - the common case runs straight through (conditional execution, compares
 *   folded into one unsigned test); rare cases - Canon drew over us, a focus
 *   peaking hit, a clipped colour channel - branch out of it;
 * - redundant stores to the overlay are skipped (most of it never changes).
 */

#include "overlay_kernels.h"

#define OVK_INLINE static inline __attribute__((always_inline))
#define OVK_NOINLINE static __attribute__((noinline))

/* UYVY word, little endian: U Y0 V Y1 */
#define UYVY_U(w)   ((w) & 0xFF)
#define UYVY_Y0(w)  (((w) >> 8) & 0xFF)
#define UYVY_V(w)   (((w) >> 16) & 0xFF)
#define UYVY_Y1(w)  ((w) >> 24)

OVK_INLINE int ovk_abs(int a) { return a < 0 ? -a : a; }

/* byte offset of the LV row shown on BMP row y (BM2LV_R) */
OVK_INLINE int lv_row_offset(int y, int sy, int ty, int pitch)
{
    return (((y * sy) >> 10) + ty) * pitch;
}

/* LV row of BMP row y, as words (lv_pitch is a multiple of 4) */
OVK_INLINE const uint32_t * lv_row(const struct ovk_geom * g, int y)
{
    return (const uint32_t *)(g->lv + lv_row_offset(y, g->sy, g->ty, g->lv_pitch));
}

/* ------------------------------------------------------------------ zebras */

/* erase the pixels of a BMP word that we drew earlier (BMP == mirror);
 * same as little_cleanup() in imgconv.c, kept here so the module stays pure */
static void cleanup_word(uint32_t * BP, uint32_t * MP)
{
    uint8_t * bp = (uint8_t *) BP; uint8_t * mp = (uint8_t *) MP;
    for (int i = 0; i < 4; i++)
    {
        if (bp[i] != 0 && bp[i] == mp[i]) mp[i] = bp[i] = 0;
    }
}

/* everything a row of zebras needs, so the row loop gets it in one pointer */
struct zebra_row
{
    uint32_t * bp;              /* BMP words of row y, from x0 */
    uint32_t * mp;              /* mirror words */
    int pw;                     /* BMP pitch, in words */
    int n;                      /* BMP words to paint */
    const uint32_t * lv;        /* LV sample of the first word */
    int stride;                 /* LV words between the samples of consecutive BMP words */
    const uint32_t * col;       /* colour by class on row y; row y+1 at col + OVK_ZEBRA_CLASSES */
    const uint8_t * luma_class; /* luma: class of each Y0 value */
    int zll, zclip;             /* rgb: underexposure / clipping thresholds */
    const struct ovk_yuv2rgb * t;
};

OVK_INLINE int zebra_rgb_class(uint32_t w, int zll, int zclip, const int * rv, const int * gu, const int * gv, const int * bu)
{
    const int Y = (UYVY_Y0(w) + UYVY_Y1(w)) >> 1;
    if (Y < zll) return OVK_ZEBRA_RGB_UNDER;

    /* channel > zlh, with the channel clamped to 0..255, is channel - Y > zclip - Y */
    const int u = UYVY_U(w), v = UYVY_V(w), lim = zclip - Y;
    return (rv[v]         > lim) << 2
         | (gu[u] + gv[v] > lim) << 1
         | (bu[u]         > lim);
}

/* Each word is 4 BMP pixels, painted together with the word below it from
 * one LV sample. A word is left alone when Canon drew on it (and our stale
 * pixels there are erased) or when ML graphics (0x80 colours) cover it.
 * Everything is copied to locals: with -fno-strict-aliasing, the overlay
 * stores would otherwise force the struct fields to be reloaded. */
OVK_INLINE void zebra_row(const struct zebra_row * z, const int rgb)
{
    uint32_t * bp = z->bp, * mp = z->mp;
    uint32_t * const end = bp + z->n;
    const int pw = z->pw, stride = z->stride;
    const uint32_t * lvp = z->lv;
    const uint32_t * const col = z->col;
    const uint8_t * const luma_class = z->luma_class;
    const int zll = z->zll, zclip = z->zclip;
    const int * const rv = rgb ? z->t->rv : 0;
    const int * const gu = rgb ? z->t->gu : 0;
    const int * const gv = rgb ? z->t->gv : 0;
    const int * const bu = rgb ? z->t->bu : 0;

    for (; bp < end; bp++, mp++, lvp += stride)
    {
        const uint32_t b = bp[0], m = mp[0], bn = bp[pw], mn = mp[pw];
        if ((b ^ m) | (bn ^ mn))
        {
            /* rare: Canon drew or erased something here */
            if (b != 0 && b != m) { cleanup_word(bp, mp); continue; }
            if (bn != 0 && bn != mn) { cleanup_word(bp + pw, mp + pw); continue; }
        }
        if ((m | mn) & 0x80808080) continue;

        const int cls = rgb ? zebra_rgb_class(*lvp, zll, zclip, rv, gu, gv, bu)
                            : luma_class[((const uint8_t *) lvp)[1]];
        const uint32_t c = col[cls], cn = col[cls + OVK_ZEBRA_CLASSES];
        if ((b ^ c) | (m ^ c))      bp[0]  = mp[0]  = c;
        if ((bn ^ cn) | (mn ^ cn))  bp[pw] = mp[pw] = cn;
    }
}

OVK_NOINLINE void zebra_row_luma(const struct zebra_row * z) { zebra_row(z, 0); }
OVK_NOINLINE void zebra_row_rgb (const struct zebra_row * z) { zebra_row(z, 1); }

/* LV words between consecutive samples lv_x[0], lv_x[step], ... if constant, else 0 */
static int lv_stride(const int * lv_x, int step, int n)
{
    const int s = n > 1 ? (lv_x[step] >> 1) - (lv_x[0] >> 1) : 0;
    for (int i = 2; i < n; i++)
    {
        if ((lv_x[i * step] >> 1) - (lv_x[0] >> 1) != s * i) return 0;
    }
    return s;
}

void ovk_zebra(const struct ovk_geom * g, int x0, int x1, int y0, int y1,
               int zlh, int zll, const struct ovk_yuv2rgb * t, ovk_zebra_color_fn color)
{
    const int n = x1 > x0 ? (x1 - x0 + 3) >> 2 : 0;
    const int stride = lv_stride(g->lv_x, 4, n);
    void (* const row_fn)(const struct zebra_row *) = t ? zebra_row_rgb : zebra_row_luma;

    uint8_t luma_class[256];
    struct zebra_row z = {
        .pw = g->bmp_pitch >> 2,
        .luma_class = luma_class,
        .zll = zll,
        /* clamped channel: always above zlh if zlh < 0, never if zlh >= 255;
         * +-1024 is beyond any Y + table value, so channel - Y never reaches it */
        .zclip = zlh < 0 ? -1024 : zlh >= 255 ? 1024 : zlh,
        .t = t,
    };
    if (!t)
    {
        for (int i = 0; i < 256; i++)
            luma_class[i] = i > zlh ? OVK_ZEBRA_LUMA_OVER : i < zll ? OVK_ZEBRA_LUMA_UNDER : 0;
    }

    /* Stripes repeat every 4 rows, so 2 scanned rows (y, y+1 each) cover the
     * pattern - but y % 4 is negative above the LCD area (HDMI), so the rows
     * on each side of y = 0 get their own colour tables. */
    const int y_pos = y0 < 0 ? y0 + ((1 - y0) & ~1) : y0;   /* first scanned y >= 0 */
    const int split = y_pos < y1 ? y_pos : y1;

    for (int part = 0; part < 2; part++)
    {
        const int ya = part ? split : y0;
        const int yb = part ? y1 : split;
        if (ya >= yb) continue;

        /* col[2p + r][class]: row y + r of the scanned rows y with ((y - ya) >> 1) & 1 == p */
        uint32_t col[4][OVK_ZEBRA_CLASSES];
        for (int k = 0; k < 4; k++)
            for (int cls = 0; cls < OVK_ZEBRA_CLASSES; cls++)
                col[k][cls] = color(cls, ya + k);

        for (int y = ya, p = 0; y < yb; y += 2, p ^= 1)
        {
            uint32_t * const bp = (uint32_t *)(g->bmp    + y * g->bmp_pitch) + (x0 >> 2);
            uint32_t * const mp = (uint32_t *)(g->mirror + y * g->bmp_pitch) + (x0 >> 2);
            const uint32_t * const lv = lv_row(g, y);
            z.col = col[2 * p];
            if (stride || n < 2)
            {
                z.bp = bp; z.mp = mp; z.n = n;
                z.lv = lv + (g->lv_x[0] >> 1);
                z.stride = stride;
                row_fn(&z);
            }
            else for (int i = 0; i < n; i++)    /* irregular BMP->LV mapping (RCA): word by word */
            {
                z.bp = bp + i; z.mp = mp + i; z.n = 1;
                z.lv = lv + (g->lv_x[4 * i] >> 1);
                row_fn(&z);
            }
        }
    }
}

/* ----------------------------------------------------------- focus peaking */

/* Laplacian  -1        on luma (neighbours are 2 bytes apart in UYVY);
 *         -1  4 -1     with filter_edges, strong first derivatives (usually
 *            -1        hard edges, not fine detail) are attenuated. */
OVK_INLINE int peak_edge(const uint8_t * p, int pitch, int fe)
{
    const int l = p[-2], r = p[2], u = p[-pitch], d = p[pitch];
    int e = ovk_abs(p[0] * 4 - (r + l + d + u));
    if (fe)
    {
        int d1x = ovk_abs(r - l);
        int d1y = ovk_abs(d - u);
        int d1 = d1x > d1y ? d1x : d1y;
        e -= (d1 << fe) >> 2;
        e = (e > 0 ? e : 0) * 2;
    }
    return e;
}

/* First p in [p, end), stepping 4 bytes (one sample per UYVY word: the
 * LiveView case), whose Laplacian reaches thr; end if none.
 * - the right neighbour of a sample is the left one of the next, so it is
 *   loaded once (unrolled by two so no register copy is needed);
 * - |v| >= thr is the single unsigned test v + thr-1 >= 2*thr-1 (1 <= thr < 2^30);
 * - byte loads, not word loads: ARMv5 has no bitfield extract, so unpacking
 *   bytes from words costs more than the loads it saves. */
OVK_NOINLINE const uint8_t * peak_find_w4(const uint8_t * p, const uint8_t * end, int pitch, int thr)
{
    if (thr <= 0) return p;     /* every sample is a hit */

    const int bias = thr - 1;
    const unsigned span = 2 * thr - 1;
    #define PEAK_HIT(q, l, r) ((unsigned)(((q)[0] << 2) + bias - ((l) + (r) + (q)[-pitch] + (q)[pitch])) >= span)

    int l = p[-2];
    if (p < end - 4) do
    {
        const int r = p[2];
        if (PEAK_HIT(p, l, r)) return p;
        l = p[6];
        if (PEAK_HIT(p + 4, r, l)) return p + 4;
        p += 8;
    }
    while (p < end - 4);
    if (p < end && PEAK_HIT(p, l, p[2])) return p;
    return end;
    #undef PEAK_HIT
}

/* same with filter_edges (fe > 0): max(e - ((d1 << fe) >> 2), 0) * 2 >= thr
 * is e - ((d1 << fe) >> 2) >= (thr + 1) / 2 for thr >= 1 */
OVK_NOINLINE const uint8_t * peak_find_w4_fe(const uint8_t * p, const uint8_t * end, int pitch, int fe, int thr)
{
    if (thr <= 0) return p;

    const int half = (thr + 1) >> 1;
    for (; p < end; p += 4)
    {
        const int l = p[-2], r = p[2], u = p[-pitch], d = p[pitch];
        const int e = ovk_abs((p[0] << 2) - (l + r + u + d));
        const int d1x = ovk_abs(r - l), d1y = ovk_abs(d - u);
        const int d1 = d1x > d1y ? d1x : d1y;
        if (e - ((d1 << fe) >> 2) >= half) break;
    }
    return p;
}

/* edge strength of a hit, for the callback (out of line: hits are rare) */
OVK_NOINLINE int peak_edge_at(const uint8_t * p, int pitch, int fe)
{
    return peak_edge(p, pitch, fe);
}

/* any other BMP->LV column mapping (playback, HDMI, RCA):
 * first i0 <= i < n with edge >= thr at row + xoff[i * xstep]; n if none */
OVK_NOINLINE int peak_find_lut(const uint8_t * row, const uint16_t * xoff, int xstep, int i, int n,
                               int pitch, int fe, int thr)
{
    for (xoff += i * xstep; i < n; i++, xoff += xstep)
    {
        if (peak_edge(row + *xoff, pitch, fe) >= thr) break;
    }
    return i;
}

int ovk_peak_scan(const struct ovk_peak * pk, ovk_peak_hit_fn hit, void * ctx)
{
    const int n = pk->x1 > pk->x0 ? (pk->x1 - pk->x0 + pk->xstep - 1) / pk->xstep : 0;
    const int pitch = pk->lv_pitch;
    const int fe = pk->filter_edges;
    const uint16_t * const xoff = pk->xoff;

    /* consecutive samples exactly one UYVY word apart? (LiveView on the LCD) */
    int w4 = 1;
    for (int i = 1; i < n && w4; i++)
        w4 = xoff[i * pk->xstep] == xoff[0] + 4 * i;

    int n_over = 0;
    for (int y = pk->y0; y < pk->y1; y += pk->ystep)
    {
        const uint8_t * const row = pk->lv + lv_row_offset(y, pk->sy, pk->ty, pitch);
        const uint8_t * const base = row + xoff[0];
        for (int i = 0; ; i++)
        {
            if (!w4)    i = peak_find_lut(row, xoff, pk->xstep, i, n, pitch, fe, pk->thr);
            else if (fe) i = (peak_find_w4_fe(base + 4 * i, base + 4 * n, pitch, fe, pk->thr) - base) >> 2;
            else        i = (peak_find_w4(base + 4 * i, base + 4 * n, pitch, pk->thr) - base) >> 2;
            if (i >= n) break;

            n_over++;
            if (hit(ctx, pk->x0 + i * pk->xstep, y, peak_edge_at(row + xoff[i * pk->xstep], pitch, fe))) break;
        }
    }
    return n_over;
}

/* -------------------------------------------------------------- histogram */

OVK_INLINE int is_mz_border(uint32_t w)
{
    return w == OVK_MZ_WHITE || w == OVK_MZ_BLACK || w == OVK_MZ_GREEN;
}

/* bin of COERCE(v, 0, 255) with 128 bins */
OVK_INLINE int bin255(int v)
{
    if ((unsigned) v > 255) v = v < 0 ? 0 : 255;
    return v >> 1;
}

/* With 128 bins, the bin of an 8-bit value is v >> 1, so the luma bin
 * ((Y0 + Y1) >> 1) >> 1 is simply (Y0 + Y1) >> 2.
 * Samples p[0], p[stride] ... (n of them). */
OVK_INLINE void hist_row(const uint32_t * p, int stride, int n, uint32_t (*hist)[OVK_HIST_BINS],
                         const struct ovk_yuv2rgb * t, const int rgb)
{
    uint32_t * const h = hist[0], * const hr = hist[1], * const hg = hist[2], * const hb = hist[3];
    const int * const rv = rgb ? t->rv : 0;
    const int * const gu = rgb ? t->gu : 0;
    const int * const gv = rgb ? t->gv : 0;
    const int * const bu = rgb ? t->bu : 0;

    for (int i = n; i > 0; i--, p += stride)
    {
        const uint32_t w = *p;
        const int y01 = UYVY_Y0(w) + UYVY_Y1(w);
        h[y01 >> 2]++;
        if (rgb)
        {
            const int Y = y01 >> 1;
            const int u = UYVY_U(w), v = UYVY_V(w);
            hr[bin255(Y + rv[v])]++;
            hg[bin255(Y + gu[u] + gv[v])]++;
            hb[bin255(Y + bu[u])]++;
        }
    }
}

typedef void (*hist_row_fn)(const uint32_t *, int, int, uint32_t (*)[OVK_HIST_BINS], const struct ovk_yuv2rgb *);

OVK_NOINLINE void hist_row_luma(const uint32_t * p, int stride, int n, uint32_t (*hist)[OVK_HIST_BINS],
                                const struct ovk_yuv2rgb * t) { hist_row(p, stride, n, hist, t, 0); }
OVK_NOINLINE void hist_row_rgb (const uint32_t * p, int stride, int n, uint32_t (*hist)[OVK_HIST_BINS],
                                const struct ovk_yuv2rgb * t) { hist_row(p, stride, n, hist, t, 1); }

/* Samples p[0], p[stride] ... (n of them); returns how many were counted.
 * With magic zoom on, its border colours are skipped: count the runs between them. */
static uint32_t hist_samples(const uint32_t * p, int stride, int n, int mz, uint32_t (*hist)[OVK_HIST_BINS],
                             const struct ovk_yuv2rgb * t, hist_row_fn row_fn)
{
    if (!mz)
    {
        row_fn(p, stride, n, hist, t);
        return n;
    }

    uint32_t total = 0;
    for (int i = 0; i < n; )
    {
        while (i < n && is_mz_border(p[i * stride])) i++;
        int j = i;
        while (j < n && !is_mz_border(p[j * stride])) j++;
        row_fn(p + i * stride, stride, j - i, hist, t);
        total += j - i;
        i = j;
    }
    return total;
}

uint32_t ovk_hist(const struct ovk_geom * g, int x0, int x1, int y0, int y1, int mz,
                  uint32_t hist[][OVK_HIST_BINS], const struct ovk_yuv2rgb * t)
{
    const int n = x1 > x0 ? (x1 - x0 + 1) >> 1 : 0;
    const int stride = lv_stride(g->lv_x, 2, n);
    const hist_row_fn row_fn = t ? hist_row_rgb : hist_row_luma;

    uint32_t total = 0;
    for (int y = y0; y < y1; y += 2)
    {
        const uint32_t * const row = lv_row(g, y);
        if (stride || n < 2)
            total += hist_samples(row + (g->lv_x[0] >> 1), stride, n, mz, hist, t, row_fn);
        else for (int i = 0; i < n; i++)    /* irregular BMP->LV mapping (RCA): sample by sample */
            total += hist_samples(row + (g->lv_x[2 * i] >> 1), 0, 1, mz, hist, t, row_fn);
    }
    return total;
}

uint32_t ovk_hist_max(const uint32_t hist[OVK_HIST_BINS])
{
    uint32_t max = 0;
    for (int i = 1; i < OVK_HIST_BINS; i++)
    {
        if (hist[i] > max) max = hist[i];
    }
    return max;
}
