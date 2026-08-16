/* tools/uvcplay_demo.c - Minimal "player" loop over the UVC reference API.
 *
 * This is NOT a full media player. UVC is a frame-level codec that operates
 * on in-memory int16_t luminance planes; file I/O and pixel rendering are the
 * integrator's job. This demo shows the real end-to-end loop:
 *
 *   source luma (uint8) -> analyze -> select paradigm
 *                  -> encode frame -> mux into container buffer
 *   ... (persist buffer to disk as the integrator's responsibility) ...
 *   demux buffer -> per-frame: decode (P1 or P2) -> render (here: write PGM)
 *
 * Build (after `cmake --build build`):
 *   gcc tools/uvcplay_demo.c -Icommon -Iencoder -Idecoder \
 *       build/libuvc_common.a build/libuvc_enc.a build/libuvc_dec.a \
 *       -o build/uvcplay_demo
 * Run:
 *   ./build/uvcplay_demo
 *
 * It encodes two synthetic clips (a flat "screen" clip and a textured
 * "natural" clip), each muxed as P1 or P2 frames, writes the container to
 * movie.uvc, then decodes it back and emits one PGM per frame.
 */
#include "analyzer.h"
#include "selector.h"
#include "p1.h"
#include "p2.h"
#include "container.h"
#include "quant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define LEVEL 3
#define SCALE 32768u   /* real_scale = 1.0 */

/* ---- source content generators (uint8 grayscale) ---- */
static void gen_flat(uint8_t *px, int w, int h) {
    for (int i = 0; i < w * h; i++) px[i] = 16;          /* near-uniform */
}
static void gen_textured(uint8_t *px, int w, int h, int seed) {
    for (int i = 0; i < w * h; i++) px[i] = (uint8_t)(((i * 37 + seed) & 0xFF));
}

/* ---- render an int16 luminance plane to a PGM file ---- */
static void write_pgm(const char *path, const int16_t *frame, int w, int h) {
    int minv = 32767, maxv = -32768;
    for (int i = 0; i < w * h; i++) {
        if (frame[i] < minv) minv = frame[i];
        if (frame[i] > maxv) maxv = frame[i];
    }
    int span = (maxv - minv) ? (maxv - minv) : 1;
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        int v = (int)((frame[i] - minv) * 255L / span);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        fputc(v, f);
    }
    fclose(f);
}

int main(void) {
    /* 1) Build two "clips": a screen clip (P1) and a natural clip (P2). */
    uint8_t src[W * H];
    int16_t frame[W * H];

    int nframes = 4;
    const uint8_t *ptrs[4];
    int enclen[4];
    uint8_t enc[4][W * H * 2];
    uint8_t paradigm[4];

    /* Clip A: flat screen content -> analyzer says SCREEN -> P1. */
    gen_flat(src, W, H);
    uvc_content_profile_t pf;
    uvc_analyze_frame(src, W, H, &pf);
    printf("clip A scene_type=%d (expect SCREEN=%d)\n", pf.scene_type, UVC_SCENE_SCREEN);
    uint32_t selA = uvc_select_paradigms(&pf, 1280 /*5.0 Mbps Q8*/, UVC_QUALITY_BALANCE,
                                         UVC_COMPUTE_REALTIME_HW, UVC_USE_HUMAN);
    printf("clip A selected paradigms=0x%x (P1 bit=%u)\n", selA, UVC_P1_TRADITIONAL);
    for (int i = 0; i < W * H; i++) frame[i] = (int16_t)src[i];
    enclen[0] = uvc_p1_encode_frame(frame, W, H, SCALE, enc[0], (int)(W * H * 2));
    ptrs[0]  = enc[0];
    paradigm[0] = UVC_PARADIGM_P1;
    printf("clip A frame0: %d bytes (P1)\n", enclen[0]);

    /* Clip B: textured natural content -> analyzer says NATURAL -> choose P2. */
    gen_textured(src, W, H, 7);
    uvc_content_profile_t pt;
    uvc_analyze_frame(src, W, H, &pt);
    printf("clip B scene_type=%d (expect NATURAL=%d)\n", pt.scene_type, UVC_SCENE_NATURAL);
    uint32_t selB = uvc_select_paradigms(&pt, 8 /*~0.03 Mbps*/, UVC_QUALITY_BEST,
                                         UVC_COMPUTE_SERVER, UVC_USE_HUMAN);
    printf("clip B selected paradigms=0x%x (P1=0x%x P2=0x%x)\n",
           selB, UVC_P1_TRADITIONAL, UVC_P2_NEURAL);
    for (int f = 1; f < nframes; f++) {
        gen_textured(src, W, H, f * 11);
        for (int i = 0; i < W * H; i++) frame[i] = (int16_t)src[i];
        enclen[f] = uvc_p2_encode_frame(frame, W, H, LEVEL, SCALE, enc[f], (int)(W * H * 2));
        ptrs[f]  = enc[f];
        paradigm[f] = UVC_PARADIGM_P2;
        printf("clip B frame%d: %d bytes (P2)\n", f, enclen[f]);
    }

    /* 2) Mux all frames into one container buffer (the integrator persists it). */
    uint8_t *cont = malloc(1 << 20);
    int clen = uvc_mux(ptrs, enclen, nframes, W, H, paradigm, cont, 1 << 20);
    if (clen < 0) { printf("mux failed\n"); return 1; }
    printf("muxed %d frames -> %d bytes\n", nframes, clen);

    /* (Integrator responsibility: save `cont` to movie.uvc, transmit, reload.) */
    FILE *mf = fopen("movie.uvc", "wb");
    if (mf) { fwrite(cont, 1, (size_t)clen, mf); fclose(mf); }

    /* 3) Decode side: demux, then per-frame decode by paradigm id. */
    int dw = 0, dh = 0, dn = 0;
    const uint8_t *dframes[4];
    int dlens[4];
    uint8_t dpar[4];
    int got = uvc_demux(cont, (size_t)clen, &dw, &dh, &dn, dframes, dlens, dpar);
    printf("demux: %d frames, %dx%d\n", got, dw, dh);

    int16_t *rec = malloc((size_t)W * H * sizeof(int16_t));
    char pgmpath[64];
    for (int f = 0; f < got; f++) {
        int rc;
        if (dpar[f] == UVC_PARADIGM_P1)
            rc = uvc_p1_decode_frame(dframes[f], dlens[f], dw, dh, SCALE, rec);
        else /* P2: level must be tracked out-of-band (container stores no level) */
            rc = uvc_p2_decode_frame(dframes[f], dlens[f], dw, dh, LEVEL, SCALE, rec);
        if (rc != 0) { printf("decode frame %d FAILED\n", f); continue; }
        snprintf(pgmpath, sizeof(pgmpath), "frame_%d.pgm", f);
        write_pgm(pgmpath, rec, dw, dh);
        printf("rendered frame %d (%s) -> %s\n",
               f, dpar[f] == UVC_PARADIGM_P1 ? "P1" : "P2", pgmpath);
    }

    free(rec);
    free(cont);
    printf("done. container saved as movie.uvc; frames as frame_*.pgm\n");
    return 0;
}
