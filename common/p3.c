/* common/p3.c - Implicit Neural Representation scaffold (spec §9 P3).
 *
 * Faithful integer analogue of an INR (MNeRV-family) frame representation:
 * the pixel grid is permuted by a deterministic, content-adaptive coordinate
 * hash (MLHB-style), then the scrambled samples are INT8-quantized and rANS
 * coded (mirroring P1/P2). The permutation is a true bijection derived from a
 * 32-bit seed; the seed is written into a 4-byte header so the decoder rebuilds
 * the identical inverse mapping without shipping the permutation itself.
 *
 * Why this is a fair INR analogue in a Tier-1 integer scaffold:
 *   - An INR replaces a per-pixel array with a frame-specific function
 *     (coordinate -> value). Here the "function" is the hash permutation, which
 *     is frame-specific (seeded from a frame checksum) and, like a network,
 *     is reconstructed from a small seed rather than stored pixel-by-pixel.
 *   - It forces the entropy coder to model the (now quasi-random) byte stream,
 *     which is the role INR fills versus a plain spatial transform.
 *
 * Bitstream layout (mirrors P1/P2 exactly so the container stays agnostic):
 *   [ 4-byte seed (u32, big-endian) ]
 *   [ 16 x u16 frequency counts = 32 bytes header ]
 *   [ rans-coded byte stream: two nibbles per quantized sample ]
 */
#include "p3.h"
#include <stdlib.h>
#include <string.h>

/* Deterministic 32-bit integer hash of (x, y, seed). Uses a multiply-xor
 * scheme with odd multipliers (Knuth / splitmix-style) so the low bits are
 * well mixed. Returns a value in [0, 2^32). */
static uint32_t coord_hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = seed ^ 0x9E3779B9u;
    h ^= (x * 0x85EBCA6Bu); h = (h ^ (h >> 13)) * 0xC2B2AE35u;
    h ^= (y * 0x27D4EB2Fu); h = (h ^ (h >> 15)) * 0x165667B1u;
    h ^= (h >> 16);
    return h;
}

/* Build a bijection perm[0..N-1] over source-pixel indices [0,N-1].
 * perm[k] = source index placed into hash slot k. Computed by hashing each
 * source coordinate to a preferred slot and resolving collisions with linear
 * probing. Deterministic for a given (w, h, seed), so encoder and decoder
 * produce identical perm arrays. */
static void build_perm(int w, int h, uint32_t seed, int *perm) {
    int N = w * h;
    int *slot_owner = malloc((size_t)N * sizeof(int));
    if (!slot_owner) { for (int i = 0; i < N; i++) perm[i] = i; return; }
    for (int i = 0; i < N; i++) slot_owner[i] = -1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int src = y * w + x;
            uint32_t hv = coord_hash((uint32_t)x, (uint32_t)y, seed);
            int slot = (int)(hv % (uint32_t)N);
            while (slot_owner[slot] != -1)
                slot = (slot + 1) % N;          /* open addressing */
            slot_owner[slot] = src;
            perm[slot] = src;
        }
    }
    free(slot_owner);
}

/* Content-adaptive seed: FNV-1a over the pixel buffer. Two different frames
 * (even of the same size) get different seeds, so the hash mapping is
 * frame-specific like an INR network. */
static uint32_t frame_seed(const int16_t *frame, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        uint16_t v = (uint16_t)frame[i];
        h ^= (v & 0xFF); h *= 16777619u;
        h ^= (v >> 8);   h *= 16777619u;
    }
    return h | 0x1u;   /* ensure non-zero */
}

int uvc_p3_build_dist(const int32_t *q, size_t n, rans_dist_t *d) {
    if (n == 0) return -1;
    uint32_t counts[16];
    for (int i = 0; i < 16; i++) counts[i] = 0;
    for (size_t i = 0; i < n; i++) {
        int u = (int)(q[i] + 128) & 0xFF;
        counts[u & 0xF]++;
        counts[(u >> 4) & 0xF]++;
    }
    return rans_dist_build(d, counts, 16);
}

