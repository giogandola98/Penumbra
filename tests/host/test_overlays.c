/* Characterization tests for src/overlay_kernels.c.
 *
 * Every kernel is run against the verbatim original loops in
 * reference/ref_overlays.c on deterministic synthetic UYVY frames, for the
 * LiveView geometries EOSM uses (LCD 720x480, HDMI 1080, RCA PAL), and the
 * outputs (overlay + mirror, focus peaking hits, histograms) must match
 * bit for bit.
 *
 *   test_overlays          run the tests, exit status 1 on any mismatch
 *   test_overlays bench    host timing, original vs kernel (indicative only)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reference/ref_shim.h"
#include "overlay_kernels.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------ geometry -- */

struct geometry
{
    const char * name;
    int x0, y0, x_ex, y_ex;     /* os */
    int width, height;          /* vram_lv */
    int tx, ty, sx, sy;         /* bm2lv */
};

/* from src/vram.c, _update_vram_params(), DryOS branch */
static const struct geometry geometries[] = {
    { "LCD 720x480",   0,   0, 720, 480,  720,  480,   0,  0, 1024, 1024 },
    { "HDMI 1080",   -45, -30, 810, 540, 1920, 1080, 240, 60, 2048, 1024 * 1080 / 540 },
    { "RCA PAL",      40,  29, 640, 388,  540,  572,   4,  0,  768, 1024 * 572 / 480 },
};

static uint8_t * lv_alloc;      /* LV buffer with a guard row on each side */
static uint8_t * lv_buf;

static void set_geometry(const struct geometry * g)
{
    os.x0 = g->x0; os.y0 = g->y0; os.x_ex = g->x_ex; os.y_ex = g->y_ex;
    os.x_max = os.x0 + os.x_ex;
    os.y_max = os.y0 + os.y_ex;
    vram_lv.width = g->width;
    vram_lv.height = g->height;
    vram_lv.pitch = g->width * 2;
    bm2lv.tx = g->tx; bm2lv.ty = g->ty; bm2lv.sx = g->sx; bm2lv.sy = g->sy;
    vram_update_luts();

    free(lv_alloc);
    lv_alloc = calloc(1, (size_t) vram_lv.pitch * (vram_lv.height + 2));
    lv_buf = lv_alloc + vram_lv.pitch;
    vram_lv.vram = lv_buf;
}

/* --------------------------------------------------------------- frames -- */

