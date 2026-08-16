/* common/container.c - UVC bitstream container (spec §2/§3).
 * ISOBMFF-style box muxer/demuxer for P1 frame bitstreams. Integer-only;
 * frame bytes are copied verbatim so the decode path stays bit-exact.
 */
#include "container.h"
#include <string.h>

/* big-endian helpers (spec §2: boxes are big-endian) */
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v >> 0);
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3] << 0);
}

/* Forward declaration: defined below, but also used by uvc_container_find_box. */
static int read_box(const uint8_t *p, const uint8_t *end, uint32_t *type,
                    const uint8_t **payload, size_t *plen, const uint8_t **next);

/* ---- mux ---- */

int uvc_mux(const uint8_t **frames, const int *frame_len, int nframes,
            int w, int h, const uint8_t *paradigms, uint8_t *out, int cap) {
    if (nframes < 0 || w <= 0 || h <= 0) return -1;
    int pos = 0;

    /* ftyp: brand 'UVC1' + minor_version (0) */
    {
        uint8_t payload[8];
        put_be32(payload + 0, UVC_BRAND_FTYP);
        put_be32(payload + 4, 0);
        int box = 8 + 8;
        if (pos + box > cap) return -1;
        put_be32(out + pos, (uint32_t)box); put_be32(out + pos + 4, 0x66747970u); /* 'ftyp' */
        memcpy(out + pos + 8, payload, 8);
        pos += box;
    }

    /* moov: mvhd (w,h,nframes) + uvcm (per-frame paradigm map) */
    {
        uint8_t mvhd[12];
        put_be32(mvhd + 0, (uint32_t)w);
        put_be32(mvhd + 4, (uint32_t)h);
        put_be32(mvhd + 8, (uint32_t)nframes);
        int mvhd_box = 12 + 8;

        /* uvcm: for each frame, a paradigm byte (1 = P1, 2 = P2, ...). Padded
         * to a 4-byte boundary. Default is P1 when paradigms == NULL. */
        int uvcm_payload = nframes;             /* 1 byte per frame */
        int pad = (4 - (uvcm_payload & 3)) & 3;
        int uvcm_box = (uvcm_payload + pad) + 8;

        int moov_box = mvhd_box + uvcm_box + 8;
        if (pos + moov_box > cap) return -1;
        int moov_pos = pos;
        put_be32(out + pos, (uint32_t)moov_box); put_be32(out + pos + 4, 0x6d6f6f76u); /* 'moov' */
        pos += 8;
        put_be32(out + pos, (uint32_t)mvhd_box); put_be32(out + pos + 4, 0x6d766864u); /* 'mvhd' */
        memcpy(out + pos + 8, mvhd, 12); pos += mvhd_box;
        put_be32(out + pos, (uint32_t)uvcm_box); put_be32(out + pos + 4, 0x7576636du); /* 'uvcm' */
        for (int i = 0; i < nframes; i++)
            out[pos + 8 + i] = paradigms ? paradigms[i] : UVC_PARADIGM_P1;
        pos += uvcm_box;
        (void)moov_pos;
    }

    /* mdat: concatenated frame bitstreams, each length-prefixed (u32) */
    {
        int mdat_payload = 0;
        for (int i = 0; i < nframes; i++) mdat_payload += 4 + frame_len[i];
        int mdat_box = mdat_payload + 8;
        if (pos + mdat_box > cap) return -1;
        put_be32(out + pos, (uint32_t)mdat_box); put_be32(out + pos + 4, 0x6d646174u); /* 'mdat' */
        pos += 8;
        for (int i = 0; i < nframes; i++) {
            put_be32(out + pos, (uint32_t)frame_len[i]); pos += 4;
            memcpy(out + pos, frames[i], (size_t)frame_len[i]); pos += frame_len[i];
        }
    }

    return pos;
}

/* Extended mux: same as uvc_mux but also writes a `uvsh` signaling header box
 * carrying the segment-level paradigm set (bitmask) and tier. This is the
 * normative per-segment signaling (roadmap box: "Bitstream header/signaling for
 * paradigm + tier"). paradigms[] is still written to the uvcm box as before.
 * Returns total bytes written, or -1 on overflow. */