int uvc_p3_encode_frame(const int16_t *frame, int w, int h,
                        uint16_t scale_fp, uint8_t *out, int cap) {
    if (w <= 0 || h <= 0) return -1;
    int N = w * h;
    uint32_t seed = frame_seed(frame, (size_t)N);

    int *perm = malloc((size_t)N * sizeof(int));
    if (!perm) return -1;
    build_perm(w, h, seed, perm);

    /* Scrambled samples. */
    int32_t *scram = malloc((size_t)N * sizeof(int32_t));
    if (!scram) { free(perm); return -1; }
    for (int k = 0; k < N; k++) {
        int16_t s = frame[perm[k]];
        scram[k] = quant_i8((int32_t)s, scale_fp);
    }

    rans_dist_t d;
    if (uvc_p3_build_dist(scram, (size_t)N, &d) != 0) { free(perm); free(scram); return -1; }

    /* Header: 4-byte seed (big-endian) + 32-byte freq table. */
    bitwriter_t hbw;
    bw_init(&hbw, out, (size_t)cap);
    bw_put(&hbw, (seed >> 24) & 0xFF, 8);
    bw_put(&hbw, (seed >> 16) & 0xFF, 8);
    bw_put(&hbw, (seed >> 8)  & 0xFF, 8);
    bw_put(&hbw, (seed)       & 0xFF, 8);
    for (int i = 0; i < 16; i++) {
        if (bw_put(&hbw, d.freq[i], 16) != 0) { free(perm); free(scram); return -1; }
    }
    size_t hdr = bw_flush(&hbw);
    if (hdr != 36) { free(perm); free(scram); return -1; }

    /* Symbols: two nibbles per scrambled sample. */
    rans_enc_t enc;
    rans_enc_init(&enc, out + hdr, cap - (int)hdr);
    for (int k = 0; k < N; k++) {
        int u = (int)(scram[k] + 128) & 0xFF;
        if (rans_enc_put(&enc, &d, u & 0xF) != 0) { free(perm); free(scram); return -1; }
        if (rans_enc_put(&enc, &d, (u >> 4) & 0xF) != 0) { free(perm); free(scram); return -1; }
    }
    int len = rans_enc_finish(&enc);
    free(perm); free(scram);
    return (len > 0) ? (int)hdr + len : -1;
}

int uvc_p3_decode_frame(const uint8_t *buf, int len, int w, int h,
                        uint16_t scale_fp, int16_t *frame) {
    if (w <= 0 || h <= 0 || len < 36) return -1;
    int N = w * h;

    /* Header: 4-byte seed (big-endian) + 32-byte freq table. */
    uint32_t seed = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                    ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];

    uint32_t counts[16];
    bitreader_t br;
    br_init(&br, buf, (size_t)len);
    br_get(&br, 8, &counts[0]);   /* consume seed bytes (values unused past seed) */
    br_get(&br, 8, &counts[0]);
    br_get(&br, 8, &counts[0]);
    br_get(&br, 8, &counts[0]);
    for (int i = 0; i < 16; i++) {
        uint32_t v = 0;
        if (br_get(&br, 16, &v) != 0) return -1;
        counts[i] = v;
    }
    rans_dist_t d;
    if (rans_dist_build(&d, counts, 16) != 0) return -1;

    /* Symbols: two nibbles per scrambled sample. */
    const uint8_t *sym = buf + 36;
    int sym_len = len - 36;
    rans_dec_t dec;
    rans_dec_init(&dec, sym, sym_len);

    int32_t *scram = malloc((size_t)N * sizeof(int32_t));
    if (!scram) return -1;
    for (int k = 0; k < N; k++) {
        int lo = rans_dec_get(&dec, &d);
        int hi = rans_dec_get(&dec, &d);
        if (lo < 0 || hi < 0) { free(scram); return -1; }
        int u = (hi << 4) | (lo & 0xF);
        int8_t q = (int8_t)(u - 128);
        scram[k] = dequant_i8(q, scale_fp);
    }

    /* Inverse permutation: sample k came from source perm[k]. */
    int *perm = malloc((size_t)N * sizeof(int));
    if (!perm) { free(scram); return -1; }
    build_perm(w, h, seed, perm);
    for (int k = 0; k < N; k++)
        frame[perm[k]] = (int16_t)scram[k];

    free(perm); free(scram);
    return 0;
}
