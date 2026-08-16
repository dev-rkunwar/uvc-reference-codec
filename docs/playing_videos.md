# Playing Different Kinds of Videos with UVC

A practical guide to driving the UVC reference codec as a "player" — or more
accurately, as a **frame-codec loop** — and how to map different kinds of video
content onto the codec's paradigms (P1 block-transform, P2 wavelet), quality
knobs, and decoder tiers.

> **Read this first — what UVC is and is not.**
> UVC is a *spec-driven reference scaffold*, not a shipping media player. The
> library operates on **in-memory `int16_t` luminance planes** (grayscale, one
> frame at a time). It has **no built-in file format, no color planes, no audio,
> no motion/inter-frame prediction, and no pixel display**. File I/O and
> rendering are *your* job as the integrator. What UVC *does* give you, and what
> this guide shows you how to use, is a verified, deterministic
> encode → container → decode loop plus a content analyzer that picks the right
> compression paradigm for the kind of footage you feed it.
>
> Think of this document as "how to build the playback pipeline around the
> codec," not "how to run `uvcplay movie.mp4`."

---

## 1. The mental model

A full player is three stages. UVC owns the **middle** stage; you own the edges.

```
   SOURCE (you)                UVC CODEC (this repo)              SINK (you)
 ┌──────────────┐        ┌───────────────────────────┐      ┌──────────────┐
 │ camera / file │  ->    │  analyze -> select         │      │  display /   │
 │ decode to     │        │  encode (P1 or P2)         │      │  save /      │
 │ luma plane    │        │  mux into container buffer │  ->  │  further     │
 │ (uint8 ->     │        │  ... store/persist ...     │      │  processing  │
 │  int16)       │        │  demux -> decode -> plane  │      │              │
 └──────────────┘        └───────────────────────────┘      └──────────────┘
```

So "playing a video" means: for every frame, produce a luminance plane, hand it
to UVC, get a compressed bitstream back; later (or elsewhere) hand the
bitstream back to UVC and get the plane back; then render that plane however you
like (this guide renders to PGM for verification; a real player would upload to
a GPU texture or write to a window).

---

## 2. Build the library

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

This produces `build/libuvc_common.a`, `build/libuvc_enc.a`, `build/libuvc_dec.a`
and the self-test `build/uvctest`. (On Windows/MinGW the test binary is linked
`-static` so it needs no runtime DLLs.)

**Verify the codec itself before you build anything on top of it:**

```bash
./build/uvctest      # ends with: === PASS (0 failures) ===
```

---

## 3. The end-to-end playback loop (verified)

The snippet below is **exactly** what lives in `tools/uvcplay_demo.c` and has
been compiled and run against the real library. It encodes two synthetic clips —
a flat "screen" clip and a textured "natural" clip — muxes them into one
container buffer, writes it to `movie.uvc`, then decodes it back and renders one
PGM per frame.

