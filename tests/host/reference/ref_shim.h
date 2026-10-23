/* Minimal stand-ins for the camera globals and macros used by the original
 * overlay loops, so they compile unchanged on the host.
 * Macro bodies are copied from src/vram.h, src/bmp.h, src/imath.h, src/imgconv.h,
 * src/histogram.h (DryOS / DIGIC V configuration, as built for EOSM). */
#ifndef REF_SHIM_H
#define REF_SHIM_H

#include <stdint.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define FAST __attribute__((optimize("-O3")))   /* as src/dryos.h */

/* src/bmp.h (dryos) */
#define BMP_W_PLUS   840
#define BMP_W_MINUS -120
#define BMP_H_PLUS   510
#define BMP_H_MINUS -30
#define BMPPITCH 960
#define BMP_VRAM_SIZE (960*540)
#define BMP_HDMI_OFFSET ((-BMP_H_MINUS)*BMPPITCH + (-BMP_W_MINUS))
#define BMP_TOTAL_WIDTH (BMP_W_PLUS - BMP_W_MINUS)
#define BMP_TOTAL_HEIGHT (BMP_H_PLUS - BMP_H_MINUS)

#define COLOR_BLACK             0x02
#define COLOR_BLUE              0x0B
#define COLOR_CYAN              0x05
#define COLOR_GREEN2            0x07
#define COLOR_RED               0x08
#define COLOR_YELLOW            0x0F
#define COLOR_MAGENTA           0x0E

/* src/imath.h */
#define MIN(a,b) \
   ({ __typeof__ ((a)+(b)) _a = (a); \
      __typeof__ ((a)+(b)) _b = (b); \
     _a < _b ? _a : _b; })
#define MAX(a,b) \
   ({ __typeof__ ((a)+(b)) _a = (a); \
       __typeof__ ((a)+(b)) _b = (b); \
     _a > _b ? _a : _b; })
#define ABS(a) \
   ({ __typeof__ (a) _a = (a); \
     _a > 0 ? _a : -_a; })
#define COERCE(x,lo,hi) MAX(MIN((x),(hi)),(lo))

/* src/vram.h */
struct vram_info { uint8_t * vram; int width; int pitch; int height; };
struct trans2d { int tx; int ty; int sx; int sy; };
struct os_t { int x0, y0, x_ex, y_ex, x_max, y_max, off_169; };
extern struct trans2d bm2lv;
extern struct vram_info vram_lv;
extern struct os_t os;
extern int bm2lv_x_cache[BMP_W_PLUS - BMP_W_MINUS];

#define BM2LV_Xu(x) ((((x) * bm2lv.sx) >> 10) + bm2lv.tx)
#define BM2LV_Y(y) ((((y) * bm2lv.sy) >> 10) + bm2lv.ty)
#define BM2LV_X(x) bm2lv_x_cache[(x) - BMP_W_MINUS]
#define BM2LV(x,y) (BM2LV_Y(y) * vram_lv.pitch + (BM2LV_X(x) << 1))
#define BM2LV_R(y) (BM2LV_Y(y) * vram_lv.pitch)
#define BM_R(y) ((y) * BMPPITCH     )

/* src/imgconv.h (not DIGIC 6/7/8) */
#define UYVY_GET_U(uyvy) (((uyvy)       ) & 0xFF)
#define UYVY_GET_V(uyvy) (((uyvy) >>  16) & 0xFF)
#define UYVY_GET_AVG_Y(uyvy) (((((uyvy) >> 24) & 0xFF) + (((uyvy) >> 8) & 0xFF)) >> 1)
#define COMPUTE_UYVY2YRGB(uyvy, Y, R, G, B) \
{ \
    Y = UYVY_GET_AVG_Y(uyvy); \
    const int gv = UYVY_GET_V(uyvy); \
    const int gu = UYVY_GET_U(uyvy); \
    int v = Y + yuv2rgb_RV[gv]; \
    R = COERCE(v, 0, 255); \
    v = Y + yuv2rgb_GU[gu] + yuv2rgb_GV[gv]; \
    G = COERCE(v, 0, 255); \
    v = Y + yuv2rgb_BU[gu]; \
    B = COERCE(v, 0, 255); \
} \

extern int yuv2rgb_RV[256];
extern int yuv2rgb_GU[256];
extern int yuv2rgb_GV[256];
extern int yuv2rgb_BU[256];

/* src/histogram.h */
#define MZ_WHITE 0xFE12FE34
#define MZ_BLACK 0x00120034
#define MZ_GREEN 0xB68DB69E
#define HIST_WIDTH          128
struct Histogram
{
    uint32_t hist[HIST_WIDTH];
    uint32_t hist_r[HIST_WIDTH];
    uint32_t hist_g[HIST_WIDTH];
    uint32_t hist_b[HIST_WIDTH];
    uint32_t max;
    uint32_t total_px;
    int is_rgb;
    int is_raw;
};

/* reference entry points (reference/ref_overlays.c) */
void precompute_yuv2rgb(int rec709);
void vram_update_luts(void);
void little_cleanup(void * BP, void * MP);
int zebra_color_word_row(int c, int y);
int zebra_rgb_color(int underexposed, int clipR, int clipG, int clipB, int y);

void ref_draw_zebras_soft(uint8_t * bvram, uint8_t * bvram_mirror, uint8_t * lvram,
                          int zebra_colorspace, int zebra_level_hi, int zebra_level_lo, int off);

typedef void (*ref_found_fn)(int x, int y, int e, int thr);
extern int dirty_pixels_num;
#define MAX_DIRTY_PIXELS 5000
int ref_focus_scan(uintptr_t vram, int lv, int F, int thr, int filter_edges, int off, ref_found_fn found);

void ref_hist_build(uint32_t * buf, struct Histogram * h, int hist_draw, int mz, int off);

#endif
