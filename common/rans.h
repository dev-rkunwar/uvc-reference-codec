/* common/rans.h - Entropy coder (spec §6.2).
 *
 * Reference implementation: integer-only rANS (64-bit state, word-based
 * renormalization, L = 2^31) -- a verbatim-faithful port of Fabian Giesen's
 * public-domain rans64.h. This variant has NO per-symbol frequency constraint
 * (unlike the 32-bit rans_byte.h, which requires every F >= M/256 and therefore
 * fails on the highly-peaked DCT-magnitude distributions UVC produces), so it
 * is correct for ANY distribution.
 *
 * The API is identical to the earlier canonical-Huffman scaffold: callers build
 * a distribution with rans_dist_build(), then encode symbols forward with
 * rans_enc_put()/rans_enc_finish() and decode with rans_dec_init()/rans_dec_get().
 * Swapping Huffman for rANS required no caller changes beyond the internal
 * header-serialization detail (the 32-byte header carries the raw symbol
 * counts; both encoder and decoder rebuild the identical rANS table from them).
 */
#ifndef UVC_ENTROPY_H
#define UVC_ENTROPY_H

#include <stdint.h>

#define RANS_N_MAX   16     /* alphabet size (P1 nibble alphabet = 16) */
#define RANS_P_MAX   15     /* M = 1<<p; M<=32768 so normalized freqs fit uint16 */
#define RANS64_L     (1ull << 31)

/* A distribution over up to 16 symbols.
 * - raw[16] : the original symbol counts (serialized in the 32-byte header).
 * - freq[16]: normalized frequency for each symbol (sums to m = 1<<p).
 * - cum[16] : cumulative start offset = sum(freq[0..i-1]).
 * - n       : number of symbols.
 * - p, m    : scale bits and total mass (m = 1<<p, <= 32768).
 */
typedef struct {
    uint32_t freq[RANS_N_MAX];
    uint32_t cum[RANS_N_MAX];
    uint32_t raw[RANS_N_MAX];   /* original counts, for the wire header */
    uint8_t  n;
    uint8_t  p;
    uint32_t m;
} rans_dist_t;

/* Build a rANS distribution from raw symbol counts.
 * Returns 0 on success, -1 on bad input. */
int rans_dist_build(rans_dist_t *d, const uint32_t *counts, int n);

/* ---- Encoder ---- */
typedef struct {
    uint8_t  *out;
    uint8_t  *out_end;
    int       out_len;
    /* rANS is LIFO: symbols are buffered and emitted in reverse on finish(). */
    uint8_t  *syms;
    int       nsyms;
    int       symcap;
    const rans_dist_t *d;       /* distribution captured from the first put() */
} rans_enc_t;

void rans_enc_init(rans_enc_t *e, uint8_t *buf, int cap);
int  rans_enc_put(rans_enc_t *e, const rans_dist_t *d, int sym);
int  rans_enc_finish(rans_enc_t *e);

/* ---- Decoder ---- */
typedef struct {
    const uint8_t *cur;         /* read cursor into the renorm-word stream */
    const uint8_t *end;
    uint64_t x;                 /* current rANS state */
    uint32_t m;                 /* total mass (1<<p) */
    uint32_t p;                 /* scale bits */
} rans_dec_t;

void rans_dec_init(rans_dec_t *dec, const uint8_t *buf, int len);
int  rans_dec_get(rans_dec_t *dec, const rans_dist_t *d);
int  rans_dec_tell(const rans_dec_t *dec);

#endif /* UVC_ENTROPY_H */
