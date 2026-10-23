/* Bare-metal driver for counting executed ARM instructions (see armcount.py).
 * Built with the camera compiler and flags; runs the original loops
 * (reference/ref_overlays.c) and the kernels on the same EOSM LCD frame and
 * returns a checksum of each output, so bit-exactness is also checked on the
 * real target code generation. No libc. */

#include "ref_shim.h"
#include "overlay_kernels.h"

/* freestanding helpers the compiler may emit calls to */
void * memset(void * d, int c, unsigned n) { uint8_t * p = d; while (n--) *p++ = c; return d; }
void * memcpy(void * d, const void * s, unsigned n) { uint8_t * p = d; const uint8_t * q = s; while (n--) *p++ = *q++; return d; }

#define LV_W 720
#define LV_H 480
static uint32_t lv_words[LV_W / 2 * (LV_H + 2)];
static uint8_t * const lv_buf = (uint8_t *)(lv_words + LV_W / 2);
static uint32_t bmp_ref[BMP_VRAM_SIZE / 4], mir_ref[BMP_VRAM_SIZE / 4];
static uint32_t bmp_new[BMP_VRAM_SIZE / 4], mir_new[BMP_VRAM_SIZE / 4];
#define BMP_ORIGIN(buf) ((uint8_t *)(buf) + BMP_HDMI_OFFSET)
static uint16_t bm_lv_x_cache[BMP_W_PLUS - BMP_W_MINUS];
static uint32_t hash;

static void mix(uint32_t v) { hash = (hash ^ v) * 16777619u; }
static uint32_t hash_words(const uint32_t * p, int n) { hash = 2166136261u; while (n--) mix(*p++); return hash; }