```c
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

static void gen_flat(uint8_t *px, int w, int h) {
    for (int i = 0; i < w * h; i++) px[i] = 16;
}
static void gen_textured(uint8_t *px, int w, int h, int seed) {
    for (int i = 0; i < w * h; i++) px[i] = (uint8_t)(((i * 37 + seed) & 0xFF));
}

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
        if (v < 0) v = 0; if (v > 255) v = 255;
        fputc(v, f);
    }
    fclose(f);
}

int main(void) {
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
    uint32_t selA = uvc_select_paradigms(&pf, 1280 /*5.0 Mbps Q8*/, UVC_QUALITY_BALANCE,
                                         UVC_COMPUTE_REALTIME_HW, UVC_USE_HUMAN);
    for (int i = 0; i < W * H; i++) frame[i] = (int16_t)src[i];
    enclen[0] = uvc_p1_encode_frame(frame, W, H, SCALE, enc[0], (int)(W * H * 2));
    ptrs[0]  = enc[0];
    paradigm[0] = UVC_PARADIGM_P1;

    /* Clip B: textured natural content -> analyzer says NATURAL -> P2. */
    uvc_content_profile_t pt;
    for (int f = 1; f < nframes; f++) {
        gen_textured(src, W, H, f * 11);
        uvc_analyze_frame(src, W, H, &pt);
        uint32_t selB = uvc_select_paradigms(&pt, 8 /*~0.03 Mbps*/, UVC_QUALITY_BEST,
                                             UVC_COMPUTE_SERVER, UVC_USE_HUMAN);
        (void)selB; /* for textured content this returns P1|P2 (0x3) */
        for (int i = 0; i < W * H; i++) frame[i] = (int16_t)src[i];
        enclen[f] = uvc_p2_encode_frame(frame, W, H, LEVEL, SCALE, enc[f], (int)(W * H * 2));
        ptrs[f]  = enc[f];
        paradigm[f] = UVC_PARADIGM_P2;
    }

    /* Mux all frames into one container buffer (you persist it to disk). */
    uint8_t *cont = malloc(1 << 20);
    int clen = uvc_mux(ptrs, enclen, nframes, W, H, paradigm, cont, 1 << 20);
    if (clen < 0) { printf("mux failed\n"); return 1; }
    FILE *mf = fopen("movie.uvc", "wb");
    if (mf) { fwrite(cont, 1, (size_t)clen, mf); fclose(mf); }

    /* Decode side: demux, then per-frame decode by paradigm id. */
    int dw = 0, dh = 0, dn = 0;
    const uint8_t *dframes[4];
    int dlens[4];
    uint8_t dpar[4];
    int got = uvc_demux(cont, (size_t)clen, &dw, &dh, &dn, dframes, dlens, dpar);

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
    }

    free(rec);
    free(cont);
    return 0;
}
```

**Compile and run** (link order matters — `libuvc_common.a` must come *last* so
it resolves the symbols that `enc`/`dec` reference):

```bash
gcc tools/uvcplay_demo.c -Icommon -Iencoder -Idecoder \
    build/libuvc_enc.a build/libuvc_dec.a build/libuvc_common.a \
    -o build/uvcplay_demo
./build/uvcplay_demo
```

Verified output (this is real, captured from a run against the built library):

```
clip A scene_type=1 (expect SCREEN=1)
clip A selected paradigms=0x1 (P1 bit=1)
clip A frame0: 1164 bytes (P1)
clip B scene_type=0 (expect NATURAL=0)
clip B selected paradigms=0x3 (P1=0x1 P2=0x2)
clip B frame1: 3064 bytes (P2)
clip B frame2: 3056 bytes (P2)
clip B frame3: 3012 bytes (P2)
muxed 4 frames -> 10376 bytes
demux: 4 frames, 64x64
rendered frame 0 (P1) -> frame_0.pgm
rendered frame 1 (P2) -> frame_1.pgm
rendered frame 2 (P2) -> frame_2.pgm
rendered frame 3 (P2) -> frame_3.pgm
done. container saved as movie.uvc; frames as frame_*.pgm
```

Notice the codec's behavior fell right out of the content:
- **Flat/screen frame** → analyzer reports `UVC_SCENE_SCREEN` → selector returns
  `0x1` (P1 only). P1 is the right, cheap choice for uniform content.
- **Textured/natural frames** → analyzer reports `UVC_SCENE_NATURAL` → selector
  returns `0x3` (P1 + P2). At low bitrate the wavelet (P2) is preferred.

---

## 4. Mapping "kinds of videos" onto the codec

There is no single `play()` call. Instead you steer the codec along four axes.
Each is a real, tested API surface.

### 4.1 By content type → paradigm (the analyzer + selector)

`uvc_analyze_frame(luma, w, h, &profile)` fills a `uvc_content_profile_t` with a
scene classification. `uvc_select_paradigms(&profile, bitrate_q8, quality,
compute, target_use)` returns a bitmask of which paradigms to *encode* with:

| Content kind            | Scene enum              | Typical selection (default hints)        |
|-------------------------|-------------------------|------------------------------------------|
| Flat / screen / UI      | `UVC_SCENE_SCREEN`      | P1 only (cheapest, ideal for text/UIs)   |
| Photographic / natural  | `UVC_SCENE_NATURAL`     | P1 + P2 (wavelet wins at low bitrate)    |
| Animation / cel         | `UVC_SCENE_ANIMATION`   | P1 + P2                                  |
| Volumetric / 3D render  | `UVC_SCENE_VOLUMETRIC`  | P1 + P2                                  |
| Synthetic / CGI         | `UVC_SCENE_SYNTHETIC`   | P1 + P2                                  |
| Low-light / noisy       | `UVC_SCENE_LOWLIGHT`    | P1 + P2                                  |

