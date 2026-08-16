/* common/chroma.c - Color space and chroma subsampling utilities.
 * Integer-only, spec §3.2 compliant. Used by Milestone A (color/chroma path).
 */
#include "chroma.h"
#include "p1.h"
#include <stdlib.h>
#include <string.h>

/* RGB -> YCbCr (BT.601 full-range, integer fixed-point Q15).
 * Y  = 0.299*R + 0.587*G + 0.114*B
 * Cb = -0.168736*R - 0.331264*G + 0.5*B + 128
 * Cr = 0.5*R - 0.418688*G - 0.081312*B + 128
 * Coefficients scaled by 32768 (Q15). All math in int32. */
static const int32_t Y_R  = 9798;   /* 0.299 * 32768 */
static const int32_t Y_G  = 19235;  /* 0.587 * 32768 */
static const int32_t Y_B  = 3735;   /* 0.114 * 32768 */
static const int32_t CB_R = -5528;  /* -0.168736 * 32768 */
static const int32_t CB_G = -10855; /* -0.331264 * 32768 */
static const int32_t CB_B = 16384;  /* 0.5 * 32768 */
static const int32_t CR_R = 16384;  /* 0.5 * 32768 */
static const int32_t CR_G = -13718; /* -0.418688 * 32768 */
static const int32_t CR_B = -2662;  /* -0.081312 * 32768 */

#define Q 15
#define RND (1 << (Q - 1))

int uvc_rgb_to_ycbcr(const uint8_t *r, const uint8_t *g, const uint8_t *b,
                     int w, int h, int chroma_fmt,
                     int16_t *y, int16_t *cb, int16_t *cr) {
    int nplanes = (chroma_fmt == UVC_CHROMA_MONO) ? 1 : 3;
    int cw = (chroma_fmt == UVC_CHROMA_420 || chroma_fmt == UVC_CHROMA_422) ? (w + 1) / 2 : w;
    int ch = (chroma_fmt == UVC_CHROMA_420) ? (h + 1) / 2 : h;

    /* Full-resolution Y from all RGB pixels. */
    for (int i = 0; i < w * h; i++) {
        int32_t Y = (Y_R * r[i] + Y_G * g[i] + Y_B * b[i] + RND) >> Q;
        if (Y < 0) Y = 0; if (Y > 255) Y = 255;
        y[i] = (int16_t)Y;
    }

    if (nplanes == 1) return 1;

    /* Subsample Cb/Cr: average 2x2 (420) or 2x1 (422) blocks. */
    int cb_idx = 0;
    for (int by = 0; by < h; by += (chroma_fmt == UVC_CHROMA_420 ? 2 : 1)) {
        for (int bx = 0; bx < w; bx += 2) {
            int32_t sum_cb = 0, sum_cr = 0;
            int count = 0;
            for (int dy = 0; dy < (chroma_fmt == UVC_CHROMA_420 ? 2 : 1); dy++) {
                if (by + dy >= h) continue;
                for (int dx = 0; dx < 2; dx++) {
                    if (bx + dx >= w) continue;
                    int idx = (by + dy) * w + (bx + dx);
                    sum_cb += CB_R * r[idx] + CB_G * g[idx] + CB_B * b[idx];
                    sum_cr += CR_R * r[idx] + CR_G * g[idx] + CR_B * b[idx];
                    count++;
                }
            }
            int32_t Cb = ((sum_cb / count) + RND) >> Q;
            int32_t Cr = ((sum_cr / count) + RND) >> Q;
            Cb = (Cb + 128); if (Cb < 0) Cb = 0; if (Cb > 255) Cb = 255;
            Cr = (Cr + 128); if (Cr < 0) Cr = 0; if (Cr > 255) Cr = 255;
            cb[cb_idx] = (int16_t)Cb;
            cr[cb_idx] = (int16_t)Cr;
            cb_idx++;
        }
    }
    return 3;
}

