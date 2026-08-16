/* common/p4.c - Semantic / Task layer scaffold (spec §9 P4).
 *
 * Faithful integer analogue of a VCM/PAT-VCM semantic layer: a 2x-downsampled
 * coarse token map (block-mean over 2x2) -> INT4-quantized tokens -> rANS.
 * Lossy and low-rate by design (structure, not detail). Decode upsamples
 * (nearest) to the full frame. The header carries the P4 model hash so the
 * decoder can enforce the spec's model-hash gate (tier>=2 AND held hash).
 *
 * Bitstream layout (mirrors P1/P2/P3 so the container stays agnostic):
 *   [ 4-byte model hash (u32, big-endian) ]
 *   [ 16 x u16 frequency counts = 32 bytes header ]
 *   [ rans-coded token stream: one INT4 symbol per coarse pixel ]
 * The coarse grid is (w/2) x (h/2) (w,h are forced even by the caller).
 */
#include "p4.h"
#include <stdlib.h>
#include <string.h>

int uvc_p4_build_dist(const int32_t *q, size_t n, rans_dist_t *d) {
    if (n == 0) return -1;
    uint32_t counts[16];
    for (int i = 0; i < 16; i++) counts[i] = 0;
    for (size_t i = 0; i < n; i++) {
        int u = (int)(q[i] + 8) & 0xF;   /* int4 [-8,7] -> 0..15 */
        counts[u]++;
    }
    return rans_dist_build(d, counts, 16);
}

int uvc_p4_encode_frame(const int16_t *frame, int w, int h,
                        uint16_t scale_fp, uint8_t *out, int cap) {
    if (w <= 0 || h <= 0) return -1;
    int cw = w / 2, ch = h / 2;          /* coarse grid */
    if (cw == 0 || ch == 0) return -1;
    size_t nc = (size_t)cw * ch;

    /* 2x downsample: mean of each 2x2 block. */
    int32_t *coarse = malloc(nc * sizeof(int32_t));
    if (!coarse) return -1;
    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++) {
            int s = (int)frame[(2*cy)*w + (2*cx)] + (int)frame[(2*cy)*w + (2*cx+1)] +
                    (int)frame[(2*cy+1)*w + (2*cx)] + (int)frame[(2*cy+1)*w + (2*cx+1)];
            coarse[cy * cw + cx] = s / 4;     /* integer mean */
        }

    /* INT4-quantize the coarse means. */
    int32_t *q = malloc(nc * sizeof(int32_t));
    if (!q) { free(coarse); return -1; }
    for (size_t i = 0; i < nc; i++) q[i] = quant_i4(coarse[i], scale_fp);

    rans_dist_t d;
    if (uvc_p4_build_dist(q, nc, &d) != 0) { free(coarse); free(q); return -1; }

    /* Header: 4-byte model hash + 32-byte freq table. */
    bitwriter_t hbw;
    bw_init(&hbw, out, (size_t)cap);
    bw_put(&hbw, (UVC_P4_MODEL_HASH >> 24) & 0xFF, 8);
    bw_put(&hbw, (UVC_P4_MODEL_HASH >> 16) & 0xFF, 8);
    bw_put(&hbw, (UVC_P4_MODEL_HASH >> 8)  & 0xFF, 8);
    bw_put(&hbw, (UVC_P4_MODEL_HASH)       & 0xFF, 8);
    for (int i = 0; i < 16; i++) {
        if (bw_put(&hbw, d.freq[i], 16) != 0) { free(coarse); free(q); return -1; }
    }
    size_t hdr = bw_flush(&hbw);
    if (hdr != 36) { free(coarse); free(q); return -1; }

    /* Symbols: one INT4 token per coarse pixel. */
    rans_enc_t enc;
    rans_enc_init(&enc, out + hdr, cap - (int)hdr);
    for (size_t i = 0; i < nc; i++) {
        int u = (int)(q[i] + 8) & 0xF;
        if (rans_enc_put(&enc, &d, u) != 0) { free(coarse); free(q); return -1; }
    }
    int len = rans_enc_finish(&enc);
    free(coarse); free(q);
    return (len > 0) ? (int)hdr + len : -1;
}

int uvc_p4_decode_frame(const uint8_t *buf, int len, int w, int h,
                        uint16_t scale_fp, int16_t *frame) {
    if (w <= 0 || h <= 0 || len < 36) return -1;
    int cw = w / 2, ch = h / 2;
    if (cw == 0 || ch == 0) return -1;
    size_t nc = (size_t)cw * ch;

    /* Header: 4-byte model hash + 32-byte freq table. */
    uint32_t hash = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                    ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
    if (hash != UVC_P4_MODEL_HASH) return -1;   /* model mismatch -> refuse */

    uint32_t counts[16];
    bitreader_t br;
    br_init(&br, buf, (size_t)len);
    br_get(&br, 8, &counts[0]); br_get(&br, 8, &counts[0]);
    br_get(&br, 8, &counts[0]); br_get(&br, 8, &counts[0]);
    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        if (br_get(&br, 16, &v) != 0) return -1;
        counts[i] = v;
    }
    rans_dist_t d;
    if (rans_dist_build(&d, counts, 16) != 0) return -1;

    /* Symbols: one INT4 token per coarse pixel. */
    const uint8_t *sym = buf + 36;
    int sym_len = len - 36;
    rans_dec_t dec;
    rans_dec_init(&dec, sym, sym_len);

    int32_t *coarse = malloc(nc * sizeof(int32_t));
    if (!coarse) return -1;
    for (size_t i = 0; i < nc; i++) {
        int s = rans_dec_get(&dec, &d);
        if (s < 0) { free(coarse); return -1; }
        int8_t qt = (int8_t)((int8_t)((s & 0xF) << 4) >> 4);   /* sign-extend */
        coarse[i] = dequant_i4(qt, scale_fp);
    }

    /* Upsample (nearest) back to full resolution. */
    for (int y = 0; y < h; y++) {
        int cy = y / 2;
        for (int x = 0; x < w; x++) {
            int cx = x / 2;
            frame[y * w + x] = (int16_t)coarse[cy * cw + cx];
        }
    }

    free(coarse);
    return 0;
}
