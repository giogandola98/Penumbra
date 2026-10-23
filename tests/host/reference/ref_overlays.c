/* Golden reference: the original overlay pixel loops, copied verbatim from
 * src/zebra.c, src/imgconv.c and src/vram.c as of upstream 142472e (before the
 * overlay_kernels refactor). Only the surrounding function headers were
 * changed so that the camera state comes in as parameters / shim globals;
 * every pixel loop body is untouched. Do not "fix" or optimise this file. */

#include "ref_shim.h"

struct trans2d bm2lv;
struct vram_info vram_lv;
struct os_t os;
int bm2lv_x_cache[BMP_W_PLUS - BMP_W_MINUS];
int yuv2rgb_RV[256];
int yuv2rgb_GU[256];
int yuv2rgb_GV[256];
int yuv2rgb_BU[256];

#define EXT_MONITOR_RCA 0

/* ------------------------------------------------------ src/imgconv.c */

void precompute_yuv2rgb(int rec709)
{
if (rec709) {
    /*
    *R = *Y + 1608 * V / 1024;
    *G = *Y -  191 * U / 1024 - 478 * V / 1024;
    *B = *Y + 1900 * U / 1024;
    */
    for (int u = 0; u < 256; u++)
    {
        int8_t U = u;
        yuv2rgb_GU[u] = (-191 * U) >> 10;
        yuv2rgb_BU[u] = (1900 * U) >> 10;
    }

    for (int v = 0; v < 256; v++)
    {
        int8_t V = v;
        yuv2rgb_RV[v] = (1608 * V) >> 10;
        yuv2rgb_GV[v] = (-478 * V) >> 10;
    }
} else { // REC 601
    /*
    *R = *Y + ((1437 * V) >> 10);
    *G = *Y -  ((352 * U) >> 10) - ((731 * V) >> 10);
    *B = *Y + ((1812 * U) >> 10);
    */
    for (int u = 0; u < 256; u++)
    {
        int8_t U = u;
        yuv2rgb_GU[u] = (-352 * U) >> 10;
        yuv2rgb_BU[u] = (1812 * U) >> 10;
    }

    for (int v = 0; v < 256; v++)
    {
        int8_t V = v;
        yuv2rgb_RV[v] = (1437 * V) >> 10;
        yuv2rgb_GV[v] = (-731 * V) >> 10;
    }
}
}

void little_cleanup(void* BP, void* MP)
{
    uint8_t* bp = BP; uint8_t* mp = MP;
    if (*bp != 0 && *bp == *mp) *mp = *bp = 0;
    bp++; mp++;
    if (*bp != 0 && *bp == *mp) *mp = *bp = 0;
    bp++; mp++;
    if (*bp != 0 && *bp == *mp) *mp = *bp = 0;
    bp++; mp++;
    if (*bp != 0 && *bp == *mp) *mp = *bp = 0;
}

/* --------------------------------------------------------- src/vram.c */

void vram_update_luts()
{
    for (int x = BMP_W_MINUS; x < BMP_W_PLUS; x++)
    {
        bm2lv_x_cache[x - BMP_W_MINUS] = BM2LV_Xu(x);
        //~ bm2n_x_cache[x - BMP_W_MINUS] = BM2N_Xu(x);
    }
}

/* -------------------------------------------------------- src/zebra.c */

int zebra_color_word_row(int c, int y)
{
    if (!c) return 0;

    uint32_t cw = 0;
    switch(y % 4)
    {
        case 0:
            cw  = c  | c  << 8;
            break;
        case 1:
            cw  = c << 8 | c << 16;
            break;
        case 2:
            cw = c  << 16 | c << 24;
            break;
        case 3:
            cw  = c  << 24 | c ;
            break;
    }
    return cw;
}

#define ZEBRA_COLOR_WORD_SOLID(x) ( (x) | (x)<<8 | (x)<<16 | (x)<<24 )
int zebra_rgb_color(int underexposed, int clipR, int clipG, int clipB, int y)
{
    if (underexposed) return zebra_color_word_row(79, y);

    switch ((clipR ? 4 : 0) |
            (clipG ? 2 : 0) |
            (clipB ? 1 : 0))
    {
        case 0b111: return ZEBRA_COLOR_WORD_SOLID(COLOR_BLACK);
        case 0b110: return ZEBRA_COLOR_WORD_SOLID(COLOR_YELLOW);
        case 0b101: return ZEBRA_COLOR_WORD_SOLID(COLOR_MAGENTA);
        case 0b011: return ZEBRA_COLOR_WORD_SOLID(COLOR_CYAN);
        case 0b100: return y&2 ? 0 : ZEBRA_COLOR_WORD_SOLID(COLOR_RED);
        case 0b001: return y&2 ? 0 : ZEBRA_COLOR_WORD_SOLID(COLOR_BLUE);
        case 0b010: return y&2 ? 0 : ZEBRA_COLOR_WORD_SOLID(COLOR_GREEN2);
        default: return 0;
    }
}

