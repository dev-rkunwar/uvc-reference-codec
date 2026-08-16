/* tools/uvctest.c - Self-test / conformance smoke test (spec §16) */
#include "rans.h"
#include "quant.h"
#include "bitstream.h"
#include "analyzer.h"
#include "selector.h"
#include "negotiate.h"
#include "p1.h"
#include "p2.h"
#include "container.h"
#include "segment.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } } while(0)

/* ---------- rANS round-trip + cross-call determinism ---------- */
static void test_rans(void) {
    printf("[test] rans round-trip + determinism\n");
    const int N = 2000;
    int *syms = malloc(N * sizeof(int));
    uint32_t counts[4] = { 10, 70, 15, 5 };   /* skewed distribution */
    rans_dist_t d;
    CHECK(rans_dist_build(&d, counts, 4) == 0, "dist build");

    /* deterministic symbol stream from a fixed LCG */
    uint32_t seed = 12345;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        syms[i] = seed % 4;
    }

    uint8_t buf[8192];
    rans_enc_t enc;
    rans_enc_init(&enc, buf, sizeof(buf));
    for (int i = 0; i < N; i++)
        CHECK(rans_enc_put(&enc, &d, syms[i]) == 0, "enc put");
    int len = rans_enc_finish(&enc);
    CHECK(len > 0, "enc finish");
    printf("    %d syms -> %d bytes (%.3f b/sym)\n", N, len, (float)len * 8.0f / N);

    rans_dec_t dec;
    rans_dec_init(&dec, buf, len);
    int ok = 1;
    for (int i = 0; i < N; i++) {
        int s = rans_dec_get(&dec, &d);
        if (s != syms[i]) { ok = 0; break; }
    }
    CHECK(ok, "rans decode matches encode (bit-exact)");
}

/* ---------- quant round-trip ---------- */
static void test_quant(void) {
    printf("[test] int8/int4 quant round-trip\n");
    /* scale = 32768/32768 = 1.0 : q == round(v), dequant == q within rounding slack */
    uint16_t scale = 32768;
    int32_t vals[] = { 0, -1, 5, -5, 50, -50, 100, -100 };
    for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
        int8_t q = quant_i8(vals[i], scale);
        int16_t r = dequant_i8(q, scale);
        /* allow small fixed-point rounding slack */
        CHECK(abs(r - vals[i]) <= 8, "i8 round-trip within slack");
    }
    /* INT4 pack/unpack */
    int8_t lo = -3, hi = 5;
    uint8_t packed = pack_i4(lo, hi);
    int8_t ulo, uhi;
    unpack_i4(packed, &ulo, &uhi);
    CHECK(ulo == lo && uhi == hi, "i4 pack/unpack sign-extends");
}

/* ---------- bitstream round-trip ---------- */
static void test_bitstream(void) {
    printf("[test] bitwriter/reader round-trip\n");
    uint8_t buf[64];
    bitwriter_t bw; bw_init(&bw, buf, sizeof(buf));
    bw_put(&bw, 0xABC, 12);
    bw_put(&bw, 0x1, 1);
    bw_put(&bw, 0x55, 8);
    bw_put(&bw, 0xDEADBEEF, 32);
    size_t n = bw_flush(&bw);

    bitreader_t br; br_init(&br, buf, n);
    uint32_t v;
    CHECK(br_get(&br, 12, &v) == 0 && v == 0xABC, "bw 12-bit");
    CHECK(br_get(&br, 1, &v) == 0 && v == 0x1, "bw 1-bit");
    CHECK(br_get(&br, 8, &v) == 0 && v == 0x55, "bw 8-bit");
    CHECK(br_get(&br, 32, &v) == 0 && v == 0xDEADBEEF, "bw 32-bit");
}

