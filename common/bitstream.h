/* common/bitstream.h - Bit-writer / bit-reader for UVC NALU payloads (spec §3.2) */
#ifndef UVC_BITSTREAM_H
#define UVC_BITSTREAM_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;     /* byte position */
    uint64_t bitbuf;  /* pending bits (LSB-first) */
    int      nbits;   /* number of valid bits in bitbuf */
} bitwriter_t;

typedef struct {
    const uint8_t *buf;
    size_t   len;
    size_t   pos;
    uint32_t bitbuf;
    int      nbits;
} bitreader_t;

void bw_init(bitwriter_t *bw, uint8_t *buf, size_t cap);
/* Write n bits (n<=32) from v (LSB-aligned). Returns 0 on success. */
int  bw_put(bitwriter_t *bw, uint32_t v, int n);
/* Flush remaining bits (zero-padded) and return total bytes written. */
size_t bw_flush(bitwriter_t *bw);

void br_init(bitreader_t *br, const uint8_t *buf, size_t len);
/* Read n bits (n<=32) into *v. Returns 0 on success, -1 if exhausted. */
int  br_get(bitreader_t *br, int n, uint32_t *v);

#endif /* UVC_BITSTREAM_H */