/* draw_zebras(), software luma / RGB path (zebra_colorspace 0 / 1) */
void ref_draw_zebras_soft(uint8_t * bvram, uint8_t * bvram_mirror, uint8_t * lvram,
                          int zebra_colorspace, int zebra_level_hi, int zebra_level_lo, int off)
{
        int zlh = zebra_level_hi * 255 / 100 - 1;
        int zll = zebra_level_lo * 255 / 100;

        int zlr = zlh;
        int zlg = zlh;
        int zlb = zlh;

        // draw zebra in 16:9 frame
        // y is in BM coords
        for(int y = os.y0 + off; y < os.y_max - off; y += 2 )
        {
            #define color_over           zebra_color_word_row(COLOR_RED,  y)
            #define color_under          zebra_color_word_row(COLOR_BLUE, y)
            #define color_over_2         zebra_color_word_row(COLOR_RED,  y+1)
            #define color_under_2        zebra_color_word_row(COLOR_BLUE, y+1)

            #define color_rgb_under      zebra_rgb_color(1, 0, 0, 0, y)
            #define color_rgb_under_2    zebra_rgb_color(1, 0, 0, 0, y+1)

            #define color_rgb_clipR      zebra_rgb_color(0, 1, 0, 0, y)
            #define color_rgb_clipR_2    zebra_rgb_color(0, 1, 0, 0, y+1)
            #define color_rgb_clipG      zebra_rgb_color(0, 0, 1, 0, y)
            #define color_rgb_clipG_2    zebra_rgb_color(0, 0, 1, 0, y+1)
            #define color_rgb_clipB      zebra_rgb_color(0, 0, 0, 1, y)
            #define color_rgb_clipB_2    zebra_rgb_color(0, 0, 0, 1, y+1)

            #define color_rgb_clipRG     zebra_rgb_color(0, 1, 1, 0, y)
            #define color_rgb_clipRG_2   zebra_rgb_color(0, 1, 1, 0, y+1)
            #define color_rgb_clipGB     zebra_rgb_color(0, 0, 1, 1, y)
            #define color_rgb_clipGB_2   zebra_rgb_color(0, 0, 1, 1, y+1)
            #define color_rgb_clipRB     zebra_rgb_color(0, 1, 0, 1, y)
            #define color_rgb_clipRB_2   zebra_rgb_color(0, 1, 0, 1, y+1)

            #define color_rgb_clipRGB    zebra_rgb_color(0, 1, 1, 1, y)
            #define color_rgb_clipRGB_2  zebra_rgb_color(0, 1, 1, 1, y+1)

            uint32_t * const v_row = (uint32_t*)( lvram        + BM2LV_R(y)    );  // 2 pixels
            uint32_t * const b_row = (uint32_t*)( bvram        + BM_R(y)       );  // 4 pixels
            uint32_t * const m_row = (uint32_t*)( bvram_mirror + BM_R(y)       );  // 4 pixels

            uint32_t* lvp; // that's a moving pointer through lv vram
            uint32_t* bp;  // through bmp vram
            uint32_t* mp;  // through mirror

            for (int x = os.x0; x < os.x_max; x += 4)
            {
                lvp = v_row + (BM2LV_X(x) >> 1);
                bp = b_row + (x >> 2);
                mp = m_row + (x >> 2);
                #define BP (*bp)
                #define MP (*mp)
                #define BN (*(bp + BMPPITCH/4))
                #define MN (*(mp + BMPPITCH/4))
                if (BP != 0 && BP != MP) { little_cleanup(bp, mp); continue; }
                if (BN != 0 && BN != MN) { little_cleanup(bp + (BMPPITCH >> 2), mp + (BMPPITCH >> 2)); continue; }
                if ((MP & 0x80808080) || (MN & 0x80808080)) continue;

                if (zebra_colorspace == 1 && !EXT_MONITOR_RCA) // rgb
                {
                    int Y, R, G, B;
                    //~ uyvy2yrgb(*lvp, &Y, &R, &G, &B);
                    COMPUTE_UYVY2YRGB(*lvp, Y, R, G, B);

                    if(unlikely(Y < zll)) // underexposed
                    {
                        BP = MP = color_rgb_under;
                        BN = MN = color_rgb_under_2;
                    }
                    else
                    {
                        //~ BP = MP = zebra_rgb_color(Y < zll, R > zlh, G > zlh, B > zlh, y);
                        //~ BN = MN = zebra_rgb_color(Y < zll, R > zlh, G > zlh, B > zlh, y+1);

                        if (unlikely(R > zlr)) // R clipped
                        {
                            if (unlikely(G > zlg)) // RG clipped
                            {
                                if (B > zlb) // RGB clipped (all of them)
                                {
                                    BP = MP = color_rgb_clipRGB;
                                    BN = MN = color_rgb_clipRGB_2;
                                }
                                else // only R and G clipped
                                {
                                    BP = MP = color_rgb_clipRG;
                                    BN = MN = color_rgb_clipRG_2;
                                }
                            }
                            else // R clipped, G not clipped
                            {
                                if (unlikely(B > zlb)) // only R and B clipped
                                {
                                    BP = MP = color_rgb_clipRB;
                                    BN = MN = color_rgb_clipRB_2;
                                }
                                else // only R clipped
                                {
                                    BP = MP = color_rgb_clipR;
                                    BN = MN = color_rgb_clipR_2;
                                }
                            }
                        }
                        else // R not clipped
                        {
                            if (unlikely(G > zlg)) // R not clipped, G clipped
                            {
                                if (unlikely(B > zlb)) // only G and B clipped
                                {
                                    BP = MP = color_rgb_clipGB;
                                    BN = MN = color_rgb_clipGB_2;
                                }
                                else // only G clipped
                                {
                                    BP = MP = color_rgb_clipG;
                                    BN = MN = color_rgb_clipG_2;
                                }
                            }
                            else // R not clipped, G not clipped
                            {
                                if (unlikely(B > zlb)) // only B clipped
                                {
                                    BP = MP = color_rgb_clipB;
                                    BN = MN = color_rgb_clipB_2;
                                }
                                else // nothing clipped
                                {
                                    BN = MN = BP = MP = 0;
                                }
                            }
                        }
                    }
                }
                else // luma
                {
                    int p0 = (*lvp) >> 8 & 0xFF;
                    if (unlikely(p0 > zlh))
                    {
                        BP = MP = color_over;
                        BN = MN = color_over_2;
                    }
                    else if (unlikely(p0 < zll))
                    {
                        BP = MP = color_under;
                        BN = MN = color_under_2;
                    }
                    else
                        BN = MN = BP = MP = 0;
                }

                #undef MP
                #undef BP
                #undef BN
                #undef MN
            }
        }
}

