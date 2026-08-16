/* encoder/selector.h - Paradigm selector (spec §10) */
#ifndef UVC_SELECTOR_H
#define UVC_SELECTOR_H

#include "analyzer.h"

typedef enum {
    UVC_TIER1 = 1,
    UVC_TIER2 = 2,
    UVC_TIER3 = 3
} uvc_tier_t;

/* Paradigm bit flags (spec §2.2). */
#define UVC_P1_TRADITIONAL (1u << 0)
#define UVC_P2_NEURAL      (1u << 1)
#define UVC_P3_INR         (1u << 2)
#define UVC_P4_SEMANTIC    (1u << 3)

/* Compute budget hint for encode. */
typedef enum {
    UVC_COMPUTE_REALTIME_HW = 0,  /* only HW-decodable, P1 */
    UVC_COMPUTE_REALTIME_NPU,
    UVC_COMPUTE_SERVER
} uvc_compute_t;

/* Select active paradigm set as a bitmask. Always includes P1 (base layer).
 * target_bitrate_q8 is Q8.8 fixed-point mbps (e.g. 5.0 Mbps -> 1280). */
uint32_t uvc_select_paradigms(const uvc_content_profile_t *prof,
                              int16_t target_bitrate_q8,
                              uvc_quality_t quality,
                              uvc_compute_t compute,
                              uvc_target_use_t target_use);

#endif /* UVC_SELECTOR_H */
