/* encoder/selector.c - Paradigm decision matrix (spec §10) */
#include "selector.h"

uint32_t uvc_select_paradigms(const uvc_content_profile_t *prof,
                              int16_t target_bitrate_q8,
                              uvc_quality_t quality,
                              uvc_compute_t compute,
                              uvc_target_use_t target_use) {
    uint32_t set = UVC_P1_TRADITIONAL;   /* base layer always present */

    /* Hard constraint: real-time HW -> P1 only. */
    if (compute == UVC_COMPUTE_REALTIME_HW)
        return set;

    /* Machine-only or human+machine -> semantic layer. */
    if (target_use == UVC_USE_MACHINE)
        set |= UVC_P4_SEMANTIC;
    else if (target_use == UVC_USE_BOTH)
        set |= UVC_P4_SEMANTIC;

    /* Content-driven augmentation. Profile fields are Q8.8 (real*256). */
    switch (prof->scene_type) {
        case UVC_SCENE_NATURAL:
            if (prof->temporal_complex > 154 && compute >= UVC_COMPUTE_SERVER)   /* 0.6*256 */
                set |= UVC_P2_NEURAL;       /* neural wins on high motion */
            break;
        case UVC_SCENE_SCREEN:
            if (compute >= UVC_COMPUTE_REALTIME_NPU) set |= UVC_P3_INR;
            break;
        case UVC_SCENE_ANIMATION:
            if (compute >= UVC_COMPUTE_REALTIME_NPU) set |= UVC_P3_INR;
            break;
        case UVC_SCENE_VOLUMETRIC:
            if (compute >= UVC_COMPUTE_REALTIME_NPU) set |= UVC_P3_INR;
            break;
        case UVC_SCENE_LOWLIGHT:
            if (compute >= UVC_COMPUTE_SERVER) set |= UVC_P2_NEURAL;
            break;
        case UVC_SCENE_SYNTHETIC:
            if (compute >= UVC_COMPUTE_REALTIME_NPU) set |= UVC_P3_INR;
            break;
        default:
            break;
    }

    /* Ultra-low bitrate: discrete-VQ neural variant (P2). bitrate < 0.05 Mbps
     * == Q8.8 value 12 (0.05*256). */
    if (target_bitrate_q8 > 0 && target_bitrate_q8 < 12)
        set |= UVC_P2_NEURAL;

    /* Progressive requested: INR. */
    if (quality == UVC_QUALITY_PROGRESSIVE && compute >= UVC_COMPUTE_REALTIME_NPU)
        set |= UVC_P3_INR;

    return set;
}