/* focus peaking (draw_zebra_and_focus, dot mode) */
static int focus_peaking_filter_edges;
int dirty_pixels_num;
static uint16_t bm_lv_x_cache[BMP_W_PLUS - BMP_W_MINUS];

static void zebra_update_lut()
{
    static int prev_bm2lv_sx = 0;
    int rebuild = 0;

    if(unlikely(prev_bm2lv_sx != bm2lv.sx))
    {
        prev_bm2lv_sx = bm2lv.sx;
        rebuild = 1;
    }

    if(unlikely(rebuild))
    {
        int xStart = os.x0 + 8;
        int xEnd = os.x_max - 8;

        for (int x = xStart; x < xEnd; x += 1)
        {
            bm_lv_x_cache[x - BMP_W_MINUS] = BM2LV_X(x) * 2 + 1;
        }
    }
}

static inline int FAST calc_peak(const uint8_t* p8, const int pitch)
{
    // approximate second derivative with a Laplacian kernel:
    //     -1
    //  -1  4 -1
    //     -1
    const int p8_xmin1 = (int)(*(p8 - 2));
    const int p8_xplus1 = (int)(*(p8 + 2));
    const int p8_ymin1 = (int)(*(p8 - pitch));
    const int p8_yplus1 = (int)(*(p8 + pitch));

    int result = ((int)(*p8) * 4);
    result -= p8_xplus1 + p8_xmin1 + p8_yplus1 + p8_ymin1;

    int e = ABS(result);

    if (focus_peaking_filter_edges)
    {
        // filter out strong edges where first derivative is strong
        // as these are usually false positives
        int d1x = ABS(p8_xplus1 - p8_xmin1);
        int d1y = ABS(p8_yplus1 - p8_ymin1);
        int d1 = MAX(d1x, d1y);
        e = MAX(e - ((d1 << focus_peaking_filter_edges) >> 2), 0) * 2;
    }
    return e;
}