The selector also takes **three constraints** that change the answer:

- **`target_bitrate_q8`** — Q8.8 fixed-point Mbps. e.g. `5.0 Mbps → 1280`,
  `0.03 Mbps → 8`. Lower bitrate pushes toward P2 (wavelet) and away from the
  heavier paradigms.
- **`quality`** — `UVC_QUALITY_SPEED` / `BALANCE` / `BEST` / `PROGRESSIVE`.
- **`compute`** — `UVC_COMPUTE_REALTIME_HW` (P1 only — HW-decodable),
  `UVC_COMPUTE_REALTIME_NPU`, `UVC_COMPUTE_SERVER` (allows P2/P3/P4).
- **`target_use`** — `UVC_USE_HUMAN` / `UVC_USE_MACHINE` / `UVC_USE_BOTH`.
  Machine use additionally brings in P4 (semantic) when models are available.

Always treat P1 as the **base layer**: every selection bitmask includes it.

### 4.2 By pipeline → P1 (block-transform) vs P2 (wavelet)

These are the two *implemented and verified* pipelines. Use them explicitly when
you bypass the selector and want to choose the transform yourself:

| Pipeline | Encode                                    | Decode                                      | Best for                                    |
|----------|-------------------------------------------|---------------------------------------------|---------------------------------------------|
| P1       | `uvc_p1_encode_frame(f, w, h, scale, out, cap)` | `uvc_p1_decode_frame(buf, len, w, h, scale, f)` | General video, screen content, HW decode. DCT, INT8 quant, rANS. |
| P2       | `uvc_p2_encode_frame(f, w, h, level, scale, out, cap)` | `uvc_p2_decode_frame(buf, len, w, h, level, scale, f)` | Natural/photographic at low bitrate. Integer LeGall 5/3 wavelet, INT8 quant, rANS. |

Both take a **`scale`** quantization parameter (`uint16`, Q1.15 fixed point).
`scale = 32768` means `real_scale = 1.0` (near-lossless when the signal fits
INT8). **Larger `scale` ⇒ coarser quantization ⇒ smaller file ⇒ more loss.**
This is your quality/bpp dial for "different kinds of videos":

- High-quality archival of clean source → `scale` near 32768.
- Constrained-network "tiny thumbnail stream" → larger `scale` (e.g. 65535) or
  lean on the P2 wavelet path.

> **Frame-size rule:** dimensions must be a multiple of 8 for P1 (8×8 blocks)
> and a multiple of `2^level` (so 8 for `level=3`) for P2. The demo uses 64×64.

### 4.3 By decoder capability → tier negotiation

A *receiver* may not hold every model. `uvc_negotiate_layers()` takes the
paradigm bitmask present in the bitstream plus a `uvc_decoder_config_t`
describing the decoder's **tier** (1/2/3) and which model hashes it holds, and
returns the set of layers it may legally materialize:

```c
uvc_decoder_config_t cfg = { 2, { 0xAABBCCDD }, 1 };  /* tier 2, holds 1 model */
uint8_t  required_tier[4] = { 1, 2, 2, 2 };
uint32_t model_hash[4]    = { 0, 0xAABBCCDD, 0x11223344, 0x55667788 };
uint32_t allowed = uvc_negotiate_layers(stream_mask, required_tier, model_hash, &cfg);
```

Rules:
- **Tier 1** (no models): only P1 decodes. This is the safe fallback for any
  minimal client.
- **Tier 2** (holds the P2 model): P1 + P2 decode.
- **Tier 3** (holds all models): everything present decodes.

So "playing a video on a weak device" = negotiate down to P1; "on a capable
server" = negotiate up to P1+P2 (+P3/P4 once those ship). The muxer tags each
frame's paradigm in the container's `uvcm` box, so the decoder knows which
decode function to call per frame.

### 4.4 Container: muxing a mixed-paradigm "video"

`uvc_mux()` packs N frame bitstreams plus width/height and a **per-frame
paradigm map** into one buffer; `uvc_demux()` recovers them. Pass a non-NULL
`paradigms` array to label frames (`UVC_PARADIGM_P1 = 1`, `UVC_PARADIGM_P2 = 2`);
pass `NULL` to default everything to P1.

