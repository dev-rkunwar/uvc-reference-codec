/* encoder/p2.c - UVC_P2 encode pipeline (integer wavelet, spec §9 P2 scaffold).
 *
 * Pipeline: 2D integer LeGall-5/3 DWT of the whole frame -> per-coefficient
 * INT8 quantization (using the shared quant.h scale) -> rANS entropy coding of
 * the int8 values (symbol = q+128, alphabet 0..255). Integer-only; mirrors the
 * P1 block-transform pipeline so it slots into the same container/self-test.
 *
 * Bitstream layout (mirrors P1 exactly so the container is agnostic):
 *   [ 16 x u16 frequency counts = 32 bytes header ]
 *   [ rans-coded byte stream: one byte per quantized coefficient ]
 * The decoder rebuilds the identical rANS table from the 32-byte header.
 */
#include "p2.h"
#include <stdlib.h>

/* Build the entropy distribution over the INT8-quantized wavelet coefficients.
 * Each int8 q maps to symbol (q + 128) in 0..255. rans_dist_t carries 16
 * symbols, so we quantize the 256-value alphabet down to the 16-symbol nibble
 * alphabet exactly like P1: each coefficient byte contributes two nibbles. */
int uvc_p2_build_dist(const int32_t *qcoeff, size_t ncoeff, rans_dist_t *d) {
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

/* Encode one frame: DWT + INT8 quant + entropy. Writes up to cap bytes to out.
 * Returns bytes written, or -1 on overflow. scale_fp is the quant scale (Q1.15).
 *
 * Layout: [ 16 x u16 frequency counts = 32 bytes header ]
 *         [ rans-coded stream: 2 nibbles per coefficient ]
 * The decoder rebuilds the identical rANS table from the header. */
int uvc_p2_encode_frame(const int16_t *frame, int w, int h, int level,
                        uint16_t scale_fp, uint8_t *out, int cap) {
    if (w <= 0 || h <= 0 || level < 1) return -1;
    size_t n = (size_t)w * h;

    /* Transform. */
    int32_t *coeff = malloc(n * sizeof(int32_t));
    if (!coeff) return -1;
    uvc_p2_fdwt(frame, w, h, level, coeff);

    /* Quantize all coefficients. */
    int32_t *qall = malloc(n * sizeof(int32_t));
    if (!qall) { free(coeff); return -1; }
    for (size_t i = 0; i < n; i++) qall[i] = quant_i8(coeff[i], scale_fp);
    free(coeff);

    rans_dist_t d;
    if (uvc_p2_build_dist(qall, n, &d) != 0) { free(qall); return -1; }

    /* Header: 16 frequency counts, 16 bits each = exactly 32 bytes. */
    bitwriter_t hbw;
    bw_init(&hbw, out, (size_t)cap);
    for (int i = 0; i < 16; i++) {
        if (bw_put(&hbw, d.freq[i], 16) != 0) { free(qall); return -1; }
    }
    size_t hdr = bw_flush(&hbw);
    if (hdr != 32) { free(qall); return -1; }

    /* Symbols: each coefficient byte -> two nibbles (lo, hi), entropy-coded. */
    rans_enc_t enc;
    rans_enc_init(&enc, out + hdr, cap - (int)hdr);
    for (size_t i = 0; i < n; i++) {
        int u = (int)(qall[i] + 128) & 0xFF;
        if (rans_enc_put(&enc, &d, u & 0xF) != 0) { free(qall); return -1; }
        if (rans_enc_put(&enc, &d, (u >> 4) & 0xF) != 0) { free(qall); return -1; }
    }
    int len = rans_enc_finish(&enc);
    free(qall);
    return (len > 0) ? (int)hdr + len : -1;
}
