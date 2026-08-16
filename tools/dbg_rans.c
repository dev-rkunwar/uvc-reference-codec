/* tools/dbg_rans.c - standalone rANS verifier (NOT part of the build).
 * Proves the rANS encode/decode round-trip + determinism before we port it
 * into common/rans.c. Compile directly: gcc tools/dbg_rans.c -o build/dbg_rans
 *
 * Coder: VERBATIM port of Fabian Giesen's rans64.h (public domain), word-based
 * (32-bit) renormalization with L=2^31. This is the variant that works for ANY
 * distribution: the 32-bit rans_byte.h (L=2^23, byte emission) requires every
 * frequency F >= M/256, which a peaked DCT-magnitude distribution can never
 * satisfy -> stream desync. rans64.h has no such constraint.
 *
 * Cross-platform determinism: ryg writes native-endian uint32_t words via a
 * uint32_t*; we instead emit EXPLICIT little-endian 32-bit words into a byte
 * buffer so the bitstream is identical on every target. Final 64-bit state is
 * flushed as two LE words at the front.
 *
 * rANS is LIFO: encoder processes symbols LAST->FIRST and writes the byte-
 * stream BACKWARD; decoder reads FIRST->LAST and consumes words FORWARD.
 * No manual array reversal is needed.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RANS_P_MAX 16            /* M = 1<<p; freq stored in uint16 (<=65535) */
#define RANS64_L   (1ull << 31)  /* normalization lower bound */

typedef struct {
    uint32_t freq[16];   /* symbol frequency (>=1) */
    uint32_t cum[16];    /* cumulative start = sum of freq[0..i-1] */
    uint8_t  n;
    uint8_t  p;          /* scale_bits, M = 1<<p */
    uint32_t m;          /* M */
} dist_t;

/* Build a power-of-two frequency table summing to M = 1<<p, every symbol
 * freq>=1. We try p from LARGEST (best resolution, best compression) down to
 * 1 and accept the first that is representable within uint16 frequencies.
 * Frequencies are proportional to the raw counts (rounded), then the small
 * rounding remainder is distributed one unit at a time so sum == M exactly
 * without flattening the distribution. For a single symbol (n==1) we allow
 * freq == M (which still fits uint16 up to p==15). */
