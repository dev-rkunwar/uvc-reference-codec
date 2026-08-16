/* common/p1.h - UVC_P1 shared core (spec §9 P1: block transform).
 * UVC_P1 uses an integer 8x8 forward/inverse DCT (fixed-point, no float),
 * per-block INT8 quantization (spec §12), and entropy coding via the
 * existing rans_* API (common/rans.h). The math is integer-only so decode
 * is deterministic and bit-reproducible across platforms (spec §13).
 *
 * Shared by encoder/p1.c and decoder/p1.c.
 */
#ifndef UVC_P1_H
#define UVC_P1_H

#include <stdint.h>
#include <stddef.h>
#include "quant.h"
#include "rans.h"
#include "bitstream.h"

#define UVC_P1_BLOCK 8
#define UVC_P1_COEFFS (UVC_P1_BLOCK * UVC_P1_BLOCK)   /* 64 */

/* Integer 8x8 forward DCT (fdct) of a block of int16 samples.
 * Output coefficients are scaled integer DCT values (no normalization). */
void uvc_p1_fdct(const int16_t blk[UVC_P1_COEFFS], int32_t coeff[UVC_P1_COEFFS]);

/* Integer 8x8 inverse DCT (idct) of quantized coefficients back to int16. */
void uvc_p1_idct(const int32_t coeff[UVC_P1_COEFFS], int16_t blk[UVC_P1_COEFFS]);

/* Build an entropy distribution over a frame's INT8-quantized coefficients.
 * Symbols map int8 q in [-128,127] -> q+128 (0..255). Returns 0 / -1. */
int uvc_p1_build_dist(const int32_t *qcoeff, size_t ncoeff, rans_dist_t *d);

/* Encode one frame: DCT + INT8 quant + entropy. Writes up to cap bytes to out.
 * Returns bytes written, or -1 on overflow. scale_fp is quant scale (Q1.15). */
int uvc_p1_encode_frame(const int16_t *frame, int w, int h,
                        uint16_t scale_fp, uint8_t *out, int cap);

/* Decode one P1-encoded frame (header + rans symbols) back into `frame`.
 * Returns 0 on success, -1 on error. scale_fp must match the encoder. */
int uvc_p1_decode_frame(const uint8_t *buf, int len, int w, int h,
                        uint16_t scale_fp, int16_t *frame);

#endif /* UVC_P1_H */
