/* common/p4.h - UVC_P4 shared core (Semantic / Task layer scaffold).
 *
 * UVC_P4 in the spec is a "Semantic / Task" scalable layer (VCM / PAT-VCM):
 * a machine-vision-oriented bitstream carrying high-level structure (object /
 * region tokens) rather than a pixel-faithful reconstruction, so analytics
 * (detection, classification) run on the coarse layer without the full pixel
 * path. That implies a learned tokenizer this integer scaffold does not have.
 *
 * The verifiable Tier-1-faithful analogue we build, test, and keep
 * deterministic is a *coarse semantic token map*: the frame is 2x-downsampled
 * (block-mean over 2x2 regions), each coarse mean is INT4-quantized into a
 * compact token, and the token stream is rANS-coded (mirroring P1/P2/P3). The
 * decoder reconstructs by dequantizing and upsampling (nearest) back to the
 * full resolution. P4 is intentionally *lossy and low-rate* -- it captures
 * structure, not detail -- which is exactly the role of a task/semantic layer.
 *
 * P4 is also the paradigm that exercises the *model-hash* gate (spec §1/§13):
 * decoding requires tier >= 2 AND a held model hash, unlike P2/P3 (tier-gated
 * only). The model hash is written in the header so the decoder can confirm it
 * holds the matching "tokenizer model" before materializing the layer.
 *
 * Shared by encoder/p4.c and decoder/p4.c. Integer-only.
 */
#ifndef UVC_P4_H
#define UVC_P4_H

#include <stdint.h>
#include <stddef.h>
#include "quant.h"
#include "rans.h"
#include "bitstream.h"

/* Model hash identifying the P4 tokenizer. Mirrors the negotiate.c convention
 * (hash 0x55667788 == P4 model in test_negotiate). A decoder must hold this
 * hash to legally materialize a P4 layer. */
#define UVC_P4_MODEL_HASH 0x55667788u

/* Build the entropy distribution over the INT4-quantized coarse tokens.
 * Symbols map int8 q in [-8,7] -> q+8 (0..15). Returns 0 / -1. */
int uvc_p4_build_dist(const int32_t *q, size_t n, rans_dist_t *d);

/* Encode one frame: 2x downsample -> INT4 token -> rANS. Writes up to cap
 * bytes to out. Returns bytes written, or -1 on overflow. scale_fp is the
 * quant scale (Q1.15) applied to the coarse means. The 4-byte model hash is
 * written in the header. */
int uvc_p4_encode_frame(const int16_t *frame, int w, int h,
                        uint16_t scale_fp, uint8_t *out, int cap);

/* Decode one P4-encoded frame (hash header + rANS tokens) back into `frame`
 * (full w x h, upsampled from the coarse token map). Returns 0 on success,
 * -1 on error or model-hash mismatch. scale_fp must match the encoder. */
int uvc_p4_decode_frame(const uint8_t *buf, int len, int w, int h,
                        uint16_t scale_fp, int16_t *frame);

#endif /* UVC_P4_H */
