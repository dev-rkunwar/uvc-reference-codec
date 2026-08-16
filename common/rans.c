/* common/rans.c - Static canonical Huffman coder (spec §6.2 scaffold).
 * Verifiably correct; bit-exact; integer-only. See rans.h for the rANS note.
 *
 * API mirrors the rANS design so callers are unchanged when rANS lands.
 */
#include "rans.h"
#include <string.h>

/* ---------- canonical Huffman build ---------- */

/* Find two smallest nodes among active; returns indices via a,b (-1 if none). */
static void pick_two(const int *weight, const int *active, int count,
                     int *a, int *b) {
    *a = *b = -1;
    for (int i = 0; i < count; i++) {
        if (!active[i]) continue;
        if (*a < 0 || weight[i] < weight[*a]) { *b = *a; *a = i; }
        else if (*b < 0 || weight[i] < weight[*b]) { *b = i; }
    }
}

int rans_dist_build(rans_dist_t *d, const uint32_t *counts, int n) {
    if (n <= 0 || n > 16) return -1;
    memset(d, 0, sizeof(*d));
    d->n = (uint8_t)n;

    /* Ensure every symbol has weight >= 1 so the tree is well-formed. */
    int weight[32];
    int active[32];
    int node_count = 0;
    for (int i = 0; i < n; i++) {
        d->freq[i] = (uint16_t)(counts[i] ? counts[i] : 1);
        weight[node_count] = d->freq[i];
        active[node_count] = 1;
        node_count++;
    }

    /* Build Huffman tree: merge two smallest into a new parent node. */
    int parent[32];
    for (int i = 0; i < 32; i++) parent[i] = -1;
    int total_nodes = node_count;
    int merges = node_count - 1;
    if (merges > 0) {
        for (int m = 0; m < merges; m++) {
            int a, b;
            pick_two(weight, active, total_nodes, &a, &b);
            /* new internal node */
            int ni = total_nodes++;
            weight[ni] = weight[a] + weight[b];
            active[ni] = 1;
            active[a] = 0; active[b] = 0;
            parent[a] = ni; parent[b] = ni;
        }
    }
    int root = total_nodes - 1;

    /* Compute code length per leaf symbol by walking to root. */
    uint8_t len[16];
    int maxlen = 0;
    for (int s = 0; s < n; s++) {
        int depth = 0, cur = s;
        while (parent[cur] != -1) { depth++; cur = parent[cur]; }
        len[s] = (uint8_t)depth;
        if (depth > maxlen) maxlen = depth;
    }
    if (maxlen == 0) { len[0] = 1; maxlen = 1; } /* single symbol edge case */

    /* Canonical code assignment (DEFLATE-style). */
    int bl_count[32];
    memset(bl_count, 0, sizeof(bl_count));
    for (int s = 0; s < n; s++) bl_count[len[s]]++;

    int next_code[32];
    {
        int code = 0;
        for (int bits = 1; bits <= maxlen; bits++) {
            code = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
        }
    }
    for (int s = 0; s < n; s++) {
        int l = len[s];
        if (l != 0) {
            d->code[s] = (uint16_t)next_code[l];
            next_code[l]++;
        } else {
            d->code[s] = 0;
        }
        d->len[s] = (uint8_t)l;
    }
    d->maxlen = (uint8_t)maxlen;
    return 0;
}

/* ---------- bit-level I/O (MSB-first) ---------- */

static void enc_flush_byte(rans_enc_t *e) {
    /* Left-align so a partial final byte is read MSB-first by the decoder. */
    uint8_t b = (uint8_t)((e->bitbuf << (8 - e->bitcnt)) & 0xFF);
    if (e->out_len + 1 <= (e->out_end - e->out))
        e->out[e->out_len++] = b;
    e->bitbuf = 0; e->bitcnt = 0;
}

static void enc_bit(rans_enc_t *e, int b) {
    e->bitbuf = (e->bitbuf << 1) | (b & 1);
    e->bitcnt++;
    if (e->bitcnt == 8) enc_flush_byte(e);
}

void rans_enc_init(rans_enc_t *e, uint8_t *buf, int cap) {
    e->out = buf; e->out_end = buf + cap; e->out_len = 0;
    e->bitbuf = 0; e->bitcnt = 0;
}

int rans_enc_put(rans_enc_t *e, const rans_dist_t *d, int sym) {
    if (sym < 0 || sym >= d->n) return -1;
    uint16_t code = d->code[sym];
    int l = d->len[sym];
    for (int i = l - 1; i >= 0; i--)
        enc_bit(e, (code >> i) & 1);
    return 0;
}

int rans_enc_finish(rans_enc_t *e) {
    /* Pad final partial byte with zero bits. */
    if (e->bitcnt > 0) enc_flush_byte(e);
    return e->out_len;
}

/* ---------- decoder ---------- */

static int dec_read_bit(rans_dec_t *dec) {
    if (dec->bitcnt == 0) {
        if (dec->in < dec->in_end) {
            dec->bitbuf = (uint32_t)(*dec->in++) << 24;
            dec->bitcnt = 8;
        } else {
            dec->bitbuf = 0; dec->bitcnt = 0;
        }
    }
    int b = (dec->bitbuf >> 31) & 1;
    dec->bitbuf <<= 1;
    dec->bitcnt--;
    return b;
}

void rans_dec_init(rans_dec_t *dec, const uint8_t *buf, int len) {
    dec->in = buf; dec->in_end = buf + len;
    dec->bitbuf = 0; dec->bitcnt = 0;
}

int rans_dec_get(rans_dec_t *dec, const rans_dist_t *d) {
    unsigned cur = 0;
    for (int bits = 1; bits <= d->maxlen; bits++) {
        cur = (cur << 1) | (unsigned)dec_read_bit(dec);
        for (int s = 0; s < d->n; s++) {
            if (d->len[s] == bits && (unsigned)d->code[s] == cur)
                return s;
        }
    }
    return -1; /* corrupt stream */
}

int rans_dec_tell(const rans_dec_t *dec) {
    return (int)(dec->in_end - dec->in);
}