int uvc_ycbcr_to_rgb(const int16_t *y, const int16_t *cb, const int16_t *cr,
                     int w, int h, int chroma_fmt,
                     uint8_t *r, uint8_t *g, uint8_t *b) {
    int nplanes = (chroma_fmt == UVC_CHROMA_MONO) ? 1 : 3;
    if (nplanes == 1) {
        for (int i = 0; i < w * h; i++) {
            int Y = y[i];
            r[i] = g[i] = b[i] = (uint8_t)(Y < 0 ? 0 : (Y > 255 ? 255 : Y));
        }
        return 1;
    }

    /* Upsample Cb/Cr to full resolution. */
    int cw = (w + 1) / 2;
    int ch = (chroma_fmt == UVC_CHROMA_420) ? (h + 1) / 2 : h;
    int cb_idx = 0;
    int cb_full[w * h];
    int cr_full[w * h];

    for (int by = 0; by < h; by++) {
        int src_y = (chroma_fmt == UVC_CHROMA_420) ? by / 2 : by;
        if (src_y >= ch) src_y = ch - 1;
        for (int bx = 0; bx < w; bx++) {
            int src_x = bx / 2;
            if (src_x >= cw) src_x = cw - 1;
            cb_full[by * w + bx] = cb[src_y * cw + src_x];
            cr_full[by * w + bx] = cr[src_y * cw + src_x];
        }
    }

    /* BT.601 inverse: R = Y + 1.402*(Cr-128); G = Y - 0.344*(Cb-128) - 0.714*(Cr-128); B = Y + 1.772*(Cb-128) */
    static const int32_t R_CR = 45934;  /* 1.402 * 32768 */
    static const int32_t G_CB = -11282; /* -0.344 * 32768 */
    static const int32_t G_CR = -23400; /* -0.714 * 32768 */
    static const int32_t B_CB = 58050;  /* 1.772 * 32768 */

    for (int i = 0; i < w * h; i++) {
        int Y = y[i];
        int Cb = cb_full[i] - 128;
        int Cr = cr_full[i] - 128;
        int32_t R = ((int32_t)Y << Q) + R_CR * Cr;
        int32_t G = ((int32_t)Y << Q) + G_CB * Cb + G_CR * Cr;
        int32_t B = ((int32_t)Y << Q) + B_CB * Cb;
        r[i] = (uint8_t)(R < 0 ? 0 : (R > (255 << Q) ? 255 : (R >> Q)));
        g[i] = (uint8_t)(G < 0 ? 0 : (G > (255 << Q) ? 255 : (G >> Q)));
        b[i] = (uint8_t)(B < 0 ? 0 : (B > (255 << Q) ? 255 : (B >> Q)));
    }
    return 3;
}

int uvc_alloc_planes(int w, int h, int chroma_fmt, int16_t **y, int16_t **cb, int16_t **cr) {
    int nplanes = (chroma_fmt == UVC_CHROMA_MONO) ? 1 : 3;
    int cw = (chroma_fmt == UVC_CHROMA_420 || chroma_fmt == UVC_CHROMA_422) ? (w + 1) / 2 : w;
    int ch = (chroma_fmt == UVC_CHROMA_420) ? (h + 1) / 2 : h;

    *y = malloc((size_t)w * h * sizeof(int16_t));
    if (!*y) return -1;
    if (nplanes > 1) {
        *cb = malloc((size_t)cw * ch * sizeof(int16_t));
        *cr = malloc((size_t)cw * ch * sizeof(int16_t));
        if (!*cb || !*cr) {
            free(*y);
            if (*cb) free(*cb);
            if (*cr) free(*cr);
            return -1;
        }
    } else {
        *cb = *cr = NULL;
    }
    return 0;
}

void uvc_free_planes(int16_t *y, int16_t *cb, int16_t *cr) {
    if (y) free(y);
    if (cb) free(cb);
    if (cr) free(cr);
}

int uvc_p1_encode_chroma_frame(const int16_t *const *plane_frames,
                               int nplanes, int w, int h, int chroma_fmt,
                               uint16_t scale_fp, uint8_t *out, int cap) {
    int cw = (chroma_fmt == UVC_CHROMA_420 || chroma_fmt == UVC_CHROMA_422) ? (w + 1) / 2 : w;
    int ch = (chroma_fmt == UVC_CHROMA_420) ? (h + 1) / 2 : h;

    int pos = 0;
    int per_plane_cap = cap / nplanes;
    for (int p = 0; p < nplanes; p++) {
        int pw = (p == 0) ? w : cw;
        int ph = (p == 0) ? h : ch;
        int len = uvc_p1_encode_frame(plane_frames[p], pw, ph, scale_fp,
                                      out + pos, per_plane_cap);
        if (len < 0) return -1;
        pos += len;
    }
    return pos;
}

int uvc_p1_decode_chroma_frame(const uint8_t *const *plane_frames,
                               const int *plane_lens, int nplanes,
                               int w, int h, int chroma_fmt,
                               uint16_t scale_fp, int16_t *out_y,
                               int16_t *out_cb, int16_t *out_cr) {
    int cw = (chroma_fmt == UVC_CHROMA_420 || chroma_fmt == UVC_CHROMA_422) ? (w + 1) / 2 : w;
    int ch = (chroma_fmt == UVC_CHROMA_420) ? (h + 1) / 2 : h;

    int rc = uvc_p1_decode_frame(plane_frames[0], plane_lens[0], w, h, scale_fp, out_y);
    if (rc != 0) return -1;
    if (nplanes > 1) {
        rc = uvc_p1_decode_frame(plane_frames[1], plane_lens[1], cw, ch, scale_fp, out_cb);
        if (rc != 0) return -1;
        rc = uvc_p1_decode_frame(plane_frames[2], plane_lens[2], cw, ch, scale_fp, out_cr);
        if (rc != 0) return -1;
    }
    return 0;
}