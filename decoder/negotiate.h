/* decoder/negotiate.h - Tier + model-hash capability check (spec §1, §13) */
#ifndef UVC_NEGOTIATE_H
#define UVC_NEGOTIATE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t tier;                 /* 1,2,3 */
    uint32_t held_model_hashes[8];
    uint8_t  held_model_count;
} uvc_decoder_config_t;

/* Given the paradigms present in a bitstream (bitmask) and the model hashes
 * required per paradigm, return the set of layers this decoder MAY materialize.
 * A layer is allowed iff:
 *   - required tier <= cfg.tier, AND
 *   - (layer requires no model) OR (its model hash is in cfg.held_model_hashes)
 * P1 (base) has no model requirement and is always allowed if present. */
uint32_t uvc_negotiate_layers(uint32_t stream_paradigms,
                              uint8_t required_tier_per_paradigm[4],
                              const uint32_t model_hash_per_paradigm[4],
                              const uvc_decoder_config_t *cfg);

#endif /* UVC_NEGOTIATE_H */