static uint32_t rng_state;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static uint32_t uyvy(int u, int y0, int v, int y1)
{
    return (uint32_t)(u & 0xFF) | (uint32_t)(y0 & 0xFF) << 8 | (uint32_t)(v & 0xFF) << 16 | (uint32_t)(y1 & 0xFF) << 24;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

enum { FRAME_GRADIENT, FRAME_EDGES, FRAME_NOISE, FRAME_CLIPPED, FRAME_PHOTO, FRAME_MZ, FRAME_COUNT };
static const char * frame_names[] = { "gradient", "edges", "noise", "clipped", "photo", "mz-borders" };

static void make_frame(int kind, uint32_t seed)
{
    rng_state = seed;
    const int w2 = vram_lv.width / 2, h = vram_lv.height;
    for (int y = 0; y < h; y++)
    {
        uint32_t * row = (uint32_t *)(lv_buf + y * vram_lv.pitch);
        for (int i = 0; i < w2; i++)
        {
            const int x = 2 * i;
            uint32_t w = 0;
            switch (kind)
            {
                case FRAME_GRADIENT:    /* horizontal luma ramp, vertical chroma ramps */
                    w = uyvy(y * 255 / h, x * 255 / vram_lv.width, 255 - y * 255 / h, (x + 1) * 255 / vram_lv.width);
                    break;
                case FRAME_EDGES:       /* bars and checkers of varying contrast */
                {
                    int bar = (x / 7) & 1, chk = ((x / 16) ^ (y / 16)) & 1;
                    int c = (y / 40) * 12;
                    int Y0 = 128 + (bar ? c : -c) / 2 + (chk ? 40 : -40);
                    int Y1 = 128 + ((((x + 1) / 7) & 1) ? c : -c) / 2 + (chk ? 40 : -40);
                    w = uyvy(128 + (chk ? 30 : -30), clamp8(Y0), 128 - (bar ? 50 : -50), clamp8(Y1));
                    break;
                }
                case FRAME_NOISE:       /* every byte random */
                case FRAME_MZ:
                    w = rng() ^ (rng() << 16);
                    if (kind == FRAME_MZ)
                    {
                        static const uint32_t mzc[3] = { MZ_WHITE, MZ_BLACK, MZ_GREEN };
                        if ((rng() & 7) < 3) w = mzc[rng() % 3];
                    }
                    break;
                case FRAME_CLIPPED:     /* clipped / crushed blocks, values around the thresholds */
                {
                    int blk = ((x / 32) + 3 * (y / 24)) % 6;
                    static const int lv[6] = { 255, 0, 254, 1, 252, 12 };
                    int Y0 = clamp8(lv[blk] + (int)(rng() % 5) - 2);
                    int Y1 = clamp8(lv[blk] + (int)(rng() % 5) - 2);
                    w = uyvy(rng(), Y0, rng(), Y1);
                    break;
                }
                case FRAME_PHOTO:       /* smooth shading, textured patches, mild noise */
                {
                    int base = ((x * 3 + y * 2) >> 3) & 255;
                    int tex = (((x >> 3) + (y >> 3)) % 5 == 0) ? (int)((x * y) & 31) - 16 : 0;
                    int Y0 = clamp8(base + tex + (int)(rng() % 7) - 3);
                    int Y1 = clamp8(base + tex + (int)(rng() % 7) - 3);
                    w = uyvy(128 + ((x >> 4) & 31) - 16, Y0, 128 + ((y >> 4) & 31) - 16, Y1);
                    break;
                }
            }
            row[i] = w;
        }
    }
}

/* ------------------------------------------------------------- overlays -- */

static uint8_t bmp_ref[BMP_VRAM_SIZE], mir_ref[BMP_VRAM_SIZE];
static uint8_t bmp_new[BMP_VRAM_SIZE], mir_new[BMP_VRAM_SIZE];
#define BMP_ORIGIN(buf) ((buf) + BMP_HDMI_OFFSET)

/* 0: empty; 1: mix of our old drawings, Canon drawings and ML menus */
static void make_bmp_state(int kind, uint32_t seed)
{
    rng_state = seed;
    for (int i = 0; i < BMP_VRAM_SIZE; i++)
    {
        uint8_t m = 0, b = 0;
        if (kind)
        {
            switch (rng() % 8)
            {
                case 0: m = 0x08; b = m; break;             /* our zebra */
                case 1: m = 0x0B; b = 0; break;             /* we drew, Canon cleared */
                case 2: m = 0x08; b = 0x30; break;          /* Canon drew over us */
                case 3: m = 0x80 | (rng() & 0x7F); b = m; break; /* ML menu bit */
                case 4: m = 0; b = rng() & 0xFF; break;     /* Canon graphics */
                default: break;
            }
        }
        bmp_ref[i] = bmp_new[i] = b;
        mir_ref[i] = mir_new[i] = m;
    }
}

/* adapters as in src/zebra.c, on top of the reference colour functions */
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

/* overlay_geom() in src/zebra.c */
static void overlay_geom(struct ovk_geom * g, const uint8_t * lv, uint8_t * bvram, uint8_t * mirror, int x0)
{
    g->lv = lv;
    g->lv_pitch = vram_lv.pitch;
    g->sy = bm2lv.sy;
    g->ty = bm2lv.ty;
    g->lv_x = &BM2LV_X(x0);
    g->bmp = bvram;
    g->mirror = mirror;
    g->bmp_pitch = BMPPITCH;
}

static void new_draw_zebras(uint8_t * bvram, uint8_t * mirror, int colorspace, int level_hi, int level_lo, int off)
{
    int zlh = level_hi * 255 / 100 - 1;
    int zll = level_lo * 255 / 100;
    struct ovk_geom g;
    overlay_geom(&g, lv_buf, bvram, mirror, os.x0);
    if (colorspace == 1)
    {
        struct ovk_yuv2rgb t = { yuv2rgb_RV, yuv2rgb_GU, yuv2rgb_GV, yuv2rgb_BU };
        ovk_zebra(&g, os.x0, os.x_max, os.y0 + off, os.y_max - off, zlh, zll, &t, zebra_rgb_class_color);
    }
    else
    {
        ovk_zebra(&g, os.x0, os.x_max, os.y0 + off, os.y_max - off, zlh, zll, 0, zebra_luma_class_color);
    }
}

static int first_diff(const uint8_t * a, const uint8_t * b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return i;
    return -1;
}

static void test_zebras(const struct geometry * geo)
{
    static const int levels[][2] = { { 99, 0 }, { 100, 5 }, { 101, 20 }, { 50, 50 }, { 0, 0 }, { 95, 1 }, { 101, 100 } };
    for (int cs = 0; cs < 2; cs++)
    for (int f = 0; f < FRAME_COUNT; f++)
    for (int st = 0; st < 2; st++)
    for (unsigned l = 0; l < sizeof(levels) / sizeof(levels[0]); l++)
    for (int off = 0; off <= 40; off += 40)
    {
        make_bmp_state(st, 1234 + f);
        for (int pass = 0; pass < 2; pass++)     /* second pass: redraw over our own zebras */
        {
            make_frame(pass ? (f + 1) % FRAME_COUNT : f, 42 + pass);
            ref_draw_zebras_soft(BMP_ORIGIN(bmp_ref), BMP_ORIGIN(mir_ref), lv_buf, cs, levels[l][0], levels[l][1], off);
            new_draw_zebras(BMP_ORIGIN(bmp_new), BMP_ORIGIN(mir_new), cs, levels[l][0], levels[l][1], off);
            int db = first_diff(bmp_ref, bmp_new, BMP_VRAM_SIZE);
            int dm = first_diff(mir_ref, mir_new, BMP_VRAM_SIZE);
            CHECK(db < 0 && dm < 0, "zebras %s %s frame=%s bmp=%d hi=%d lo=%d off=%d pass=%d: bmp diff @%d, mirror diff @%d",
                  geo->name, cs ? "rgb" : "luma", frame_names[f], st, levels[l][0], levels[l][1], off, pass, db, dm);
        }
    }
}

/* -------------------------------------------------------- focus peaking -- */

#define MAX_HITS 400000
struct hit { int x, y, e, thr; };
static struct hit hits_ref[MAX_HITS], hits_new[MAX_HITS];
static int n_hits_ref, n_hits_new;

/* stands in for focus_found_pixel(): every drawn dot takes a dirty slot */
static void found_ref(int x, int y, int e, int thr)
{
    if (n_hits_ref < MAX_HITS) hits_ref[n_hits_ref++] = (struct hit) { x, y, e, thr };
    dirty_pixels_num++;
}

struct new_ctx { int thr; int dirty; };

/* focus_peak_hit_lv() / _playback() in src/zebra.c, with found_ref()'s bookkeeping */
static int found_new(void * ctx, int x, int y, int e)
{
    struct new_ctx * c = ctx;
    if (c->dirty >= MAX_DIRTY_PIXELS) return 1;
    if (n_hits_new < MAX_HITS) hits_new[n_hits_new++] = (struct hit) { x, y, e, c->thr };
    c->dirty++;
    return 0;
}

static uint16_t bm_lv_x_cache[BMP_W_PLUS - BMP_W_MINUS];

/* the part of draw_zebra_and_focus() in src/zebra.c that sets up ovk_peak_scan */
static int new_focus_scan(int lv, int thr, int fe, int off, struct new_ctx * ctx)
{
    int yStart = os.y0 + off + 8;
    int yEnd = os.y_max - off - 8;
    int xStart = os.x0 + 8;
    int xEnd = os.x_max - 8;

    /* zebra_update_lut() */
    for (int x = xStart; x < xEnd; x += 1)
        bm_lv_x_cache[x - BMP_W_MINUS] = BM2LV_X(x) * 2 + 1;

    struct ovk_peak pk = {
        .lv = lv_buf,
        .lv_pitch = vram_lv.pitch,
        .sy = bm2lv.sy, .ty = bm2lv.ty,
        .xoff = &bm_lv_x_cache[xStart - BMP_W_MINUS],
        .x0 = xStart, .x1 = xEnd,
        .y0 = yStart, .y1 = yEnd,
        .filter_edges = fe,
        .thr = thr,
    };
    if (lv) { pk.xstep = 2; pk.ystep = 3; }
    else    { pk.xstep = 1; pk.ystep = 1; }
    return ovk_peak_scan(&pk, found_new, ctx);
}

static void test_peaking(const struct geometry * geo)
{
    static const int thrs[] = { 1, 15, 30, 60, 120, 400 };
    for (int f = 0; f < FRAME_COUNT; f++)
    {
        make_frame(f, 7 + f);
        for (int lv = 0; lv < 2; lv++)
        for (int fe = 0; fe <= 2; fe++)
        for (unsigned t = 0; t < sizeof(thrs) / sizeof(thrs[0]); t++)
        for (int off = 0; off <= 40; off += 40)
        {
            n_hits_ref = n_hits_new = 0;
            dirty_pixels_num = 0;
            int over_ref = ref_focus_scan((uintptr_t) lv_buf, lv, 1, thrs[t], fe, off, found_ref);
            struct new_ctx ctx = { thrs[t], 0 };
            int over_new = new_focus_scan(lv, thrs[t], fe, off, &ctx);

            int same = over_ref == over_new && n_hits_ref == n_hits_new &&
                       memcmp(hits_ref, hits_new, n_hits_ref * sizeof(struct hit)) == 0;
            CHECK(same, "peaking %s frame=%s %s fe=%d thr=%d off=%d: n_over %d vs %d, hits %d vs %d",
                  geo->name, frame_names[f], lv ? "LV" : "play", fe, thrs[t], off,
                  over_ref, over_new, n_hits_ref, n_hits_new);
        }
    }
}

/* ------------------------------------------------------------ histogram -- */

/* the histogram part of hist_build() in src/zebra.c */
static void new_hist_build(struct Histogram * h, int mz, int off)
{
    struct ovk_geom g;
    struct ovk_yuv2rgb t = { yuv2rgb_RV, yuv2rgb_GU, yuv2rgb_GV, yuv2rgb_BU };
    overlay_geom(&g, lv_buf, 0, 0, os.x0);
    h->total_px = ovk_hist(&g, os.x0, os.x_max, os.y0 + off, os.y_max - off, mz,
                           &h->hist, h->is_rgb ? &t : 0);
    h->max = ovk_hist_max(h->hist);
}

static void test_histogram(const struct geometry * geo)
{
    for (int f = 0; f < FRAME_COUNT; f++)
    {
        make_frame(f, 99 + f);
        for (int rgb = 0; rgb < 2; rgb++)
        for (int mz = 0; mz < 2; mz++)
        for (int off = 0; off <= 40; off += 40)
        {
            struct Histogram a, b;
            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));
            a.is_rgb = b.is_rgb = rgb;
            ref_hist_build((uint32_t *) lv_buf, &a, 1, mz, off);
            new_hist_build(&b, mz, off);
            CHECK(memcmp(&a, &b, sizeof(a)) == 0, "histogram %s frame=%s rgb=%d mz=%d off=%d: total %u vs %u, max %u vs %u",
                  geo->name, frame_names[f], rgb, mz, off, a.total_px, b.total_px, a.max, b.max);
        }
    }
}

