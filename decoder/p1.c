/* decoder/p1.c - UVC_P1 decode pipeline (spec §9 P1).
 * Mirrors encoder/p1.c: read the 32-byte frequency header, rebuild the
 * canonical Huffman table, entropy-decode the nibble stream, reconstruct each
 * int8 coefficient from two nibbles, then INT8 dequant + integer IDCT back to
 * the pixel/block domain. Integer-only.
 */
#include "p1.h"
#include <stdlib.h>

int uvc_p1_decode_frame(const uint8_t *buf, int len, int w, int h,
                        uint16_t scale_fp, int16_t *frame) {
    int bw = (w / 8) * 8, bh = (h / 8) * 8;
    if (bw == 0 || bh == 0 || len < 32) return -1;
    int nblocks = (bw / 8) * (bh / 8);
    size_t ncoeff = (size_t)nblocks * 64;

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

    int32_t *qall = malloc(ncoeff * sizeof(int32_t));
    if (!qall) return -1;
    int qi = 0;
    for (size_t i = 0; i < ncoeff; i++) {
        int lo = rans_dec_get(&dec, &d);
        int hi = rans_dec_get(&dec, &d);
        if (lo < 0 || hi < 0) { free(qall); return -1; }
        int u = (hi << 4) | (lo & 0xF);     /* unsigned 0..255 */
        qall[qi++] = (int32_t)(u - 128);    /* back to int8 q */
    }

    /* Inverse: dequant + IDCT per block. */
    int bi = 0;
    for (int by = 0; by < bh; by += 8)
        for (int bx = 0; bx < bw; bx += 8) {
            int32_t coeff[64];
            for (int k = 0; k < 64; k++) coeff[k] = dequant_i8((int8_t)qall[bi + k], scale_fp);
            int16_t blk[64];
            uvc_p1_idct(coeff, blk);
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    frame[(by + r) * w + (bx + c)] = blk[r * 8 + c];
            bi += 64;
        }

    free(qall);
    return 0;
}