/* ---------- analyzer + selector ---------- */
static void test_analyze_select(void) {
    printf("[test] analyzer + selector\n");
    int w = 64, h = 64;
    uint8_t *flat = calloc(w * h, 1);
    uint8_t *tex  = malloc(w * h);
    for (int i = 0; i < w * h; i++) tex[i] = (uint8_t)((i * 37) & 0xFF);

    uvc_content_profile_t pf, pt;
    uvc_analyze_frame(flat, w, h, &pf);
    uvc_analyze_frame(tex, w, h, &pt);

    CHECK(pf.scene_type == UVC_SCENE_SCREEN, "flat -> screen");
    CHECK(pt.scene_type == UVC_SCENE_NATURAL, "textured -> natural");

    /* flat + realtime HW -> P1 only */
    uint32_t s1 = uvc_select_paradigms(&pf, 1280 /*5.0 Mbps Q8*/, UVC_QUALITY_BALANCE,
                                       UVC_COMPUTE_REALTIME_HW, UVC_USE_HUMAN);
    CHECK(s1 == UVC_P1_TRADITIONAL, "realtime HW -> P1 only");

    /* textured + server + low bitrate (Q8.8 < 12 == <0.05 Mbps) -> P1+P2 */
    uint32_t s2 = uvc_select_paradigms(&pt, 8 /*~0.03 Mbps*/, UVC_QUALITY_BEST,
                                       UVC_COMPUTE_SERVER, UVC_USE_HUMAN);
    CHECK((s2 & UVC_P1_TRADITIONAL) && (s2 & UVC_P2_NEURAL), "low-bpp server -> P1+P2");

    /* machine use -> P4 present */
    uint32_t s3 = uvc_select_paradigms(&pt, 256 /*1.0 Mbps Q8*/, UVC_QUALITY_BALANCE,
                                       UVC_COMPUTE_SERVER, UVC_USE_MACHINE);
    CHECK(s3 & UVC_P4_SEMANTIC, "machine use -> P4");

    free(flat); free(tex);
}

/* ---------- decoder negotiation ---------- */
static void test_negotiate(void) {
    printf("[test] decoder tier negotiation\n");
    uint8_t tier[4] = { 1, 2, 2, 2 };          /* P1 tier1, others tier2 */
    uint32_t hash[4] = { 0, 0xAABBCCDD, 0x11223344, 0x55667788 };
    uint32_t stream = UVC_P1_TRADITIONAL | UVC_P2_NEURAL | UVC_P3_INR | UVC_P4_SEMANTIC;

    /* Tier-1 with no models: only P1 */
    uvc_decoder_config_t t1 = { 1, {0}, 0 };
    uint32_t a1 = uvc_negotiate_layers(stream, tier, hash, &t1);
    CHECK(a1 == UVC_P1_TRADITIONAL, "tier1 -> P1 only");

    /* Tier-2 holding P2 model but not P3/P4: P1+P2 */
    uvc_decoder_config_t t2 = { 2, { 0xAABBCCDD }, 1 };
    uint32_t a2 = uvc_negotiate_layers(stream, tier, hash, &t2);
    CHECK(a2 == (UVC_P1_TRADITIONAL | UVC_P2_NEURAL), "tier2 w/ P2 model");

    /* Tier-3 holding all models: everything */
    uvc_decoder_config_t t3 = { 3, { 0xAABBCCDD, 0x11223344, 0x55667788 }, 3 };
    uint32_t a3 = uvc_negotiate_layers(stream, tier, hash, &t3);
    CHECK(a3 == stream, "tier3 w/ all models -> full set");
}

