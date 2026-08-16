/* common/rans.c - Integer rANS entropy coder (spec §6.2).
 *
 * Verbatim-faithful port of Fabian Giesen's public-domain rans64.h:
 * 64-bit state, word-based (32-bit) renormalization with L = 2^31. This is the
 * variant that works for ANY distribution (unlike rans_byte.h, which requires
 * every frequency F >= M/256 and therefore fails on the highly-peaked DCT
 * magnitude distributions UVC produces -> stream desync).
 *
 * Cross-platform determinism: the ryg original writes native-endian uint32_t
 * words via a uint32_t*; we instead emit EXPLICIT little-endian 32-bit words so
 * the bitstream is byte-identical on every target. The final 64-bit state is
 * flushed as two LE words at the front of the stream.
 *
 * rANS is LIFO: the encoder processes symbols LAST->FIRST and writes the
 * renorm-word stream BACKWARD from the tail of the buffer; the decoder reads
 * FIRST->LAST and consumes words forward. No manual array reversal is needed.
 *
 * API note: to keep callers (encoder/p1.c, decoder/p1.c, tools/uvctest.c)
 * unchanged, the encoder buffers symbols during rans_enc_put() and performs the
 * reverse-order rANS emission in rans_enc_finish(); the decoder is naturally
 * forward-streaming. The 32-byte frame header carries the raw symbol counts;
 * both sides rebuild the identical rANS table from them.
 */
#include "rans.h"
#include <stdlib.h>
#include <string.h>

/* ---------- distribution build ---------- */

static int find_sym(const rans_dist_t *d, uint32_t slot) {
    for (int i = 0; i < d->n; i++)
        if (slot >= d->cum[i] && slot < d->cum[i] + d->freq[i]) return i;
    return -1;
}

/* Normalize raw counts into a power-of-two mass table (sum = m = 1<<p) with
 * every symbol freq >= 1 and freqs that fit uint16 (m <= 1<<RANS_P_MAX). We try
 * p from largest (best resolution / best compression) down to 1 and accept the
 * first representable table, distributing the small rounding remainder one unit
 * at a time so the distribution shape is preserved (not flattened). A single
 * symbol may occupy the whole alphabet (freq == m). */
int rans_dist_build(rans_dist_t *d, const uint32_t *counts, int n) {
    if (n <= 0 || n > RANS_N_MAX) return -1;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += counts[i];
    if (total == 0) return -1;

    for (int b = RANS_P_MAX; b >= 1; b--) {
        uint32_t M = (uint32_t)1 << b;
        uint32_t f[RANS_N_MAX];
        uint64_t cap = (n == 1) ? (uint64_t)M : (uint64_t)(M - 1);
        if (cap > 65535) cap = 65535;
        int ok = 1;
        for (int i = 0; i < n; i++) {
            uint64_t fi = ((uint64_t)counts[i] * M + total / 2) / total; /* round */
            if (fi < 1) fi = 1;
            if (fi > cap) { ok = 0; break; }
            f[i] = (uint32_t)fi;
        }
        if (!ok) continue;
        uint64_t sum = 0;
        for (int i = 0; i < n; i++) sum += f[i];
        if (sum != M) {                            /* distribute remainder minimally */
            int32_t delta = (int32_t)(M - sum);    /* >0: add units; <0: remove units */
            if (delta < 0) {
                for (int k = 0; k < -delta; k++) {
                    int mi = 0;
                    for (int i = 1; i < n; i++) if (f[i] > f[mi]) mi = i;
                    if (f[mi] <= 1) { ok = 0; break; }
                    f[mi]--;
                }
            } else {
                for (int k = 0; k < delta; k++) {
                    int mi = 0;
                    for (int i = 1; i < n; i++) if (f[i] > f[mi]) mi = i;
                    if (f[mi] >= cap) { ok = 0; break; }
                    f[mi]++;
                }
            }
            if (!ok) continue;
            sum = 0; for (int i = 0; i < n; i++) sum += f[i];
            if (sum != M) ok = 0;
        }
        if (!ok) continue;

        memset(d, 0, sizeof(*d));
        d->n = (uint8_t)n;
        d->p = (uint8_t)b;
        d->m = M;
        for (int i = 0; i < n; i++) d->raw[i] = counts[i];
        uint32_t acc = 0;
        for (int i = 0; i < n; i++) {
            d->freq[i] = f[i];
            d->cum[i] = acc;
            acc += f[i];
        }
        if (acc != M) return -1;
        return 0;
    }
    return -1;
}

