/* decoder/negotiate.c - Tier + model-hash capability check (spec §1, §13) */
#include "negotiate.h"

static int hash_held(const uvc_decoder_config_t *cfg, uint32_t h) {
    for (int i = 0; i < cfg->held_model_count; i++)
        if (cfg->held_model_hashes[i] == h) return 1;
    return 0;
}

uint32_t uvc_negotiate_layers(uint32_t stream_paradigms,
                              uint8_t required_tier_per_paradigm[4],
                              const uint32_t model_hash_per_paradigm[4],
                              const uvc_decoder_config_t *cfg) {
    uint32_t allowed = 0;
    /* paradigm index: 0=P1,1=P2,2=P3,3=P4 (maps to UVC_P1..UVC_P4) */
    for (int p = 0; p < 4; p++) {
        uint32_t flag = (1u << p);
        if (!(stream_paradigms & flag)) continue;
        uint8_t req_tier = required_tier_per_paradigm[p];
        if (cfg->tier < req_tier) continue;          /* cannot decode this tier */
        /* P1 has no model; others need a held model hash (0 means "no model"). */
        uint32_t mh = model_hash_per_paradigm[p];
        if (mh != 0 && !hash_held(cfg, mh)) continue; /* model missing -> skip */
        allowed |= flag;
    }
    /* P1 (base) must be allowed if present, regardless of model (guaranteed above). */
    return allowed;
}