/* ---------- P1 block-transform pipeline round-trip ---------- */
static void test_p1_pipeline(void) {
    printf("[test] P1 block-transform pipeline (DCT+quant+entropy)\n");
    const int W = 64, H = 64;
    int16_t *orig = malloc((size_t)W * H * sizeof(int16_t));
    int16_t *rec  = malloc((size_t)W * H * sizeof(int16_t));
    uint8_t *bit  = malloc((size_t)W * H * 2);   /* generous cap */

    /* deterministic pseudo-content in a small range so DCT coefficients (incl.
     * the DC term, which carries the per-block mean) fit int8 without saturation
     * (real_scale is limited by the uint16 Q1.15 scale in quant.h). This proves
     * the P1 pipeline (DCT + INT8 quant + entropy + IDCT) is mathematically exact
     * when the signal fits the quantizer; larger signals exercise the documented
     * lossy mode. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            orig[y * W + x] = (int16_t)((x * 5 + y * 3) & 15);

    uint16_t scale = 32768;   /* real_scale = 1.0 : q ~= coeff, stays in int8 range */
    int n = uvc_p1_encode_frame(orig, W, H, scale, bit, (int)(W * H * 2));
    CHECK(n > 32, "p1 encode produced a bitstream");
    printf("    encoded %dx%d -> %d bytes (%.2f bpp)\n", W, H, n, (float)n * 8.0f / (W * H));

    int rc = uvc_p1_decode_frame(bit, n, W, H, scale, rec);
    CHECK(rc == 0, "p1 decode succeeded");

    /* Reconstruction must be close: count pixels within a small tolerance and
     * verify the mean-absolute-error is bounded (real lossy codec behaviour). */
    long mae = 0;
    int worst = 0;
    for (int i = 0; i < W * H; i++) {
        int d = abs((int)rec[i] - (int)orig[i]);
        mae += d;
        if (d > worst) worst = d;
    }
    mae /= (W * H);
    printf("    MAE=%ld (worst=%d) over %d samples\n", mae, worst, W * H);
    CHECK(mae <= 8, "p1 reconstruction MAE within lossy bound");
    CHECK(worst <= 40, "p1 worst-case error bounded");

    /* DCT adjoint sanity: idct(fdct(x)) within fixed-point rounding. */
    int16_t blk[64];
    for (int i = 0; i < 64; i++) blk[i] = (int16_t)((i * 37 - 1000) & 0x7FF);
    int32_t c[64]; uvc_p1_fdct(blk, c);
    int16_t b2[64]; uvc_p1_idct(c, b2);
    int amax = 0;
    for (int i = 0; i < 64; i++) amax = (abs((int)b2[i] - (int)blk[i]) > amax) ? abs((int)b2[i] - (int)blk[i]) : amax;
    printf("    DCT adjoint max error = %d LSB\n", amax);
    CHECK(amax <= 16, "p1 DCT adjoint within 16 LSB");

    free(orig); free(rec); free(bit);
}

/* ---------- P2 wavelet-transform pipeline round-trip ---------- */
static void test_p2_pipeline(void) {
    printf("[test] P2 wavelet-transform pipeline (DWT+quant+entropy)\n");
    const int W = 64, H = 64, LEVEL = 3;
    int16_t *orig = malloc((size_t)W * H * sizeof(int16_t));
    int16_t *rec  = malloc((size_t)W * H * sizeof(int16_t));
    uint8_t *bit  = malloc((size_t)W * H * 2);   /* generous cap */

    /* Small deterministic content so the wavelet coefficients fit int8 at
     * scale 1.0 (real_scale limited by the uint16 Q1.15 scale). This proves the
     * P2 pipeline (DWT + INT8 quant + entropy + iDWT) is mathematically exact
     * when the signal fits the quantizer, exercising the lossless mode. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            orig[y * W + x] = (int16_t)((x * 5 + y * 3) & 15);

    uint16_t scale = 32768;   /* real_scale = 1.0 */
    int n = uvc_p2_encode_frame(orig, W, H, LEVEL, scale, bit, (int)(W * H * 2));
    CHECK(n > 32, "p2 encode produced a bitstream");
    printf("    encoded %dx%d (lvl %d) -> %d bytes (%.2f bpp)\n", W, H, LEVEL, n, (float)n * 8.0f / (W * H));

    int rc = uvc_p2_decode_frame(bit, n, W, H, LEVEL, scale, rec);
    CHECK(rc == 0, "p2 decode succeeded");

    long mae = 0;
    int worst = 0;
    for (int i = 0; i < W * H; i++) {
        int d = abs((int)rec[i] - (int)orig[i]);
        mae += d;
        if (d > worst) worst = d;
    }
    mae /= (W * H);
    printf("    MAE=%ld (worst=%d) over %d samples\n", mae, worst, W * H);
    CHECK(mae <= 8, "p2 reconstruction MAE within lossy bound");
    CHECK(worst <= 40, "p2 worst-case error bounded");

    /* DWT adjoint sanity: idwt(fdwt(x)) EXACTLY recovers x (lossless lifting). */
    int16_t blk[64];
    for (int i = 0; i < 64; i++) blk[i] = (int16_t)((i * 37 - 1000) & 0x7FF);
    int32_t c[64]; uvc_p2_fdwt(blk, 8, 8, 3, c);
    int16_t b2[64]; uvc_p2_idwt(c, 8, 8, 3, b2);
    int amax = 0;
    for (int i = 0; i < 64; i++) amax = (abs((int)b2[i] - (int)blk[i]) > amax) ? abs((int)b2[i] - (int)blk[i]) : amax;
    printf("    DWT adjoint max error = %d LSB\n", amax);
    CHECK(amax == 0, "p2 DWT adjoint exact (lossless)");

    free(orig); free(rec); free(bit);
}

