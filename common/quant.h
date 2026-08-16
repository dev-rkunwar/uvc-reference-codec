/* common/quant.h - Integer quantization (spec §12) */
#ifndef UVC_QUANT_H
#define UVC_QUANT_H

#include <stdint.h>

/* Symmetric per-channel quantization with INT16 scale stored Q1.15 fixed-point:
 *   real_scale = scale_fp / 32768
 *   q  = round( v / real_scale ) = round( v * 32768 / scale_fp )
 *   v' = q * real_scale = q * scale_fp / 32768
 * This is exact integer arithmetic, no floating point in the codec path. */

static inline int8_t quant_i8(int32_t v, uint16_t scale_fp) {
    /* round half away from zero */
    int64_t num = (int64_t)v * 32768;
    int64_t q = (num >= 0) ? (num + scale_fp / 2) / scale_fp
                           : (num - scale_fp / 2) / scale_fp;
    if (q > 127) q = 127;
    if (q < -128) q = -128;
    return (int8_t)q;
}

static inline int16_t dequant_i8(int8_t q, uint16_t scale_fp) {
    int32_t v = ((int32_t)q * (int32_t)scale_fp + 16384) / 32768;  /* round */
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

/* Per-channel symmetric INT4 quantize (range [-8,7]). */
static inline int8_t quant_i4(int32_t v, uint16_t scale_fp) {
    int64_t num = (int64_t)v * 32768;
    int64_t q = (num >= 0) ? (num + scale_fp / 2) / scale_fp
                           : (num - scale_fp / 2) / scale_fp;
    if (q > 7) q = 7;
    if (q < -8) q = -8;
    return (int8_t)q;
}

static inline int16_t dequant_i4(int8_t q, uint16_t scale_fp) {
    int32_t v = ((int32_t)q * (int32_t)scale_fp + 16384) / 32768;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

/* Pack two INT4 values (lo in low nibble, hi in high nibble) into one byte. */
static inline uint8_t pack_i4(int8_t lo, int8_t hi) {
    return (uint8_t)(((hi & 0xF) << 4) | (lo & 0xF));
}
static inline void unpack_i4(uint8_t b, int8_t *lo, int8_t *hi) {
    *lo = (int8_t)((int8_t)(b & 0xF) << 4) >> 4;   /* sign extend 4->8 */
    *hi = (int8_t)((int8_t)(b & 0xF0) >> 4);
}

#endif /* UVC_QUANT_H */