/* ---------------------------------------------------------------- bench -- */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static void found_nop(int x, int y, int e, int thr) { (void) x; (void) y; (void) e; (void) thr; }
static int hit_nop(void * ctx, int x, int y, int e) { (void) ctx; (void) x; (void) y; (void) e; return 0; }

/* parameters of the kernel being timed */
static int b_thr, b_fe, b_cs, b_rgb;

static void ref_peak(void)  { dirty_pixels_num = 0; ref_focus_scan((uintptr_t) lv_buf, 1, 1, b_thr, b_fe, 0, found_nop); }
static void new_peak(void)
{
    struct ovk_peak pk = { .lv = lv_buf, .lv_pitch = vram_lv.pitch, .sy = bm2lv.sy, .ty = bm2lv.ty,
        .xoff = &bm_lv_x_cache[8 - BMP_W_MINUS], .x0 = 8, .x1 = 712, .xstep = 2,
        .y0 = 8, .y1 = 472, .ystep = 3, .filter_edges = b_fe, .thr = b_thr };
    ovk_peak_scan(&pk, hit_nop, 0);
}
static void ref_zeb(void)   { ref_draw_zebras_soft(BMP_ORIGIN(bmp_ref), BMP_ORIGIN(mir_ref), lv_buf, b_cs, 95, 5, 0); }
static void new_zeb(void)   { new_draw_zebras(BMP_ORIGIN(bmp_new), BMP_ORIGIN(mir_new), b_cs, 95, 5, 0); }
static void ref_hist(void)  { struct Histogram h; memset(&h, 0, sizeof(h)); h.is_rgb = b_rgb; ref_hist_build((uint32_t *) lv_buf, &h, 1, 0, 0); }
static void new_hist(void)  { struct Histogram h; memset(&h, 0, sizeof(h)); h.is_rgb = b_rgb; new_hist_build(&h, 0, 0); }