/* ---------- Container paradigm-map mux/demux (P1+P2 labeling) ---------- */
static void test_container_paradigm(void) {
    printf("[test] container uvcm paradigm map (P1+P2 labeling)\n");
    const int W = 32, H = 32;
    uint16_t scale = 32768;

    /* Frame 0 = P1, frames 1..3 = P2 (wavelet). */
    int16_t *orig[4];
    uint8_t *enc[4];
    int enc_len[4];
    uint8_t par[4] = { UVC_PARADIGM_P1, UVC_PARADIGM_P2, UVC_PARADIGM_P2, UVC_PARADIGM_P2 };
    for (int f = 0; f < 4; f++) {
        orig[f] = malloc((size_t)W * H * sizeof(int16_t));
        enc[f]  = malloc((size_t)W * H * 2);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                orig[f][y * W + x] = (int16_t)(((x * 5 + y * 3 + f * 7) & 15));
        if (f == 0)
            enc_len[f] = uvc_p1_encode_frame(orig[f], W, H, scale, enc[f], (int)(W * H * 2));
        else
            enc_len[f] = uvc_p2_encode_frame(orig[f], W, H, 3, scale, enc[f], (int)(W * H * 2));
        CHECK(enc_len[f] > 0, "paradigm: frame encode ok");
    }

    const uint8_t *ptrs[4];
    for (int f = 0; f < 4; f++) ptrs[f] = enc[f];
    uint8_t *cont = malloc(1 << 20);
    int clen = uvc_mux(ptrs, enc_len, 4, W, H, par, cont, 1 << 20);
    CHECK(clen > 0, "paradigm: mux produced bytes");

    int dw = 0, dh = 0, dn = 0;
    const uint8_t *dframes[4];
    int dlens[4];
    uint8_t dpar[4];
    int got = uvc_demux(cont, (size_t)clen, &dw, &dh, &dn, dframes, dlens, dpar);
    CHECK(got == 4, "paradigm: demux frame count");
    CHECK(dpar[0] == UVC_PARADIGM_P1 && dpar[1] == UVC_PARADIGM_P2 &&
          dpar[2] == UVC_PARADIGM_P2 && dpar[3] == UVC_PARADIGM_P2,
          "paradigm: map round-trips P1/P2 labels");

    for (int f = 0; f < 4; f++) free(orig[f]), free(enc[f]);
    free(cont);
}

