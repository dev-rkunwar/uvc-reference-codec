/* common/p3.h - UVC_P3 shared core (Implicit Neural Representation scaffold).
 *
 * UVC_P3 in the spec is an "Implicit Neural Representation" (MNeRV-family):
 * a small parallel MLP that maps pixel coordinates (x,y) -> occupancy/colour,
 * so the "bitstream" is the network weights, not a per-pixel array. That needs
 * a neural runtime this integer-only scaffold does not have. The verifiable
 * Tier-1-faithful analogue we can actually build, test, and keep deterministic
 * is a *multi-resolution coordinate-hash permutation* (MLHB-style hash table):
 *
 *   - A deterministic integer hash maps each pixel coordinate (x,y) to a hash
 *     slot; filling the slots in hash order yields a scrambled, quasi-random
 *     byte stream that is much harder to compress than the spatial layout
 *     (i.e. it forces the entropy coder to do the heavy lifting, exactly the
 *     role INR fills vs. a plain transform). The permutation is *content
 *     adaptive*: the hash seed is derived from a checksum of the frame, so the
 *     mapping changes per frame, like a frame-specific INR network.
 *   - The scrambled array is then INT8-quantized and rANS-coded, exactly like
 *     P1/P2, so it slots into the same container/self-test unchanged.
 *
 * Shared by encoder/p3.c and decoder/p3.c. Integer-only; decode is
 * bit-reproducible across platforms (spec §13) when scale_fp == 32768.
 */
#ifndef UVC_P3_H
#define UVC_P3_H

#include <stdint.h>
#include <stddef.h>
#include "quant.h"
#include "rans.h"
#include "bitstream.h"

/* Build the entropy distribution over the INT8-quantized hashed samples.
 * Symbols map int8 q in [-128,127] -> q+128 (0..255), exactly like P1/P2.
 * Returns 0 / -1. */
int uvc_p3_build_dist(const int32_t *q, size_t n, rans_dist_t *d);

/* Encode one frame: coordinate-hash permutation -> INT8 quant -> rANS.
 * Writes up to cap bytes to out. Returns bytes written, or -1 on overflow.
 * scale_fp is the quant scale (Q1.15). The permutation seed is content-derived
 * and written in a 4-byte header so the decoder can invert it. */
int uvc_p3_encode_frame(const int16_t *frame, int w, int h,
                        uint16_t scale_fp, uint8_t *out, int cap);

/* Decode one P3-encoded frame (seed header + rans symbols) back into `frame`.
 * Returns 0 on success, -1 on error. scale_fp must match the encoder. */
int uvc_p3_decode_frame(const uint8_t *buf, int len, int w, int h,
                        uint16_t scale_fp, int16_t *frame);

#endif /* UVC_P3_H */