static void bench_one(const char * label, void (*ref)(void), void (*new)(void))
{
    const int iters = 200;
    double best_r = 1e30, best_n = 1e30;
    for (int rep = 0; rep < 5; rep++)
    {
        double t0 = now_ns();
        for (int it = 0; it < iters; it++) ref();
        double t1 = now_ns();
        for (int it = 0; it < iters; it++) new();
        double t2 = now_ns();
        if (t1 - t0 < best_r) best_r = t1 - t0;
        if (t2 - t1 < best_n) best_n = t2 - t1;
    }
    printf("  %-30s %10.0f %10.0f   x%.2f\n", label, best_r / iters, best_n / iters, best_r / best_n);
}

static int bench(void)
{
    set_geometry(&geometries[0]);
    make_frame(FRAME_PHOTO, 5);
    printf("host micro-benchmark, %s, photo-like frame; ns/frame (best of 5), indicative only\n", geometries[0].name);
    printf("  %-30s %10s %10s\n", "kernel", "original", "new");

    /* focus peaking: threshold where ~1% of samples are hits, as the auto-threshold aims for */
    b_thr = 15;
    while (b_thr < 1000 && ref_focus_scan((uintptr_t) lv_buf, 1, 1, b_thr, 0, 0, found_nop) > 352 * 155 / 100) b_thr++;
    for (int x = 8; x < 712; x++) bm_lv_x_cache[x - BMP_W_MINUS] = BM2LV_X(x) * 2 + 1;
    for (b_fe = 0; b_fe <= 1; b_fe++)
    {
        char label[64];
        snprintf(label, sizeof(label), "peaking LV fe=%d (thr %d)", b_fe, b_thr);
        bench_one(label, ref_peak, new_peak);
    }

    for (b_cs = 0; b_cs < 2; b_cs++)
    {
        make_bmp_state(0, 1);
        ref_zeb(); new_zeb();   /* steady state: our zebras are already on screen */
        bench_one(b_cs ? "zebras rgb (steady state)" : "zebras luma (steady state)", ref_zeb, new_zeb);
    }

    for (b_rgb = 0; b_rgb < 2; b_rgb++)
        bench_one(b_rgb ? "histogram rgb" : "histogram luma", ref_hist, new_hist);
    return 0;
}

int main(int argc, char ** argv)
{
    precompute_yuv2rgb(1);      /* EOSM: CONFIG_REC709 (internals-common.h) */

    if (argc > 1 && strcmp(argv[1], "bench") == 0) return bench();

    for (int rec709 = 1; rec709 >= 0; rec709--)
    {
        precompute_yuv2rgb(rec709);
        for (unsigned i = 0; i < sizeof(geometries) / sizeof(geometries[0]); i++)
        {
            set_geometry(&geometries[i]);
            test_zebras(&geometries[i]);
            if (rec709)         /* luma only: no need to repeat with other tables */
            {
                test_peaking(&geometries[i]);
            }
            test_histogram(&geometries[i]);
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