static inline int FAST peak_d2xy(const uint8_t* p8)
{
    return calc_peak(p8, vram_lv.pitch);
}

/* the scan loops; focus_found_pixel*() is replaced by found() */
/* FAST like draw_zebra_and_focus(), which holds these loops in zebra.c */
int FAST ref_focus_scan(uintptr_t vram, int lv, int F, int thr, int filter_edges, int off, ref_found_fn found)
{
        focus_peaking_filter_edges = filter_edges;
        int yStart = os.y0 + off + 8;
        int yEnd = os.y_max - off - 8;
        int xStart = os.x0 + 8;
        int xEnd = os.x_max - 8;
        int n_over = 0;

        const uint8_t* p8; // that's a moving pointer

        zebra_update_lut();

        int n_total = 0;
        if (lv) // fast, realtime
        {
            n_total = ((yEnd - yStart) * (xEnd - xStart)) / 6;
            for(int y = yStart; y < yEnd; y += 3)
            {
                uintptr_t row = vram + BM2LV_R(y); /* host: uint32_t on the camera */

                for (int x = xStart; x < xEnd; x += 2)
                {
                    p8 = (uint8_t *)(uintptr_t)(row + bm_lv_x_cache[x - BMP_W_MINUS]);

                    int e = peak_d2xy(p8);

                    /* executed for 1% of pixels */
                    if (unlikely(e >= thr))
                    {
                        n_over++;
                        if (unlikely(dirty_pixels_num >= MAX_DIRTY_PIXELS)) break; // threshold too low, abort
                        found(x, y, e, thr);
                    }
                }
            }
        }
        else // playback - can be slower and more accurate
        {
            n_total = ((yEnd - yStart) * (xEnd - xStart));
            for(int y = yStart; y < yEnd; y ++)
            {
                uintptr_t row = vram + BM2LV_R(y); /* host: uint32_t on the camera */

                for (int x = xStart; x < xEnd; x ++)
                {
                    p8 = (uint8_t *)(uintptr_t)(row + bm_lv_x_cache[x - BMP_W_MINUS]);
                    int e = peak_d2xy(p8);

                    /* executed for 1% of pixels */
                    if (unlikely(e >= thr))
                    {
                        n_over++;

                        if (unlikely(dirty_pixels_num >= MAX_DIRTY_PIXELS)) // threshold too low, abort
                            break;

                        if (F==1) found(x, y, e, thr);
                    }
                }
            }
        }
        (void) n_total;
        return n_over;
}

/* histogram (hist_build, YUV part) */
static struct Histogram histogram;

static void hist_add_pixel(uint32_t pixel, int Y)
{
    if (histogram.is_rgb)
    {
        int R, G, B;
        //~ uyvy2yrgb(pixel, &Y, &R, &G, &B);
        COMPUTE_UYVY2YRGB(pixel, Y, R, G, B);
        // YRGB range: 0-255
        uint32_t R_level = (R * HIST_WIDTH) >> 8;
        uint32_t G_level = (G * HIST_WIDTH) >> 8;
        uint32_t B_level = (B * HIST_WIDTH) >> 8;

        histogram.hist_r[R_level & (HIST_WIDTH-1)]++;
        histogram.hist_g[G_level & (HIST_WIDTH-1)]++;
        histogram.hist_b[B_level & (HIST_WIDTH-1)]++;
    }

    /* luma component is always computed, since we need histogram.max */
    /* and it's much less expensive than RGB anyway */
    histogram.total_px++;
    uint32_t hist_level = (Y * HIST_WIDTH) >> 8;

    // Ignore the 0 bin.  It generates too much noise
    unsigned count = ++ (histogram.hist[ hist_level & (HIST_WIDTH-1)]);
    if( hist_level && count > histogram.max )
        histogram.max = count;
}

void ref_hist_build(uint32_t * buf, struct Histogram * h, int hist_draw, int mz, int off)
{
    int x, y;
    histogram = *h;     /* memset + is_rgb / is_raw set up by the caller */

    for( y = os.y0 + off; y < os.y_max - off; y += 2 )
    {
        for( x = os.x0 ; x < os.x_max ; x += 2 )
        {
            uint32_t pixel = buf[BM2LV(x,y) >> 2];

            // ignore magic zoom borders
            if (mz && (pixel == MZ_WHITE || pixel == MZ_BLACK || pixel == MZ_GREEN))
                continue;

            int Y = UYVY_GET_AVG_Y(pixel);

            if (hist_draw && !histogram.is_raw)
            {
                hist_add_pixel(pixel, Y);
            }
        }
    }
    *h = histogram;
}