/* ---------- encoder ---------- */

static uint8_t *ensure_symbuf(rans_enc_t *e, int extra) {
    if (e->nsyms + extra <= e->symcap) return e->syms;
    int newcap = e->symcap ? e->symcap * 2 : 256;
    while (newcap < e->nsyms + extra) newcap *= 2;
    uint8_t *nb = realloc(e->syms, (size_t)newcap);
    if (!nb) return NULL;
    e->syms = nb;
    e->symcap = newcap;
    return e->syms;
}

void rans_enc_init(rans_enc_t *e, uint8_t *buf, int cap) {
    e->out = buf;
    e->out_end = buf + cap;
    e->out_len = 0;
    e->syms = NULL;
    e->nsyms = 0;
    e->symcap = 0;
    e->d = NULL;
}

int rans_enc_put(rans_enc_t *e, const rans_dist_t *d, int sym) {
    if (sym < 0 || sym >= d->n) return -1;
    if (e->d == NULL) e->d = d;          /* capture the distribution */
    if (!ensure_symbuf(e, 1)) return -1;
    e->syms[e->nsyms++] = (uint8_t)sym;
    return 0;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Emit the buffered symbols (reverse order) into out[], returning the number of
 * bytes written (>=0) or -1 on overflow. Frees the internal symbol buffer. */
int rans_enc_finish(rans_enc_t *e) {
    const rans_dist_t *d = e->d;
    if (!d) { free(e->syms); e->syms = NULL; return -1; }

    int n = e->nsyms;
    uint32_t p = d->p;
    uint8_t *base = e->out;
    int cap = (int)(e->out_end - base);

    if (n == 0) {                        /* empty stream: just the start state */
        if (cap < 8) { free(e->syms); e->syms = NULL; return -1; }
        uint64_t x0 = RANS64_L;
        for (int b = 0; b < 8; b++) base[b] = (uint8_t)(x0 >> (8 * b));
        free(e->syms); e->syms = NULL;
        e->out_len = 8;
        return 8;
    }

    uint64_t x = RANS64_L;
    uint8_t *ptr = base + cap;           /* write backward from the tail */
    for (int i = n - 1; i >= 0; i--) {   /* rANS: encode LAST symbol first */
        uint32_t F = d->freq[e->syms[i]], C = d->cum[e->syms[i]];
        uint64_t x_max = ((RANS64_L >> p) << 32) * F;   /* renormalize threshold */
        if (x >= x_max) {
            if (ptr - 4 < base) { free(e->syms); e->syms = NULL; return -1; }
            ptr -= 4;
            put_le32(ptr, (uint32_t)x);
            x >>= 32;
        }
        x = ((x / F) << p) + (x % F) + C;
    }
    if (ptr - 8 < base) { free(e->syms); e->syms = NULL; return -1; }
    ptr -= 8;
    for (int b = 0; b < 8; b++) ptr[b] = (uint8_t)(x >> (8 * b));

    int len = (int)(base + cap - ptr);
    /* move the tail-written stream to the front so consumers read from out[0] */
    if (ptr != base) memmove(base, ptr, (size_t)len);
    free(e->syms); e->syms = NULL;
    e->out_len = len;
    return len;
}

/* ---------- decoder ---------- */

void rans_dec_init(rans_dec_t *dec, const uint8_t *buf, int len) {
    dec->cur = buf;
    dec->end = buf + len;
    dec->x = 0;
    if (len >= 8) {
        for (int b = 0; b < 8; b++) dec->x |= (uint64_t)buf[b] << (8 * b);
        dec->cur = buf + 8;
    }
    dec->m = 0;
    dec->p = 0;
}

int rans_dec_get(rans_dec_t *dec, const rans_dist_t *d) {
    uint32_t M = d->m, p = d->p;
    if (M == 0) return -1;
    uint32_t slot = (uint32_t)(dec->x & (M - 1));
    int s = find_sym(d, slot);
    if (s < 0) return -1;
    uint32_t F = d->freq[s], C = d->cum[s];
    dec->x = (uint64_t)F * (dec->x >> p) + slot - C;
    if (dec->x < RANS64_L) {             /* renormalize: pull one LE word */
        if (dec->cur + 4 > dec->end) return -1;
        uint32_t w = get_le32(dec->cur);
        dec->cur += 4;
        dec->x = (dec->x << 32) | w;
    }
    return s;
}

int rans_dec_tell(const rans_dec_t *dec) {
    return (int)(dec->end - dec->cur);
}
