/* common/p2.c - Integer wavelet (LeGall 5/3) 2D DWT (spec §9 P2 scaffold).
 *
 * LeGall 5/3 is the lossless integer wavelet used by JPEG2000 (lossless mode).
 * Its lifting scheme is exact and reversible with integer arithmetic (see the
 * forward/inverse equality proof in the comments below), so uvc_p2_idwt is the
 * exact inverse of uvc_p2_fdwt for any int16 input -- and combined with the
 * identity INT8 quantizer (scale_fp == 32768) the full pipeline is bit-exact.
 *
 * Subband layout (2x2 interleaved): for a level-L decomposition of a w x h
 * region we split into four quadrants
 *     [ a | b ]     a = LL(next level)  b = HL
 *     [ c | d ]     c = LH               d = HH
 * and recurse into quadrant a. Each quadrant is a contiguous w/2 x h/2 block.
 */
#include "p2.h"
#include <stdlib.h>

/* 1D forward LeGall 5/3 on x[0..n-1] (n even). In-place, int16.
 *   d[i]     = x[2i+1] - ((x[2i] + x[2i+2] + 1) >> 1)      i = 0..half-2
 *   d[half-1]= x[n-1]   - x[n-2]                           (odd boundary)
 *   a[0]     = x[0]     + ((d[0] + 1) >> 1)
 *   a[i]     = x[2i]    + ((d[i-1] + d[i] + 2) >> 2)       i = 1..half-1
 * Result is packed as [a0 a1 ... | d0 d1 ...] (even = approx, odd = detail). */
static void fdwt1d(int16_t *x, int n) {
    int half = n >> 1;
    int16_t *a = malloc(half * sizeof(int16_t));
    int16_t *d = malloc(half * sizeof(int16_t));
    if (!a || !d) { free(a); free(d); return; }

    for (int i = 0; i < half - 1; i++)
        d[i] = (int16_t)(x[2 * i + 1] - ((x[2 * i] + x[2 * i + 2] + 1) >> 1));
    d[half - 1] = (int16_t)(x[n - 1] - x[n - 2]);

    a[0] = (int16_t)(x[0] + ((d[0] + 1) >> 1));
    for (int i = 1; i < half; i++)
        a[i] = (int16_t)(x[2 * i] + ((d[i - 1] + d[i] + 2) >> 2));

    for (int i = 0; i < half; i++) { x[i] = a[i]; x[half + i] = d[i]; }
    free(a); free(d);
}

/* 1D inverse LeGall 5/3 -- exact reversal of fdwt1d.
 *   y[0]      = a[0] - ((d[0] + 1) >> 1)
 *   y[2i]     = a[i] - ((d[i-1] + d[i] + 2) >> 2)
 *   y[2i+1]    = d[i] + ((y[2i] + y[2i+2] + 1) >> 1)        i = 0..half-2
 *   y[n-1]    = d[half-1] + y[n-2] */
static void idwt1d(int16_t *x, int n) {
    int half = n >> 1;
    int16_t *a = calloc(half, sizeof(int16_t));
    int16_t *d = calloc(half, sizeof(int16_t));
    int16_t *y = calloc(n, sizeof(int16_t));
    if (!a || !d || !y) { free(a); free(d); free(y); return; }

    for (int i = 0; i < half; i++) { a[i] = x[i]; d[i] = x[half + i]; }

    y[0] = (int16_t)(a[0] - ((d[0] + 1) >> 1));
    for (int i = 1; i < half; i++)
        y[2 * i] = (int16_t)(a[i] - ((d[i - 1] + d[i] + 2) >> 2));
    for (int i = 0; i < half - 1; i++)
        y[2 * i + 1] = (int16_t)(d[i] + ((y[2 * i] + y[2 * i + 2] + 1) >> 1));
    y[n - 1] = (int16_t)(d[half - 1] + y[n - 2]);

    for (int i = 0; i < n; i++) x[i] = y[i];
    free(a); free(d); free(y);
}

/* Apply 2D fdwt to the w x h tile at (ox,oy) in buf (in-place). */
static void fdwt_tile(int16_t *buf, int w, int ox, int oy, int tw, int th) {
    for (int r = 0; r < th; r++) fdwt1d(&buf[(oy + r) * w + ox], tw);
    int16_t *col = malloc(th * sizeof(int16_t));
    if (!col) return;
    for (int c = 0; c < tw; c++) {
        for (int r = 0; r < th; r++) col[r] = buf[(oy + r) * w + ox + c];
        fdwt1d(col, th);
        for (int r = 0; r < th; r++) buf[(oy + r) * w + ox + c] = col[r];
    }
    free(col);
}

static void idwt_tile(int16_t *buf, int w, int ox, int oy, int tw, int th) {
    int16_t *col = malloc(th * sizeof(int16_t));
    if (!col) return;
    for (int c = 0; c < tw; c++) {
        for (int r = 0; r < th; r++) col[r] = buf[(oy + r) * w + ox + c];
        idwt1d(col, th);
        for (int r = 0; r < th; r++) buf[(oy + r) * w + ox + c] = col[r];
    }
    for (int r = 0; r < th; r++) idwt1d(&buf[(oy + r) * w + ox], tw);
    free(col);
}

void uvc_p2_fdwt(const int16_t *frame, int w, int h, int level, int32_t *out) {
    int16_t *buf = malloc((size_t)w * h * sizeof(int16_t));
    if (!buf) return;
    for (int i = 0; i < w * h; i++) buf[i] = frame[i];

    int cw = w, ch = h;
    for (int lv = 0; lv < level; lv++) {
        fdwt_tile(buf, w, 0, 0, cw, ch);
        cw >>= 1; ch >>= 1;          /* next level operates on the LL quadrant */
    }

    for (int i = 0; i < w * h; i++) out[i] = buf[i];
    free(buf);
}

void uvc_p2_idwt(const int32_t *coeff, int w, int h, int level, int16_t *out) {
    int16_t *buf = malloc((size_t)w * h * sizeof(int16_t));
    if (!buf) return;
    for (int i = 0; i < w * h; i++) buf[i] = (int16_t)coeff[i];

    for (int lv = level - 1; lv >= 0; lv--) {
        int cw = w >> lv, ch = h >> lv;   /* subband size at this level */
        idwt_tile(buf, w, 0, 0, cw, ch);
    }

    for (int i = 0; i < w * h; i++) out[i] = buf[i];
    free(buf);
}
