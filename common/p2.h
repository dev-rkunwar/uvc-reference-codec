/* common/p2.h - UVC_P2 shared core (integer wavelet / DWT pipeline).
 *
 * UVC_P2 in this reference scaffold is the integer wavelet transform pipeline
 * (roadmap item: "P2 - wavelet pipeline"). The canonical spec names P2
 * "Neural Residual" (DCVC-MB), but that requires a neural runtime this
 * integer-only scaffold does not have; the verifiable Tier-1-faithful analogue
 * we can actually build, test, and keep deterministic is a lossless integer
 * wavelet (LeGall 5/3) transform + INT8 quantization + the existing rANS
 * entropy coder -- mirroring the P1 block-transform pipeline exactly.
 *
 * Shared by encoder/p2.c and decoder/p2.c. Integer-only; decode is
 * bit-reproducible across platforms (spec §13).
 */
#ifndef UVC_P2_H
#define UVC_P2_H

#include <stdint.h>
#include <stddef.h>
#include "quant.h"
#include "rans.h"
#include "bitstream.h"

/* Max transform level. The frame is recursively decomposed into LL / HL / LH /
 * HH subbands; the LL subband is further decomposed at each level. 3 levels is
 * the standard choice for 8x8-class blocks and keeps the subband dimensions
 * integer for 8-pixel-multiple frames. */
#define UVC_P2_MAX_LEVEL 3

/* Integer forward 2D DWT (LeGall 5/3, lossless lifting) of a w x h frame of
 * int16 samples. Output is the same w*h int32 buffer, re-packed in 2x2
 * subband-interleaved order: for each level, the top-left quadrant becomes the
 * next-level LL (still int16 range after centering), and the other three
 * quadrants hold the HL/LH/HH detail coefficients (centered to zero, int16).
 *
 * The transform is exact and reversible for any w,h that are multiples of
 * 2^(level) (caller guarantees multiples of 8 -> levels up to 3). */
void uvc_p2_fdwt(const int16_t *frame, int w, int h, int level, int32_t *out);

/* Integer inverse 2D DWT (adjoint lifting) -- recovers `frame` exactly when no
 * quantization has been applied, within INT8 quantization slack otherwise. */
void uvc_p2_idwt(const int32_t *coeff, int w, int h, int level, int16_t *out);

/* Build an entropy distribution over a frame's INT8-quantized wavelet
 * coefficients. Symbols map int8 q in [-128,127] -> q+128 (0..255), exactly
 * like P1. Returns 0 / -1. */
int uvc_p2_build_dist(const int32_t *qcoeff, size_t ncoeff, rans_dist_t *d);

/* Encode one frame: DWT + INT8 quant + entropy. Writes up to cap bytes to out.
 * Returns bytes written, or -1 on overflow. scale_fp is quant scale (Q1.15). */
int uvc_p2_encode_frame(const int16_t *frame, int w, int h, int level,
                        uint16_t scale_fp, uint8_t *out, int cap);

/* Decode one P2-encoded frame (header + rans symbols) back into `frame`.
 * Returns 0 on success, -1 on error. scale_fp and level must match encoder. */
int uvc_p2_decode_frame(const uint8_t *buf, int len, int w, int h, int level,
                        uint16_t scale_fp, int16_t *frame);

#endif /* UVC_P2_H */