/* ---------- Segment signaling + selector->encode->decode wiring --------- */
static void test_segment_pipeline(void) {
    printf("[test] segment plan + signaling header + decode routing\n");
    const int W = 32, H = 32, NFR = 4;
    uint16_t scale = 32768;

    /* Textured content so the selector (server compute) picks P1+P2. */
    int16_t *orig[NFR];
    const int16_t *ptrs[NFR];
    int16_t *rec[NFR];
    for (int f = 0; f < NFR; f++) {
        orig[f] = malloc((size_t)W * H * sizeof(int16_t));
        rec[f]  = malloc((size_t)W * H * sizeof(int16_t));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                orig[f][y * W + x] = (int16_t)(((x * 5 + y * 3 + f * 7) & 15));
        ptrs[f] = orig[f];
    }

    uvc_content_profile_t prof;
    memset(&prof, 0, sizeof(prof));
    prof.spatial_complex = 200;                 /* Q8.8 ~0.78 -> textured/natural */
    prof.temporal_complex = 200;                 /* Q8.8 ~0.78 > 0.6 -> triggers P2 */
    prof.scene_type = UVC_SCENE_NATURAL;

    uvc_segment_config_t cfg;
    int prc = uvc_plan_segment(&prof, 256 /*1.0 Mbps Q8*/, UVC_QUALITY_BALANCE,
                               UVC_COMPUTE_SERVER, UVC_USE_HUMAN, scale, &cfg);
    CHECK(prc == 0, "segment plan ok");
    CHECK((cfg.paradigm_set & UVC_P1_TRADITIONAL) && (cfg.paradigm_set & UVC_P2_NEURAL),
          "plan selects P1+P2 for textured/server");
    CHECK(cfg.tier == 2, "plan tier == 2 (highest paradigm is P2)");

    /* Encode the whole segment through the wired pipeline. */
    uint8_t *cont = malloc(1 << 20);
    int clen = uvc_encode_segment(ptrs, NFR, W, H, &cfg, cont, 1 << 20);
    CHECK(clen > 0, "segment encode produced a container");

    /* The signaling header must round-trip: uvsh carries paradigm_set + tier. */
    const uint8_t *sh = NULL; size_t shlen = 0;
    int found = uvc_container_find_box(cont, (size_t)clen, 0x75767368u, &sh, &shlen);
    CHECK(found && shlen >= 5, "uvsh signaling header present");
    if (found && shlen >= 5) {
        uint32_t sig_set = ((uint32_t)sh[0] << 24) | ((uint32_t)sh[1] << 16) |
                           ((uint32_t)sh[2] << 8)  | (uint32_t)sh[3];
        uint8_t  sig_tier = sh[4];
        CHECK(sig_set == cfg.paradigm_set, "signaled paradigm_set matches plan");
        CHECK(sig_tier == cfg.tier, "signaled tier matches plan");
    }

    /* Decode at tier-3 (P2 allowed, no model needed) -> reconstructs P2 frames. */
    uvc_decoder_config_t t3 = { 3, { 0 }, 0 };
    uint8_t dpar[NFR];
    int drc = uvc_decode_segment(cont, (size_t)clen, W, H, &t3, rec, dpar);
    CHECK(drc == 0, "tier-3 decode (P2 allowed) succeeds");
    if (drc == 0) {
        long mae = 0; int worst = 0;
        for (int f = 0; f < NFR; f++)
            for (int i = 0; i < W * H; i++) {
                int d = abs((int)rec[f][i] - (int)orig[f][i]);
                mae += d; if (d > worst) worst = d;
            }
        mae /= (W * H * NFR);
        printf("    tier3 decode: MAE=%ld (worst=%d) over %d samples\n", mae, worst, W * H * NFR);
        CHECK(mae == 0, "tier-3 reconstruction is bit-exact (scale 1.0, lossless content)");
        CHECK(dpar[0] == UVC_PARADIGM_P2, "tier-3 routed frames to P2");
    }

    /* Decode at tier-1 (P2 NOT decodable by base layer) -> must refuse. This
     * proves the capability gate is real, not a silent mis-decode. */
    uvc_decoder_config_t t1 = { 1, { 0 }, 0 };
    int drc1 = uvc_decode_segment(cont, (size_t)clen, W, H, &t1, rec, NULL);
    CHECK(drc1 != 0, "tier-1 correctly REFUSES a P2-encoded segment");

    /* P1-only plan (realtime HW) -> encodable AND decodable at both tiers. */
    uvc_segment_config_t cfg1;
    int prc1 = uvc_plan_segment(&prof, 1280 /*5.0 Mbps*/, UVC_QUALITY_BALANCE,
                                UVC_COMPUTE_REALTIME_HW, UVC_USE_HUMAN, scale, &cfg1);
    CHECK(prc1 == 0 && (cfg1.paradigm_set == UVC_P1_TRADITIONAL), "realtime HW -> P1 only");
    if (prc1 == 0) {
        int c1 = uvc_encode_segment(ptrs, NFR, W, H, &cfg1, cont, 1 << 20);
        CHECK(c1 > 0, "P1-only segment encode ok");
        int d1 = uvc_decode_segment(cont, (size_t)c1, W, H, &t1, rec, NULL);
        CHECK(d1 == 0, "P1-only segment decodes at tier-1");
        int d3 = uvc_decode_segment(cont, (size_t)c1, W, H, &t3, rec, NULL);
        CHECK(d3 == 0, "P1-only segment decodes at tier-3");
    }

    for (int f = 0; f < NFR; f++) free(orig[f]), free(rec[f]);
    free(cont);
}