static int build_dist(dist_t *d, const uint32_t *counts, int n) {
    if (n <= 0 || n > 16) return -1;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += counts[i];
    if (total == 0) return -1;

    for (int b = RANS_P_MAX; b >= 1; b--) {          /* largest p first */
        uint32_t M = (uint32_t)1 << b;
        uint32_t f[16];
        /* a single symbol may occupy the whole alphabet (freq==M); with >=2
         * symbols freq must stay < M so the others keep >=1. Never exceed 65535. */
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
        if (sum != M) {                              /* distribute remainder minimally */
            int32_t delta = (int32_t)(M - sum);      /* >0: add units; <0: remove units */
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

        d->p = (uint8_t)b; d->m = M; d->n = (uint8_t)n;
        uint32_t acc = 0;
        for (int i = 0; i < n; i++) {
            d->freq[i] = (uint32_t)f[i];
            d->cum[i] = acc;
            acc += d->freq[i];
        }
        if (acc != M) return -1;
        return 0;
    }
    return -1;
}

static int find_sym(const dist_t *d, uint32_t slot) {
    for (int i = 0; i < d->n; i++)
        if (slot >= d->cum[i] && slot < d->cum[i] + d->freq[i]) return i;
    return -1;
}

static inline uint64_t mul_hi(uint64_t a, uint64_t b) {
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
}

/* Encode n symbols (forward order) into out[cap]. Returns bytes written or -1.
 * Layout: [8-byte final state LE][renorm words...LE]. Encoder walks symbols in
 * reverse and emits words backward from the end of the buffer. At most one
 * 32-bit word is emitted per symbol, so the output fits 8 + 4*n bytes. */
static int rans_enc(const dist_t *d, const uint8_t *syms, int n, uint8_t *out, int cap) {
    uint32_t M = d->m, p = d->p;
    uint64_t x = RANS64_L;
    uint8_t *ptr = out + cap;
    for (int i = n - 1; i >= 0; i--) {            /* rANS: encode LAST symbol first */
        uint32_t F = d->freq[syms[i]], C = d->cum[syms[i]];
        uint64_t x_max = ((RANS64_L >> p) << 32) * F;   /* renormalize threshold */
        if (x >= x_max) {
            if (ptr - 4 < out) return -1;
            ptr -= 4;
            ptr[0] = (uint8_t)(x & 0xFF); ptr[1] = (uint8_t)((x >> 8) & 0xFF);
            ptr[2] = (uint8_t)((x >> 16) & 0xFF); ptr[3] = (uint8_t)((x >> 24) & 0xFF);
            x >>= 32;
        }
        /* x = C(s,x): exact integer division + remainder + start offset */
        x = ((x / F) << p) + (x % F) + C;
    }
    if (ptr - 8 < out) return -1;                  /* flush final state (2 LE words) */
    ptr -= 8;
    for (int b = 0; b < 8; b++) ptr[b] = (uint8_t)(x >> (8 * b));
    int len = (int)(out + cap - ptr);
    /* Stream was written at the tail; move it to the front so consumers that
     * read from out[0] (incl. rans_dec + our test harness) get the right bytes.
     * Layout becomes [8-byte state][renorm words...], identical to the decoder's
     * view. */
    if (ptr != out) memmove(out, ptr, (size_t)len);
    return len;
}

/* Decode len bytes (len>=8) into syms[0..n-1] in forward order. Returns 0/-1. */
static int rans_dec(const dist_t *d, const uint8_t *in, int len, uint8_t *syms, int n) {
    uint32_t M = d->m, p = d->p;
    if (len < 8) return -1;
    const uint8_t *ptr = in;
    uint64_t x = 0;
    for (int b = 0; b < 8; b++) x |= (uint64_t)in[b] << (8 * b);   /* read state */
    ptr += 8;
    for (int i = 0; i < n; i++) {
        uint32_t slot = (uint32_t)(x & (M - 1));   /* Rans64DecGet */
        int s = find_sym(d, slot);
        if (s < 0) return -1;
        uint32_t F = d->freq[s], C = d->cum[s];
        x = (uint64_t)F * (x >> p) + slot - C;      /* Rans64DecAdvance (step) */
        if (x < RANS64_L) {                         /* renormalize (single word) */
            if (ptr + 4 > in + len) return -1;
            uint32_t w = (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) |
                        ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
            ptr += 4;
            x = (x << 32) | w;
        }
        syms[i] = (uint8_t)s;
    }
    return 0;
}

static int test_one(const char *name, const uint32_t *counts, int n, int nstreams, int slen) {
    dist_t d;
    if (build_dist(&d, counts, n)) { printf("  %s: build FAIL (unrepresentable)\n", name); return 1; }
    uint8_t *syms = malloc(slen ? slen : 1);
    uint8_t *out  = malloc(slen * 4 + 64);
    uint8_t *dec  = malloc(slen * 4 + 64);
    int fails = 0, bytes = 0;
    for (int t = 0; t < nstreams; t++) {
        uint64_t tot = 0; for (int i = 0; i < n; i++) tot += counts[i];
        for (int i = 0; i < slen; i++) {
            uint64_t r = ((uint64_t)rand() * rand()) % tot;
            uint64_t a = 0; int s = 0;
            for (; s < n; s++) { a += counts[s]; if (r < a) break; }
            syms[i] = (uint8_t)s;
        }
        int len = rans_enc(&d, syms, slen, out, slen * 4 + 64);
        if (len < 0) { printf("  %s: enc overflow\n", name); fails++; break; }
        if (t == 0) bytes = len;
        if (rans_dec(&d, out, len, dec, slen)) { printf("  %s: decode FAIL (corrupt)\n", name); fails++; break; }
        if (memcmp(syms, dec, slen) != 0) { printf("  %s: MISMATCH at stream %d\n", name, t); fails++; break; }
    }
    if (!fails) {                                   /* determinism: same stream => same bytes */
        uint64_t tot = 0; for (int i = 0; i < n; i++) tot += counts[i];
        for (int i = 0; i < slen; i++) { uint64_t r = ((uint64_t)rand() * rand()) % tot; uint64_t a = 0; int s = 0; for (; s < n; s++) { a += counts[s]; if (r < a) break; } syms[i] = (uint8_t)s; }
        int l1 = rans_enc(&d, syms, slen, out, slen * 4 + 64);
        int l2 = rans_enc(&d, syms, slen, dec, slen * 4 + 64);
        if (l1 != l2 || memcmp(out, dec, l1) != 0) { printf("  %s: NONDETERMINISTIC\n", name); fails++; }
    }
    printf("  %s: %s (%d syms/stream, %d bytes, %.3f b/sym)\n",
           name, fails ? "FAIL" : "ok", slen, bytes, (double)bytes * 8 / slen);
    free(syms); free(out); free(dec);
    return fails;
}

int main(void) {
    int fails = 0;
    printf("=== rANS standalone verifier (ryg rans64.h port, LE words) ===\n");
    srand(12345);

    uint32_t a[] = {1000, 100, 10, 1};
    fails += test_one("skewed4", a, 4, 200, 2000);
    uint32_t b[16]; for (int i = 0; i < 16; i++) b[i] = 1;
    fails += test_one("uniform16", b, 16, 200, 2000);
    uint32_t c[16]; c[0] = 9000; for (int i = 1; i < 16; i++) c[i] = 50;
    fails += test_one("dominant", c, 16, 200, 2000);
    uint32_t e[1] = {50};
    fails += test_one("single", e, 1, 200, 2000);
    uint32_t f[16]; for (int i = 0; i < 16; i++) f[i] = 1 + (unsigned)rand() % 200;
    fails += test_one("randcounts", f, 16, 200, 2000);
    printf(fails ? "=== VERIFY FAIL ===\n" : "=== VERIFY PASS ===\n");
    return fails ? 1 : 0;
}
