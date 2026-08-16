/* encoder/analyzer.h - Content analyzer interface (spec §9) */
#ifndef UVC_ANALYZER_H
#define UVC_ANALYZER_H

#include <stdint.h>

typedef enum {
    UVC_SCENE_NATURAL = 0,
    UVC_SCENE_SCREEN,
    UVC_SCENE_ANIMATION,
    UVC_SCENE_VOLUMETRIC,
    UVC_SCENE_SYNTHETIC,
    UVC_SCENE_LOWLIGHT,
    UVC_SCENE_COUNT
} uvc_scene_t;

typedef enum {
    UVC_MOTION_STATIC = 0,
    UVC_MOTION_SLOW,
    UVC_MOTION_FAST,
    UVC_MOTION_COMPLEX,
    UVC_MOTION_COUNT
} uvc_motion_t;

typedef enum {
    UVC_USE_HUMAN = 0,
    UVC_USE_MACHINE,
    UVC_USE_BOTH,
    UVC_USE_COUNT
} uvc_target_use_t;

typedef enum {
    UVC_QUALITY_SPEED = 0,
    UVC_QUALITY_BALANCE,
    UVC_QUALITY_BEST,
    UVC_QUALITY_PROGRESSIVE,
    UVC_QUALITY_COUNT
} uvc_quality_t;

/* ContentProfile as in spec §9.1 (kept bit-compatible).
 * Complexity fields are fixed-point Q8.8 (8 integer bits, 8 frac bits) so the
 * analyzer stays 100% integer/fixed-point (spec §13 determinism, no float CRT). */
typedef struct {
    uint8_t scene_type;
    uint8_t motion_complex;
    uint8_t texture_rich;
    uint8_t target_use;
    uint8_t quality_pref;
    int16_t spatial_complex;   /* Q8.8, 0..256 -> [0.0, 1.0] */
    int16_t temporal_complex;  /* Q8.8 */
    int16_t semantic_complex;  /* Q8.8 */
} uvc_content_profile_t;

/* Lightweight heuristic analyzer over a luminance buffer (grayscale, w*h).
 * This scaffold uses simple statistics instead of MobileNetV3; the interface
 * matches spec §9 so it can be swapped for the NN backbone later. */
void uvc_analyze_frame(const uint8_t *luma, int w, int h,
                       uvc_content_profile_t *out);

/* Analyze a segment (mean of per-frame profiles). prev may be NULL. */
void uvc_analyze_segment(const uint8_t **frames_luma, int nframes,
                         int w, int h, uvc_content_profile_t *out);

#endif /* UVC_ANALYZER_H */