int uvc_mux_ex(const uint8_t **frames, const int *frame_len, int nframes,
               int w, int h, const uint8_t *paradigms, uint32_t paradigm_set,
               uint8_t tier, uint8_t *out, int cap) {
    if (nframes < 0 || w <= 0 || h <= 0) return -1;
    int pos = 0;

    /* ftyp: brand 'UVC1' + minor_version (0) */
    {
        uint8_t payload[8];
        put_be32(payload + 0, UVC_BRAND_FTYP);
        put_be32(payload + 4, 0);
        int box = 8 + 8;
        if (pos + box > cap) return -1;
        put_be32(out + pos, (uint32_t)box); put_be32(out + pos + 4, 0x66747970u); /* 'ftyp' */
        memcpy(out + pos + 8, payload, 8);
        pos += box;
    }

    /* uvsh: signaling header — paradigm_set (u32, big-endian) + tier (u8). */
    {
        uint8_t pl[5];
        put_be32(pl + 0, paradigm_set);
        pl[4] = tier;
        int box = (int)sizeof(pl) + 8;
        if (pos + box > cap) return -1;
        put_be32(out + pos, (uint32_t)box); put_be32(out + pos + 4, 0x75767368u); /* 'uvsh' */
        memcpy(out + pos + 8, pl, sizeof(pl));
        pos += box;
    }

    /* moov: mvhd (w,h,nframes) + uvcm (per-frame paradigm map) */
    {
        uint8_t mvhd[12];
        put_be32(mvhd + 0, (uint32_t)w);
        put_be32(mvhd + 4, (uint32_t)h);
        put_be32(mvhd + 8, (uint32_t)nframes);
        int mvhd_box = 12 + 8;

        /* uvcm: for each frame, a paradigm byte (1 = P1, 2 = P2, ...). Padded
         * to a 4-byte boundary. Default is P1 when paradigms == NULL. */
        int uvcm_payload = nframes;             /* 1 byte per frame */
        int pad = (4 - (uvcm_payload & 3)) & 3;
        int uvcm_box = (uvcm_payload + pad) + 8;

        int moov_box = mvhd_box + uvcm_box + 8;
        if (pos + moov_box > cap) return -1;
        int moov_pos = pos;
        put_be32(out + pos, (uint32_t)moov_box); put_be32(out + pos + 4, 0x6d6f6f76u); /* 'moov' */
        pos += 8;
        put_be32(out + pos, (uint32_t)mvhd_box); put_be32(out + pos + 4, 0x6d766864u); /* 'mvhd' */
        memcpy(out + pos + 8, mvhd, 12); pos += mvhd_box;
        put_be32(out + pos, (uint32_t)uvcm_box); put_be32(out + pos + 4, 0x7576636du); /* 'uvcm' */
        for (int i = 0; i < nframes; i++)
            out[pos + 8 + i] = paradigms ? paradigms[i] : UVC_PARADIGM_P1;
        pos += uvcm_box;
        (void)moov_pos;
    }

    /* mdat: concatenated frame bitstreams, each length-prefixed (u32) */
    {
        int mdat_payload = 0;
        for (int i = 0; i < nframes; i++) mdat_payload += 4 + frame_len[i];
        int mdat_box = mdat_payload + 8;
        if (pos + mdat_box > cap) return -1;
        put_be32(out + pos, (uint32_t)mdat_box); put_be32(out + pos + 4, 0x6d646174u); /* 'mdat' */
        pos += 8;
        for (int i = 0; i < nframes; i++) {
            put_be32(out + pos, (uint32_t)frame_len[i]); pos += 4;
            memcpy(out + pos, frames[i], (size_t)frame_len[i]); pos += frame_len[i];
        }
    }

    return pos;
}

int uvc_container_find_box(const uint8_t *buf, size_t len, uint32_t type,
                           const uint8_t **out_payload, size_t *out_len) {
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    uint32_t t; const uint8_t *pl; size_t plen; const uint8_t *nx;
    if (out_payload) *out_payload = NULL;
    if (out_len) *out_len = 0;
    while (read_box(p, end, &t, &pl, &plen, &nx) == 0) {
        if (t == type) {
            if (out_payload) *out_payload = pl;
            if (out_len) *out_len = plen;
            return 1;
        }
        p = nx;
    }
    return 0;
}

/* ---- demux ---- */

static int read_box(const uint8_t *p, const uint8_t *end, uint32_t *type,
                    const uint8_t **payload, size_t *plen, const uint8_t **next) {
    if (p + 8 > end) return -1;
    uint32_t size = get_be32(p);
    *type = get_be32(p + 4);
    if (size < 8) return -1;
    if (p + (size_t)size > end) return -1;
    *payload = p + 8;
    *plen = (size_t)size - 8;
    *next = p + size;
    return 0;
}

int uvc_demux(const uint8_t *buf, size_t len, int *w, int *h, int *nframes,
              const uint8_t **out_frames, int *out_lens, uint8_t *out_par) {
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    int got_mvhd = 0, got_mdat = 0;
    int mw = 0, mh = 0, mn = 0;
    const uint8_t *uvcm_pl = NULL; size_t uvcm_len = 0;

    uint32_t type; const uint8_t *pl; size_t plen; const uint8_t *nx;
    while (read_box(p, end, &type, &pl, &plen, &nx) == 0) {
        if (type == 0x6d6f6f76u) {               /* 'moov' */
            const uint8_t *q = pl, *qend = pl + plen;
            while (read_box(q, qend, &type, &pl, &plen, &nx) == 0) {
                if (type == 0x6d766864u) {        /* 'mvhd' */
                    if (plen < 12) return -1;
                    mw = (int)get_be32(pl + 0);
                    mh = (int)get_be32(pl + 4);
                    mn = (int)get_be32(pl + 8);
                    got_mvhd = 1;
                } else if (type == 0x7576636du) { /* 'uvcm' paradigm map */
                    uvcm_pl = pl;
                    uvcm_len = plen;
                }
                q = nx;
            }
        } else if (type == 0x6d646174u) {        /* 'mdat' */
            if (out_frames && out_lens) {
                const uint8_t *q = pl, *qend = pl + plen;
                for (int i = 0; i < mn && q + 4 <= qend; i++) {
                    uint32_t fl = get_be32(q); q += 4;
                    if (q + (size_t)fl > qend) return -1;
                    out_frames[i] = q;
                    out_lens[i] = (int)fl;
                    q += fl;
                }
            }
            got_mdat = 1;
        }
        p = nx;
    }

    if (!got_mvhd || !got_mdat) return -1;
    if (w) *w = mw;
    if (h) *h = mh;
    if (nframes) *nframes = mn;
    if (out_par && uvcm_pl) {
        int n = (uvcm_len > (size_t)mn) ? mn : (int)uvcm_len;
        for (int i = 0; i < n; i++) out_par[i] = uvcm_pl[i];
        for (int i = n; i < mn; i++) out_par[i] = UVC_PARADIGM_P1;
    }
    return mn;
}