static uint32_t rng_state;
static uint32_t rng(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* same "photo" frame as test_overlays.c: smooth shading, textured patches, mild noise */
void setup(void)
{
    precompute_yuv2rgb(1);
    os.x0 = 0; os.y0 = 0; os.x_ex = LV_W; os.y_ex = LV_H; os.x_max = LV_W; os.y_max = LV_H;
    vram_lv.width = LV_W; vram_lv.height = LV_H; vram_lv.pitch = LV_W * 2; vram_lv.vram = lv_buf;
    bm2lv.tx = 0; bm2lv.ty = 0; bm2lv.sx = 1024; bm2lv.sy = 1024;
    vram_update_luts();
    for (int x = 8; x < LV_W - 8; x++) bm_lv_x_cache[x - BMP_W_MINUS] = BM2LV_X(x) * 2 + 1;

    rng_state = 5;
    for (int y = 0; y < LV_H; y++)
        for (int x = 0; x < LV_W; x += 2)
        {
            int base = ((x * 3 + y * 2) >> 3) & 255;
            int tex = (((x >> 3) + (y >> 3)) % 5 == 0) ? (int)((x * y) & 31) - 16 : 0;
            int Y0 = clamp8(base + tex + (int)(rng() % 7) - 3);
            int Y1 = clamp8(base + tex + (int)(rng() % 7) - 3);
            int u = 128 + ((x >> 4) & 31) - 16, v = 128 + ((y >> 4) & 31) - 16;
            ((uint32_t *)(lv_buf + y * vram_lv.pitch))[x / 2] = u | Y0 << 8 | v << 16 | (uint32_t) Y1 << 24;
        }
}

/* focus peaking, LiveView grid; returns hash of (n_over, hits) */
static void found_ref(int x, int y, int e, int thr) { mix(x); mix(y); mix(e); mix(thr); dirty_pixels_num++; }
static int found_new(void * ctx, int x, int y, int e)
{
    if (dirty_pixels_num >= MAX_DIRTY_PIXELS) return 1;
    mix(x); mix(y); mix(e); mix(*(int *) ctx); dirty_pixels_num++;
    return 0;
}

uint32_t ref_peak(int thr, int fe)
{
    hash = 2166136261u; dirty_pixels_num = 0;
    int n = ref_focus_scan((uintptr_t) lv_buf, 1, 1, thr, fe, 0, found_ref);
    mix(n);
    return hash;
}

/* hits of the original scan, to calibrate the threshold */
int ref_peak_count(int thr, int fe)
{
    dirty_pixels_num = 0;
    return ref_focus_scan((uintptr_t) lv_buf, 1, 1, thr, fe, 0, found_ref);
}

uint32_t new_peak(int thr, int fe)
{
    hash = 2166136261u; dirty_pixels_num = 0;
    struct ovk_peak pk = { .lv = lv_buf, .lv_pitch = vram_lv.pitch, .sy = bm2lv.sy, .ty = bm2lv.ty,
        .xoff = &bm_lv_x_cache[8 - BMP_W_MINUS], .x0 = 8, .x1 = LV_W - 8, .xstep = 2,
        .y0 = 8, .y1 = LV_H - 8, .ystep = 3, .filter_edges = fe, .thr = thr };
    int n = ovk_peak_scan(&pk, found_new, &thr);
    mix(n);
    return hash;
}

/* zebras: returns hash of overlay + mirror */
static uint32_t zebra_luma_class_color(int cls, int y)
{
    return cls == OVK_ZEBRA_LUMA_OVER  ? zebra_color_word_row(COLOR_RED,  y) :
           cls == OVK_ZEBRA_LUMA_UNDER ? zebra_color_word_row(COLOR_BLUE, y) : 0;
}
static uint32_t zebra_rgb_class_color(int cls, int y)
{
    if (cls == OVK_ZEBRA_RGB_UNDER) return zebra_rgb_color(1, 0, 0, 0, y);
    return zebra_rgb_color(0, cls & 4, cls & 2, cls & 1, y);
}

uint32_t ref_zebra(int cs, int hi, int lo)
{
    ref_draw_zebras_soft(BMP_ORIGIN(bmp_ref), BMP_ORIGIN(mir_ref), lv_buf, cs, hi, lo, 0);
    return hash_words(bmp_ref, BMP_VRAM_SIZE / 4) ^ hash_words(mir_ref, BMP_VRAM_SIZE / 4);
}

/* cost of the checksum alone, subtracted by armcount.py */
uint32_t hash_only(void)
{
    return hash_words(bmp_ref, BMP_VRAM_SIZE / 4) ^ hash_words(mir_ref, BMP_VRAM_SIZE / 4);
}

uint32_t new_zebra(int cs, int hi, int lo)
{
    struct ovk_geom g = { lv_buf, vram_lv.pitch, bm2lv.sy, bm2lv.ty, &BM2LV_X(0),
                          BMP_ORIGIN(bmp_new), BMP_ORIGIN(mir_new), BMPPITCH };
    struct ovk_yuv2rgb t = { yuv2rgb_RV, yuv2rgb_GU, yuv2rgb_GV, yuv2rgb_BU };
    ovk_zebra(&g, 0, LV_W, 0, LV_H, hi * 255 / 100 - 1, lo * 255 / 100,
              cs ? &t : 0, cs ? zebra_rgb_class_color : zebra_luma_class_color);
    return hash_words(bmp_new, BMP_VRAM_SIZE / 4) ^ hash_words(mir_new, BMP_VRAM_SIZE / 4);
}

/* histogram: returns hash of the whole struct */
static struct Histogram h;

uint32_t ref_hist(int rgb)
{
    memset(&h, 0, sizeof(h)); h.is_rgb = rgb;
    ref_hist_build((uint32_t *) lv_buf, &h, 1, 0, 0);
    return hash_words((uint32_t *) &h, sizeof(h) / 4);
}

uint32_t new_hist(int rgb)
{
    memset(&h, 0, sizeof(h)); h.is_rgb = rgb;
    struct ovk_geom g = { lv_buf, vram_lv.pitch, bm2lv.sy, bm2lv.ty, &BM2LV_X(0), 0, 0, BMPPITCH };
    struct ovk_yuv2rgb t = { yuv2rgb_RV, yuv2rgb_GU, yuv2rgb_GV, yuv2rgb_BU };
    h.total_px = ovk_hist(&g, 0, LV_W, 0, LV_H, 0, &h.hist, rgb ? &t : 0);
    h.max = ovk_hist_max(h.hist);
    return hash_words((uint32_t *) &h, sizeof(h) / 4);
}

/* armcount.py calls the functions above directly and stops at this address */
void halt(void) { for (;;) { } }
