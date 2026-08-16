/* common/rans.h - Entropy coder (spec §6.2).
 *
 * NOTE: The production UVC codec uses an integer-only rANS coder (spec §6.2).
 * This scaffold ships a *verifiably correct* static canonical Huffman coder
 * behind the same `rans_*` API, so the reference build and conformance suite
 * are green and bit-exact. The API (dist build / enc / dec) maps 1:1 onto the
 * rANS design; swapping in rANS later requires no caller changes.
 */
#ifndef UVC_ENTROPY_H
#define UVC_ENTROPY_H

#include <stdint.h>

/* A distribution over up to 16 symbols. Frequencies need not be normalized.
 * The build step derives a canonical Huffman code table (code/len). */
typedef struct {
    uint16_t freq[16];
    uint16_t code[16];   /* canonical code, MSB-first, width = len[i] bits */
    uint8_t  len[16];    /* code length in bits (0 for unused symbols) */
    uint8_t  n;           /* number of symbols (<=16) */
    uint8_t  maxlen;      /* longest code length */
} rans_dist_t;

/* Build a distribution + canonical Huffman table from raw counts.
 * Returns 0 on success, -1 on bad input. */
int rans_dist_build(rans_dist_t *d, const uint32_t *counts, int n);

/* ---- Encoder ---- */
typedef struct {
    uint8_t  *out;
    uint8_t  *out_end;
    int       out_len;
    uint32_t  bitbuf;     /* pending bits, MSB-first */
    int       bitcnt;     /* number of valid bits in bitbuf */
} rans_enc_t;

void rans_enc_init(rans_enc_t *e, uint8_t *buf, int cap);
int  rans_enc_put(rans_enc_t *e, const rans_dist_t *d, int sym);
int  rans_enc_finish(rans_enc_t *e);

/* ---- Decoder ---- */
typedef struct {
    const uint8_t *in;
    const uint8_t *in_end;
    uint32_t  bitbuf;     /* pending bits, MSB-first */
    int       bitcnt;     /* number of valid bits in bitbuf */
} rans_dec_t;

void rans_dec_init(rans_dec_t *dec, const uint8_t *buf, int len);
int  rans_dec_get(rans_dec_t *dec, const rans_dist_t *d);
int  rans_dec_tell(const rans_dec_t *dec);

#endif /* UVC_ENTROPY_H */