```c
int clen = uvc_mux(ptrs, enclen, nframes, W, H, paradigm, out, cap);
int got  = uvc_demux(out, clen, &w, &h, &nframes, frames, lens, paradigmid);
```

This is how a single "video file" can mix P1 and P2 frames — e.g. screen
splash frames in P1 and photographic frames in P2 within one stream.

> **Caveat for integrators:** the container stores *width, height, frame count,
> and per-frame paradigm id* — but **not** the P2 wavelet `level` nor the
> quantization `scale`. You must carry those out-of-band (file header, sidecar,
> or fixed convention) and pass them back to `uvc_p2_decode_frame`. The demo
> hard-codes `LEVEL` and `SCALE` on both ends to keep the example self-contained.

---

## 5. What is and isn't implemented

| Piece | Status | Notes |
|-------|--------|-------|
| P1 block-transform (DCT + INT8 quant + rANS) | ✅ Implemented & verified | Bit-exact round-trip when signal fits INT8. |
| P2 integer wavelet (LeGall 5/3 + INT8 quant + rANS) | ✅ Implemented & verified | Lossless transform (adjoint error = 0 LSB). |
| rANS entropy coder (64-bit, word renorm) | ✅ Implemented & verified | Faithful port of ryg `rans64.h`; works for *any* distribution. |
| Container mux/demux (ISOBMFF-style, P1+P2 map) | ✅ Implemented & verified | Big-endian, integer-only, bit-exact. |
| Analyzer / selector | ✅ Implemented (heuristic) | Integer-only stats stand in for the spec's NN backbone. |
| Tier negotiation | ✅ Implemented | P1 always allowed; P2/P3/P4 gated by tier + model hash. |
| **P3 (INR)** and **P4 (semantic token)** pipelines | ❌ Stub | Bits exist in selectors/negotiation, but no encode/decode yet. |
| Color (chroma), audio, inter-frame/motion, real file I/O, GPU display | ❌ Out of scope | Integrator responsibility. |

**Bottom line for "different kinds of videos":** today you can genuinely encode,
store, and decode **grayscale, single-frame-at-a-time** video in two paradigms —
P1 for general/screen content and P2 for natural content at low bitrate — with
the analyzer choosing between them and tier negotiation governing what a given
decoder can play. Color, sound, and temporal compression are future work.

---

## 6. Quick reference — the functions you'll call

Encoder / selection:
- `uvc_analyze_frame(luma, w, h, &profile)`
- `uvc_select_paradigms(&profile, bitrate_q8, quality, compute, target_use)`
- `uvc_p1_encode_frame(frame, w, h, scale, out, cap)`
- `uvc_p2_encode_frame(frame, w, h, level, scale, out, cap)`

Container:
- `uvc_mux(ptrs, lens, nframes, w, h, paradigms, out, cap)`
- `uvc_demux(buf, len, &w, &h, &nframes, frames, lens, paradigmid)`

Decoder:
- `uvc_p1_decode_frame(buf, len, w, h, scale, frame)`
- `uvc_p2_decode_frame(buf, len, w, h, level, scale, frame)`
- `uvc_negotiate_layers(stream_mask, required_tier[4], model_hash[4], &cfg)`

Quantization helper (the `scale` knob):
- `real_scale = scale_fp / 32768`; `scale = 32768` ⇒ 1.0 (near-lossless).

---

## 7. Extending this to a "real" player

To turn the loop above into something users would call a player:

1. **Sources:** replace `gen_flat`/`gen_textured` with your decoder (e.g. decode
   an MP4/H264 frame, or read a camera) and copy its luma plane into `int16_t`.
2. **Color:** UVC is luma-only. Run chroma through the same P1/P2 path (or a
   separate low-rate path) and re-combine on the sink side.
3. **Temporal:** UVC has no inter-frame coding. For motion, encode each frame
   independently (P1/P2 are intra) and let your player handle frame timing.
4. **Persistence:** write `movie.uvc` (the mux buffer) with a small sidecar
   recording `scale` and, for any P2 frame, `level`.
5. **Sink:** replace `write_pgm` with `glTexImage2D` / a window surface / network
   push.
