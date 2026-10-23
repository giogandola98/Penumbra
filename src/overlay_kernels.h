/** \file
 * Pixel kernels for the LiveView overlays (zebras, focus peaking, histogram).
 *
 * Pure C99 with no camera dependencies: every buffer, stride and threshold is
 * passed explicitly, so the very same code is built and characterised on the
 * host (tests/host) against verbatim copies of the original zebra.c loops.
 *
 * Buffers: the LiveView image is UYVY (YUV422, little-endian words
 * U Y0 V Y1); the overlay ("BMP") is 8 bits per pixel, palette indexed,
 * and has a mirror holding what ML drew last time, so that pixels drawn by
 * Canon (BMP != 0 && BMP != mirror) are never overwritten.
 */
#ifndef _overlay_kernels_h_
#define _overlay_kernels_h_

#include <stdint.h>

/* BMP -> LiveView mapping and the buffers the zebra / histogram scans touch */
struct ovk_geom
{
    const uint8_t * lv;     /* UYVY LiveView buffer */
    int lv_pitch;           /* bytes per LV row; must be a multiple of 4 */
    int sy, ty;             /* LV row of BMP row y: ((y * sy) >> 10) + ty, as BM2LV_Y */
    const int * lv_x;       /* LV column of BMP column x0 + i, as BM2LV_X (i.e. bm2lv_x_cache + x0 - BMP_W_MINUS) */
    uint8_t * bmp;          /* overlay and its mirror; both 4-byte aligned */
    uint8_t * mirror;
    int bmp_pitch;          /* bytes per BMP row; multiple of 4 */
};

/* the four tables of imgconv.c (precompute_yuv2rgb); entries must stay within
 * +-768 (they are about +-240) for the RGB zebra clip test, see ovk_zebra() */
struct ovk_yuv2rgb
{
    const int * rv;
    const int * gu;
    const int * gv;
    const int * bu;
};

/* ---------------------------------------------------------------- zebras -- */

/* colour classes; RGB zebras use the clip mask R=4|G=2|B=1 (0 = nothing clipped) */
#define OVK_ZEBRA_LUMA_OVER     1
#define OVK_ZEBRA_LUMA_UNDER    2
#define OVK_ZEBRA_RGB_UNDER     8
#define OVK_ZEBRA_CLASSES       9

/* 4-pixel BMP colour word painted for a class on BMP row y (0 = clear) */
typedef uint32_t (*ovk_zebra_color_fn)(int cls, int y);

/* Software zebras for BMP rows y0, y0+2 .. < y1 and columns x0, x0+4 .. < x1;
 * each LV sample paints a 4x2 block of the overlay.
 *   t == 0 (luma): Y0 of the sample > zlh => over, < zll => under
 *   t != 0 (rgb):  average Y < zll => under, else clip mask of R,G,B > zlh */
void ovk_zebra(const struct ovk_geom * g, int x0, int x1, int y0, int y1,
               int zlh, int zll, const struct ovk_yuv2rgb * t, ovk_zebra_color_fn color);

/* ----------------------------------------------------------- focus peaking */

/* Called for every sample whose edge strength e >= thr, in scan order.
 * Return nonzero to skip the rest of the current row. */
typedef int (*ovk_peak_hit_fn)(void * ctx, int x, int y, int e);

struct ovk_peak
{
    const uint8_t * lv;     /* UYVY LiveView buffer */
    int lv_pitch;
    int sy, ty;             /* row mapping, as in ovk_geom */
    const uint16_t * xoff;  /* byte offset of the luma of BMP column x0 + i within an LV row */
    int x0, x1, xstep;
    int y0, y1, ystep;
    int filter_edges;       /* 0 = plain Laplacian, else attenuate strong first derivatives */
    int thr;
};

/* Laplacian edge detector over the given BMP grid; returns how many samples
 * reached thr (each of them reported to hit(), in scan order). */
int ovk_peak_scan(const struct ovk_peak * pk, ovk_peak_hit_fn hit, void * ctx);

/* -------------------------------------------------------------- histogram */

#define OVK_HIST_BINS 128

/* Histogram of the average luma of the LV samples at BMP rows y0, y0+2 .. < y1,
 * columns x0, x0+2 .. < x1, added to hist[0]. If t is given, the R, G, B
 * histograms are added to hist[1..3] as well. If mz, samples equal to one of
 * the magic zoom border colours are skipped. Returns the number of samples
 * counted. */
uint32_t ovk_hist(const struct ovk_geom * g, int x0, int x1, int y0, int y1, int mz,
                  uint32_t hist[][OVK_HIST_BINS], const struct ovk_yuv2rgb * t);

/* largest bin, ignoring bin 0 (too noisy) - used to scale the plot */
uint32_t ovk_hist_max(const uint32_t hist[OVK_HIST_BINS]);

/* magic zoom border colours, never counted in the histogram */
#define OVK_MZ_WHITE 0xFE12FE34
#define OVK_MZ_BLACK 0x00120034
#define OVK_MZ_GREEN 0xB68DB69E

#endif /* _overlay_kernels_h_ */
