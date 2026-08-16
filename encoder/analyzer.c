/* encoder/analyzer.c - Heuristic content analyzer (spec §9 scaffold)
 *
 * Integer-only. Complexity stored as fixed-point Q8.8 (value = real*256).
 * Replaces the earlier float/sqrt version that pulled the broken
 * api-ms-win-crt-math-l1-1-0.dll forwarder on this host.
 */
#include "analyzer.h"
#include <string.h>
#include <stdlib.h>

#define Q8 8
#define FP(x) ((int16_t)((x) * 256))

/* Spatial complexity: normalized luminance variance, expressed as Q8.8.
 * Variance can reach ~16384 (stddev 128). We report stddev/128 clamped to [0,1]
 * without a sqrt by using integer stddev then a fixed divide. */
static int16_t spatial_complex_q8(const uint8_t *luma, int w, int h) {
    uint64_t s = 0;
    int n = w * h;
    for (int i = 0; i < n; i++) s += luma[i];
    uint64_t mean = s / (uint64_t)n;            /* 0..255 */
    uint64_t var_sum = 0;
    for (int i = 0; i < n; i++) {
        int64_t d = (int64_t)luma[i] - (int64_t)mean;
        var_sum += (uint64_t)(d * d);
    }
    uint64_t var = var_sum / (uint64_t)n;       /* 0..16384 */
    /* stddev ~ sqrt(var); approximate integer stddev by var>>(log2 scale).
     * Use sqrt-free scaling: report (var >> 6) clamped to 256 (== 1.0 in Q8.8).
     * var 16384 -> 256 (1.0); var 4096 -> 64 (0.25). Good enough for routing. */
    uint64_t v = var >> 6;
    if (v > 256) v = 256;
    return (int16_t)v;                           /* already Q8.8 (<=256) */
}

void uvc_analyze_frame(const uint8_t *luma, int w, int h,
                       uvc_content_profile_t *out) {
    memset(out, 0, sizeof(*out));
    int16_t sp = spatial_complex_q8(luma, w, h);
    out->spatial_complex = sp;
    if (sp < FP(0.08f)) {
        out->scene_type = UVC_SCENE_SCREEN;     /* flat -> likely UI/text */
        out->texture_rich = 0;
    } else if (sp > FP(0.60f)) {
        out->scene_type = UVC_SCENE_NATURAL;
        out->texture_rich = 2;                  /* detailed */
    } else {
        out->scene_type = UVC_SCENE_NATURAL;
        out->texture_rich = 1;
    }
    out->motion_complex   = UVC_MOTION_SLOW;
    out->target_use       = UVC_USE_HUMAN;
    out->quality_pref     = UVC_QUALITY_BALANCE;
    out->temporal_complex = 0;
    out->semantic_complex = 0;
}

void uvc_analyze_segment(const uint8_t **frames_luma, int nframes,
                         int w, int h, uvc_content_profile_t *out) {
    uint64_t sp_sum = 0;
    uint64_t tm_sum = 0;
    int valid_t = nframes > 1 ? nframes - 1 : 1;
    for (int f = 0; f < nframes; f++) {
        uvc_content_profile_t p;
        uvc_analyze_frame(frames_luma[f], w, h, &p);
        sp_sum += (uint64_t)p.spatial_complex;
        if (f > 0) {
            /* crude temporal complexity: mean abs diff, scaled to Q8.8. */
            uint64_t d = 0;
            int n = w * h;
            for (int i = 0; i < n; i++)
                d += (uint64_t)abs((int)frames_luma[f][i] - (int)frames_luma[f - 1][i]);
            /* mean diff 0..255 -> Q8.8 = (d/n)*256; /64 clamps typical motion */
            tm_sum += ((d / (uint64_t)n) * 256) >> 6;
        }
    }
    memset(out, 0, sizeof(*out));
    out->spatial_complex = (int16_t)(sp_sum / (uint64_t)nframes);
    out->temporal_complex = (int16_t)(tm_sum / (uint64_t)valid_t);
    if (out->spatial_complex < FP(0.08f))      out->scene_type = UVC_SCENE_SCREEN;
    else if (out->spatial_complex > FP(0.60f)) out->scene_type = UVC_SCENE_NATURAL;
    else                                       out->scene_type = UVC_SCENE_NATURAL;
    if (out->temporal_complex > FP(0.30f))      out->motion_complex = UVC_MOTION_FAST;
    else if (out->temporal_complex > FP(0.10f)) out->motion_complex = UVC_MOTION_SLOW;
    else                                        out->motion_complex = UVC_MOTION_STATIC;
    out->texture_rich    = out->spatial_complex > FP(0.30f) ? 2 : 1;
    out->target_use      = UVC_USE_HUMAN;
    out->quality_pref    = UVC_QUALITY_BALANCE;
    out->semantic_complex = 0;
}