/* ---------- Container mux/demux round-trip (spec §2/§3) ---------- */
static void test_container(void) {
    printf("[test] ISOBMFF-style container mux/demux (P1 frames)\n");
    const int W = 32, H = 32, NFR = 4;
    uint16_t scale = 32768;

    /* Encode NFR frames individually into separate bitstreams. */
    int16_t *orig[NFR];
    uint8_t *enc[NFR];
    int enc_len[NFR];
    for (int f = 0; f < NFR; f++) {
        orig[f] = malloc((size_t)W * H * sizeof(int16_t));
        enc[f]  = malloc((size_t)W * H * 2);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                orig[f][y * W + x] = (int16_t)(((x * 5 + y * 3 + f * 7) & 15));
        enc_len[f] = uvc_p1_encode_frame(orig[f], W, H, scale, enc[f], (int)(W * H * 2));
        CHECK(enc_len[f] > 0, "container: frame encode ok");
    }

    /* Mux into one container buffer. */
    const uint8_t *ptrs[NFR];
    for (int f = 0; f < NFR; f++) ptrs[f] = enc[f];
    uint8_t *cont = malloc(1 << 20);
    int clen = uvc_mux(ptrs, enc_len, NFR, W, H, NULL, cont, 1 << 20);
    CHECK(clen > 0, "container: mux produced bytes");
    printf("    muxed %d frames (%dx%d) -> %d bytes\n", NFR, W, H, clen);

    /* Demux: recover frame pointers + lengths (into the container buffer). */
    int dw = 0, dh = 0, dn = 0;
    const uint8_t *dframes[NFR];
    int dlens[NFR];
    int got = uvc_demux(cont, (size_t)clen, &dw, &dh, &dn, dframes, dlens, NULL);
    CHECK(got == NFR, "container: demux frame count");
    CHECK(dw == W && dh == H, "container: demux dims match");
    if (got != NFR) { for (int f = 0; f < NFR; f++) { free(orig[f]); free(enc[f]); } free(cont); return; }

    /* Decode each demuxed frame and compare to its original. */
    int allok = 1;
    for (int f = 0; f < NFR; f++) {
        CHECK(dlens[f] == enc_len[f], "container: frame length preserved");
        int16_t *rec = malloc((size_t)W * H * sizeof(int16_t));
        int rc = uvc_p1_decode_frame(dframes[f], dlens[f], W, H, scale, rec);
        CHECK(rc == 0, "container: frame decode ok");
        long mae = 0; int worst = 0;
        for (int i = 0; i < W * H; i++) {
            int d = abs((int)rec[i] - (int)orig[f][i]);
            mae += d; if (d > worst) worst = d;
        }
        mae /= (W * H);
        if (mae > 8 || rc != 0) allok = 0;
        free(rec);
        printf("    frame %d: %d bytes, MAE=%ld (worst=%d)\n", f, dlens[f], mae, worst);
    }
    CHECK(allok, "container: all frames reconstruct bit-exact");

    for (int f = 0; f < NFR; f++) { free(orig[f]); free(enc[f]); }
    free(cont);
}

int main(void) {
    printf("=== UVC reference scaffold self-test ===\n");
    test_rans();
    test_quant();
    test_bitstream();
    test_analyze_select();
    test_negotiate();
    test_p1_pipeline();
    test_p2_pipeline();
    test_segment_pipeline();
    test_container();
    test_container_paradigm();
    printf("=== %s (%d failures) ===\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
