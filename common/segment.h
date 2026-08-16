/* common/segment.h - Per-segment encode/decode orchestration (spec §9/§10/§1).
 *
 * Closes two roadmap (#1) boxes at once:
 *   1. Bitstream header/signaling for paradigm + tier  -> the container `uvsh`
 *      box (written by uvc_mux_ex, read by uvc_demux / uvc_container_find_box).
 *   2. Per-segment paradigm selection wired through selector -> encoder ->
 *      decoder: uvc_encode_segment() runs the analyzer+selector to pick the
 *      paradigm set and required tier, then encodes each frame with the chosen
 *      codec; uvc_decode_segment() demuxes, negotiates the decoder's tier/model
 *      capabilities against the signaled set, and routes each frame to the
 *      matching pipeline (or to the P1 base layer fallback when the paradigm is
 *      unavailable).
 *
 * Everything is integer/fixed-point (spec §13) so the path stays deterministic.
 *
 * Scope note: only P1 (block DCT) and P2 (integer wavelet) have real codec
 * pipelines in this Tier-1 scaffold. P3 (INR) and P4 (semantic) are signaled
 * and negotiate correctly, but decode falls back to the P1 base layer (per spec
 * §7.3 / §1: "if NPU absent, skip P3, use P1"). The selection, signaling, and
 * capability negotiation are fully exercised and verifiable.
 */
#ifndef UVC_SEGMENT_H
#define UVC_SEGMENT_H

#include <stdint.h>
#include <stddef.h>

#include "../encoder/analyzer.h"
#include "../encoder/selector.h"
#include "../decoder/negotiate.h"
#include "container.h"

/* Per-segment encode parameters derived from the selector + caller intent. */
typedef struct {
    uint32_t paradigm_set;   /* bitmask UVC_P1_TRADITIONAL..UVC_P4_SEMANTIC */
    uint8_t  tier;           /* required decode tier for the highest paradigm */
    uint16_t scale_fp;       /* quant scale (Q1.15) used for every frame */
    int      p2_level;       /* DWT level if P2 is active (else unused) */
} uvc_segment_config_t;

/* Decide a segment config from content + encode targets. `compute` bounds which
 * paradigms the selector may enable; `target_use` selects the human/machine
 * path. Returns 0 on success. */
int uvc_plan_segment(const uvc_content_profile_t *prof,
                     int16_t target_bitrate_q8,
                     uvc_quality_t quality,
                     uvc_compute_t compute,
                     uvc_target_use_t target_use,
                     uint16_t scale_fp,
                     uvc_segment_config_t *cfg);

/* Encode a segment of `nframes` luminance frames (each w*h int16) into `out`.
 * Frames the selector routed to P2 use the wavelet pipeline; P1 is always the
 * base layer. Returns total container bytes, or -1 on overflow. */
int uvc_encode_segment(const int16_t *const *frames, int nframes, int w, int h,
                       const uvc_segment_config_t *cfg, uint8_t *out, int cap);

/* Decode a segment container. `cfg` describes the decoder's capabilities
 * (tier + held model hashes); frames whose paradigm the decoder cannot satisfy
 * fall back to the P1 base layer (per spec). Reconstructed frames are written
 * to rec[i] (each w*h int16, caller-allocated). Returns 0 on success, -1 on
 * parse error. The actually-decoded paradigm id per frame is written to
 * out_paradigm (may be NULL). */
int uvc_decode_segment(const uint8_t *buf, size_t len, int w, int h,
                       const uvc_decoder_config_t *cfg,
                       int16_t *const *rec, uint8_t *out_paradigm);

#endif /* UVC_SEGMENT_H */
