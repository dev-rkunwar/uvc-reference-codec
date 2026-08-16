/* decoder/p2.c - UVC_P2 decode pipeline (integer wavelet, spec §9 P2 scaffold).
 *
 * Mirrors decoder/p1.c: read the 32-byte frequency header, rebuild the rANS
 * distribution, entropy-decode the byte stream (two nibbles per coefficient),
 * reconstruct each int8 coefficient, then INT8 dequant + integer inverse DWT
 * back to the pixel/block domain. Integer-only.
 */
#include "p2.h"
#include <stdlib.h>

int uvc_p2_decode_frame(const uint8_t *buf, int len, int w, int h, int level,
                        uint16_t scale_fp, int16_t *frame) {
    if (w <= 0 || h <= 0 || level < 1 || len < 32) return -1;
    size_t n = (size_t)w * h;

    /* Header: 16 frequency counts, 16 bits each (32 bytes). */
    uint32_t counts[16];
    bitreader_t br;
    br_init(&br, buf, (size_t)len);
    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        if (br_get(&br, 16, &v) != 0) return -1;
        counts[i] = v;
    }
    rans_dist_t d;
    if (rans_dist_build(&d, counts, 16) != 0) return -1;

    /* Symbols: 2 nibbles per coefficient. */
    const uint8_t *sym = buf + 32;
    int sym_len = len - 32;
    rans_dec_t dec;
    rans_dec_init(&dec, sym, sym_len);

    int32_t *qall = malloc(n * sizeof(int32_t));
    if (!qall) return -1;
    for (size_t i = 0; i < n; i++) {
        int lo = rans_dec_get(&dec, &d);
        int hi = rans_dec_get(&dec, &d);
        if (lo < 0 || hi < 0) { free(qall); return -1; }
        int u = (hi << 4) | (lo & 0xF);     /* unsigned 0..255 */
        qall[i] = (int32_t)(u - 128);        /* back to int8 q */
    }

    /* Inverse: dequant + iDWT over the whole frame. */
    int32_t *coeff = malloc(n * sizeof(int32_t));
    if (!coeff) { free(qall); return -1; }
    for (size_t i = 0; i < n; i++) coeff[i] = dequant_i8((int8_t)qall[i], scale_fp);

    uvc_p2_idwt(coeff, w, h, level, frame);

    free(qall);
    free(coeff);
    return 0;
}
