/* common/bitstream.c - Bit-writer / bit-reader (spec §3.2) */
#include "bitstream.h"
#include <string.h>

void bw_init(bitwriter_t *bw, uint8_t *buf, size_t cap) {
    bw->buf = buf; bw->cap = cap; bw->pos = 0;
    bw->bitbuf = 0; bw->nbits = 0;
}

int bw_put(bitwriter_t *bw, uint32_t v, int n) {
    if (n < 0 || n > 32) return -1;
    uint64_t bits = (uint64_t)(v & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u)));
    bw->bitbuf |= bits << bw->nbits;
    bw->nbits += n;
    while (bw->nbits >= 8) {
        if (bw->pos >= bw->cap) return -1;
        bw->buf[bw->pos++] = (uint8_t)(bw->bitbuf & 0xFF);
        bw->bitbuf >>= 8;
        bw->nbits -= 8;
    }
    return 0;
}

size_t bw_flush(bitwriter_t *bw) {
    if (bw->nbits > 0) {
        if (bw->pos < bw->cap) bw->buf[bw->pos++] = (uint8_t)(bw->bitbuf & 0xFF);
        bw->bitbuf = 0; bw->nbits = 0;
    }
    return bw->pos;
}

void br_init(bitreader_t *br, const uint8_t *buf, size_t len) {
    br->buf = buf; br->len = len; br->pos = 0;
    br->bitbuf = 0; br->nbits = 0;
}

int br_get(bitreader_t *br, int n, uint32_t *v) {
    if (n < 0 || n > 32) return -1;
    while (br->nbits < n) {
        if (br->pos >= br->len) return -1;
        br->bitbuf |= ((uint32_t)br->buf[br->pos++]) << br->nbits;
        br->nbits += 8;
    }
    *v = br->bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
    br->bitbuf >>= n;
    br->nbits -= n;
    return 0;
}
