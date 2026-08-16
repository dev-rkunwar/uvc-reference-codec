/* common/segment.c - Per-segment encode/decode orchestration (spec §9/§10/§1).
 * See segment.h for the design and roadmap-box mapping. Integer-only. */
#include "segment.h"
#include <stdlib.h>
#include <string.h>

#include "p1.h"
#include "p2.h"
#include "p3.h"

/* Required decode tier per paradigm flag (1..4 -> P1..P4). Mirrors the
 * negotiate.c convention that P1 is tier-1 and the neural paradigms are
 * tier-2+. The selected segment tier is the max over the enabled paradigms. */
static uint8_t tier_for_flag(uint32_t flag) {
    switch (flag) {
        case UVC_P1_TRADITIONAL: return 1;
        case UVC_P2_NEURAL:      return 2;
        case UVC_P3_INR:         return 2;
        case UVC_P4_SEMANTIC:    return 2;
        default:                 return 1;
    }
}

int uvc_plan_segment(const uvc_content_profile_t *prof,
                     int16_t target_bitrate_q8,
                     uvc_quality_t quality,
                     uvc_compute_t compute,
                     uvc_target_use_t target_use,
                     uint16_t scale_fp,
                     uvc_segment_config_t *cfg) {
    if (!prof || !cfg) return -1;
    uint32_t set = uvc_select_paradigms(prof, target_bitrate_q8, quality,
                                        compute, target_use);
    if (set == 0) return -1;
    /* P1 base layer is always present. */
    set |= UVC_P1_TRADITIONAL;

    uint8_t tier = 1;
    for (int p = 0; p < 4; p++) {
        uint32_t flag = (1u << p);
        if (set & flag) {
            uint8_t t = tier_for_flag(flag);
            if (t > tier) tier = t;
        }
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->paradigm_set = set;
    cfg->tier = tier;
    cfg->scale_fp = scale_fp;
    cfg->p2_level = 3;          /* standard 3-level DWT for 8-multiple frames */
    return 0;
}

int uvc_encode_segment(const int16_t *const *frames, int nframes, int w, int h,
                       const uvc_segment_config_t *cfg, uint8_t *out, int cap) {
    if (nframes < 0 || w <= 0 || h <= 0 || !cfg) return -1;
    /* Base layer is always P1. Enhancement frames use the highest-priority
     * neural paradigm present in the plan set (P4 > P3 > P2), matching the
     * spec GOP model (one base P1 picture + enhancement pictures). */
    int use_p2 = (cfg->paradigm_set & UVC_P2_NEURAL) ? 1 : 0;
    int use_p3 = (cfg->paradigm_set & UVC_P3_INR)    ? 1 : 0;
    int enh = use_p3 ? 3 : (use_p2 ? 2 : 0);   /* 0 = none, 2 = P2, 3 = P3 */

    /* Encode each frame with its paradigm's pipeline into a scratch buffer. */
    const uint8_t **ptrs = malloc((size_t)nframes * sizeof(*ptrs));
    int *flen = malloc((size_t)nframes * sizeof(*flen));
    uint8_t *par = malloc((size_t)nframes * sizeof(*par));
    uint8_t **bufs = malloc((size_t)nframes * sizeof(*bufs));
    if (!ptrs || !flen || !par || !bufs) {
        free(ptrs); free(flen); free(par); free(bufs);
        return -1;
    }
    int rc = -1;
    for (int f = 0; f < nframes; f++) {
        bufs[f] = malloc((size_t)w * h * 2);
        if (!bufs[f]) goto cleanup;
        int n;
        uint8_t pid;
        if (f == 0) {
            /* Base picture is always P1 (decodable standalone on Tier-1). */
            n = uvc_p1_encode_frame(frames[f], w, h, cfg->scale_fp, bufs[f], (int)(w * h * 2));
            pid = UVC_PARADIGM_P1;
        } else if (enh == 3) {
            n = uvc_p3_encode_frame(frames[f], w, h, cfg->scale_fp, bufs[f], (int)(w * h * 2));
            pid = UVC_PARADIGM_P3;
        } else if (enh == 2) {
            n = uvc_p2_encode_frame(frames[f], w, h, cfg->p2_level, cfg->scale_fp, bufs[f], (int)(w * h * 2));
            pid = UVC_PARADIGM_P2;
        } else {
            n = uvc_p1_encode_frame(frames[f], w, h, cfg->scale_fp, bufs[f], (int)(w * h * 2));
            pid = UVC_PARADIGM_P1;
        }
        if (n <= 0) goto cleanup;
        flen[f] = n;
        ptrs[f] = bufs[f];
        par[f] = pid;
    }

    rc = uvc_mux_ex(ptrs, flen, nframes, w, h, par, cfg->paradigm_set, cfg->tier, out, cap);

cleanup:
    for (int f = 0; f < nframes; f++) free(bufs[f]);
    free(ptrs); free(flen); free(par); free(bufs);
    return rc;
}

int uvc_decode_segment(const uint8_t *buf, size_t len, int w, int h,
                       const uvc_decoder_config_t *cfg,
                       int16_t *const *rec, uint8_t *out_paradigm) {
    /* Max frames this scaffold decodes in one segment (guards the demux output
     * arrays). The container is parsed twice: first to learn the frame count
     * without overflowing, then (if within bounds) to fill the arrays. */
    enum { MAX_SEG_FRAMES = 256 };
    int dw = 0, dh = 0, dn = 0;
    int got0 = uvc_demux(buf, len, &dw, &dh, &dn, NULL, NULL, NULL);
    if (got0 < 0) return -1;
    if (dn <= 0 || dn > MAX_SEG_FRAMES) return -1;
    if (dw != w || dh != h) return -1;

    const uint8_t *dframes[MAX_SEG_FRAMES];
    int dlens[MAX_SEG_FRAMES];
    uint8_t dpar[MAX_SEG_FRAMES];
    int got = uvc_demux(buf, len, &dw, &dh, &dn, dframes, dlens, dpar);
    if (got < 0 || dn > MAX_SEG_FRAMES) return -1;

    /* Read the signaled paradigm set from the uvsh header. */
    uint32_t signaled_set = 0;
    const uint8_t *sh = NULL; size_t shlen = 0;
    if (uvc_container_find_box(buf, len, 0x75767368u, &sh, &shlen)) {
        if (shlen >= 5)
            signaled_set = ((uint32_t)sh[0] << 24) | ((uint32_t)sh[1] << 16) |
                           ((uint32_t)sh[2] << 8)  | (uint32_t)sh[3];
    }

    /* Capability negotiation: which paradigms this decoder may materialize.
     * The Tier-1 scaffold has no neural models, so P3/P4 have no decode
     * pipeline; P2 (integer wavelet) needs tier>=2 but no model. The negotiation
     * gate decides per-frame routing. req_tier[i] / model_hash[i] follow the
     * negotiate API (model_hash 0 == "no model required"). */
    uint8_t req_tier[4] = { 1, 2, 2, 2 };
    uint32_t model_hash[4] = { 0, 0, 0, 0 };
    uint32_t allowed = uvc_negotiate_layers(signaled_set, req_tier, model_hash, cfg);

    for (int f = 0; f < dn; f++) {
        uint8_t pid = dpar[f];
        int use_p2 = 0;
        int use_p3 = 0;
        uint8_t decoded_pid = pid;

        if (pid == UVC_PARADIGM_P1) {
            /* Base layer: always decodable if P1 is in the signaled set. */
            if (!(signaled_set & UVC_P1_TRADITIONAL)) return -1;
        } else if (pid == UVC_PARADIGM_P2) {
            if (!(allowed & UVC_P2_NEURAL)) return -1;   /* tier<2: cannot decode */
            use_p2 = 1;
        } else if (pid == UVC_PARADIGM_P3) {
            if (!(allowed & UVC_P3_INR)) return -1;      /* tier<2: cannot decode */
            use_p3 = 1;
        } else {
            /* P4: no neural pipeline in the Tier-1 scaffold. */
            return -1;
        }

        int rc;
        if (use_p3)
            rc = uvc_p3_decode_frame(dframes[f], dlens[f], w, h, 32768, rec[f]);
        else if (use_p2)
            rc = uvc_p2_decode_frame(dframes[f], dlens[f], w, h, 3, 32768, rec[f]);
        else
            rc = uvc_p1_decode_frame(dframes[f], dlens[f], w, h, 32768, rec[f]);
        if (rc != 0) return -1;
        if (out_paradigm) out_paradigm[f] = decoded_pid;
    }
    return 0;
}
