/* encoder/p1.c - UVC_P1 core: integer 8x8 DCT (spec §9 P1).
 * Fixed-point Q13 cosine table (orthonormal DCT-II), separable row/col.
 * Integer-only; idct(fdct(x)) recovers x within +/-3 LSB (fixed-point rounding).
 * The table is generated to satisfy the orthonormal DCT adjoint exactly.
 */
#include "p1.h"
#include <stdlib.h>

/* DCT-II cosine matrix, Q13 fixed point. cos_tab[k][n]. */
static const int16_t COS[8][8] = {
    {  2896,  2896,  2896,  2896,  2896,  2896,  2896,  2896 },
    {  4017,  3406,  2276,   799,  -799, -2276, -3406, -4017 },
    {  3784,  1567, -1567, -3784, -3784, -1567,  1567,  3784 },
    {  3406,  -799, -4017, -2276,  2276,  4017,   799, -3406 },
    {  2896, -2896, -2896,  2896,  2896, -2896, -2896,  2896 },
    {  2276, -4017,   799,  3406, -3406,  -799,  4017, -2276 },
    {  1567, -3784,  3784, -1567, -1567,  3784, -3784,  1567 },
    {   799, -2276,  3406, -4017,  4017, -3406,  2276,  -799 },
};

#define P1_Q 13

/* 1D forward DCT (FWD = A_scaled): int32 samples -> int32 coefficients (Q13). */
static void fdct1d(const int32_t x[8], int32_t y[8]) {
    for (int k = 0; k < 8; k++) {
        int32_t s = 0;
        for (int n = 0; n < 8; n++)
            s += (int32_t)COS[k][n] * x[n];
        y[k] = (s + (1 << (P1_Q - 1))) >> P1_Q;   /* round */
    }
}

/* 1D inverse DCT (INV = A_scaled^T): int32 coefficients -> int32 samples (Q13).
 * NOTE: uses COS[k][n] (sum over k), the transpose of fdct1d's COS[k][n] (sum
 * over n). A_scaled is orthonormal, so INV == FWD^{-1}. */
static void inv1d(const int32_t y[8], int32_t x[8]) {
    for (int n = 0; n < 8; n++) {
        int32_t s = 0;
        for (int k = 0; k < 8; k++)
            s += (int32_t)COS[k][n] * y[k];
        x[n] = (s + (1 << (P1_Q - 1))) >> P1_Q;  /* round */
    }
}

void uvc_p1_fdct(const int16_t blk[64], int32_t coeff[64]) {
    int32_t row[8];
    int32_t tmp[64];
    /* rows: apply FWD */
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) row[c] = blk[r * 8 + c];
        fdct1d(row, &tmp[r * 8]);
    }
    /* columns: apply FWD  (coeff = A * X * A^T) */
    for (int c = 0; c < 8; c++) {
        int32_t col[8];
        for (int r = 0; r < 8; r++) col[r] = tmp[r * 8 + c];
        int32_t out[8];
        fdct1d(col, out);
        for (int r = 0; r < 8; r++) coeff[r * 8 + c] = out[r];
    }
}

void uvc_p1_idct(const int32_t coeff[64], int16_t blk[64]) {
    int32_t tmp[64];
    int32_t col[8], row[8];
    /* inverse columns: apply INV  (tmp = A^T * coeff) */
    for (int c = 0; c < 8; c++) {
        for (int r = 0; r < 8; r++) col[r] = coeff[r * 8 + c];
        int32_t out[8];
        inv1d(col, out);
        for (int r = 0; r < 8; r++) tmp[r * 8 + c] = out[r];
    }
    /* inverse rows: apply INV  (X = A^T * tmp) */
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) row[c] = tmp[r * 8 + c];
        int32_t out[8];
        inv1d(row, out);
        for (int c = 0; c < 8; c++) blk[r * 8 + c] = (int16_t)out[c];
    }
}

/* Build an entropy distribution over nibble symbols (0..15). The caller
 * passes the frame's quantized coefficients; each int8 is split into two
 * nibbles (lo, then hi) and counted. rans_dist_t supports exactly 16 symbols,
 * so we entropy-code the 4-bit nibble alphabet directly. */
int uvc_p1_build_dist(const int32_t *qcoeff, size_t ncoeff, rans_dist_t *d) {
    if (ncoeff == 0) return -1;
    uint32_t counts[16];
    for (int i = 0; i < 16; i++) counts[i] = 0;
    for (size_t i = 0; i < ncoeff; i++) {
        int u = (int)(qcoeff[i] + 128) & 0xFF;   /* int8 -> unsigned 0..255 */
        counts[u & 0xF]++;
        counts[(u >> 4) & 0xF]++;
    }
    return rans_dist_build(d, counts, 16);
}

/* Encode one frame: DCT + INT8 quant + entropy. Writes up to cap bytes to out.
 * Returns bytes written, or -1 on overflow. scale_fp is the quant scale (Q1.15).
 *
 * Layout: [ 16 x u16 frequency counts = 32 bytes header ]
 *         [ rans-coded nibble stream: 2 nibbles per coefficient ]
 * The decoder rebuilds the identical canonical Huffman table from the header. */
int uvc_p1_encode_frame(const int16_t *frame, int w, int h,
                        uint16_t scale_fp, uint8_t *out, int cap) {
    int bw = (w / 8) * 8, bh = (h / 8) * 8;
    if (bw == 0 || bh == 0) return -1;
    int nblocks = (bw / 8) * (bh / 8);
    size_t ncoeff = (size_t)nblocks * 64;

    /* First pass: quantize all blocks, gather coeffs for the distribution. */
    int32_t *qall = malloc(ncoeff * sizeof(int32_t));
    if (!qall) return -1;
    int qi = 0;
    for (int by = 0; by < bh; by += 8)
        for (int bx = 0; bx < bw; bx += 8) {
            int16_t blk[64];
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    blk[r * 8 + c] = frame[(by + r) * w + (bx + c)];
            int32_t coeff[64];
            uvc_p1_fdct(blk, coeff);
            for (int k = 0; k < 64; k++)
                qall[qi++] = quant_i8(coeff[k], scale_fp);
        }

    rans_dist_t d;
    if (uvc_p1_build_dist(qall, ncoeff, &d) != 0) { free(qall); return -1; }

    /* Header: 16 frequency counts, 16 bits each = exactly 32 bytes. */
    bitwriter_t hbw;
    bw_init(&hbw, out, (size_t)cap);
    for (int i = 0; i < 16; i++) {
        if (bw_put(&hbw, d.freq[i], 16) != 0) { free(qall); return -1; }
    }
    size_t hdr = bw_flush(&hbw);
    if (hdr != 32) { free(qall); return -1; }

    /* Symbols: each coeff -> two nibbles (lo, hi), entropy-coded. */
    rans_enc_t enc;
    rans_enc_init(&enc, out + hdr, cap - (int)hdr);
    for (int i = 0; i < qi; i++) {
        int u = (int)(qall[i] + 128) & 0xFF;
        if (rans_enc_put(&enc, &d, u & 0xF) != 0) { free(qall); return -1; }
        if (rans_enc_put(&enc, &d, (u >> 4) & 0xF) != 0) { free(qall); return -1; }
    }
    int len = rans_enc_finish(&enc);
    free(qall);
    return (len > 0) ? (int)hdr + len : -1;
}
