# UVC Roadmap — Implementation Status & Next Steps

A living roadmap for the UVC reference codec. It maps the design's
[implementation priority plan](../UVC_Specification.md#18-implementation-priority-first-12-weeks)
(§18) against what actually exists in this repository today, and proposes the
next engineering milestones. It complements (and does not duplicate) the
[README status table](../README.md#status) and the
[playing-videos conformance notes](./playing_videos.md).

Last updated: 2026-08-16. Covers work through PRs #7–#10 (P1–P4 pipelines,
segment signaling, CMake demo target, P3/P4 scaffolds).

---

## 1. How to read this doc

The UVC spec (§4–§8) describes four paradigms whose *full* realization involves
trained neural networks (Neural-ALF, DCVC-MB, MNeRV, PAT-VCM). This repository is
the **Tier-1 reference scaffold** (`README.md`): verifiable, bit-exact,
integer-only primitives that exercise the *entire codec plumbing* (analyzer →
selector → paradigm pipeline → segment signaling → container → tier
negotiation) **without** requiring trained models. Where the spec calls for a
neural network, we ship a faithful *integer analogue* that fills the same role
in the bitstream and is tested end-to-end.

Status legend:

| Tag | Meaning |
|-----|---------|
| ✅ **Done** | Real, tested pipeline; self-test passes; CI green. |
| 🟡 **Scaffold** | Integer analogue in place and wired, but it stands in for a learned model (not bit-identical to the spec's NN). |
| ⬜ **Stub** | Signaled / negotiated, but no encode/decode path yet. |
| 🔲 **Out of scope** | Named in the spec but explicitly deferred to integrators. |

---

## 2. Paradigm status (P1–P4)

| Paradigm | Spec role | Repo state | What's real | What's a stand-in |
|----------|-----------|------------|-------------|-------------------|
| **P1 — Traditional++** | Enhanced VVC base (DCT + Neural-ALF) | ✅ Done | 8×8 integer DCT (Q13, adjoint ≤1 LSB) → INT8 quant → rANS → IDCT. Bit-exact round-trip. `encoder/p1.c`, `decoder/p1.c` | Neural-ALF in-loop filter is **not** implemented (base path only) |
| **P2 — Neural Residual** | Conditional NVC (DCVC-MB) | ✅ Done | Integer **LeGall 5/3** 2D DWT (lossless, adjoint 0 LSB) → INT8 quant → rANS. Tier-2. `common/p2.c`, `encoder/p2.c`, `decoder/p2.c` | The learned DCVC-MB *residual network* is replaced by the exact integer wavelet |
| **P3 — Implicit Neural Repr.** | MNeRV per-video overfit | 🟡 Scaffold | Multi-resolution **coordinate-hash permutation** (MLHB-style), frame-checksum-seeded bijective hash → INT8 quant → rANS. Lossless at scale 1.0. Tier-2. `common/p3.c` | The MNeRV MLP is replaced by the hash permutation (same role: content-adaptive frame-specific mapping) |
| **P4 — Semantic / Task** | VCM / PAT-VCM tokens | 🟡 Scaffold | 2×-downsampled **coarse token map** → INT4 token → rANS. Lossy/low-rate (~0.6 bpp) structure layer. Exercisesthe **model-hash gate** (tier≥2 + held hash). `common/p4.c` | The learned semantic tokenizer is replaced by the coarse mean-token map |

All four are wired through the same container + segment-signaling + tier
negotiation layer, so a "P1+P2", "P1+P3", or "P1+P4" segment encodes, signals,
and decodes with correct capability fallback (a decoder that can't satisfy a
frame **refuses** it rather than mis-decoding — spec §3.3 / §1).

---

## 3. Subsystem status

| Subsystem | State | Notes |
|-----------|-------|-------|
| Entropy coder (rANS, 64-bit) | ✅ Done | Faithful port of ryg `rans64.h`; works for *any* distribution. `common/rans.c`. |
| Quantizer (INT8 / INT4) | ✅ Done | Scalar round-trip + clamp. `common/quant.h`. |
| Bitstream reader/writer | ✅ Done | Bit-exact `bw_put` / `br_get`. `common/bitstream.c`. |
| Content Analyzer | ✅ Done | Integer-only (Q8.8) spatial-complexity heuristic. `encoder/analyzer.c`. |
| Paradigm Selector | ✅ Done | Decision matrix over analyzer output. `encoder/selector.c`. |
| Tier negotiation | ✅ Done | `decoder/negotiate.c`; P4 adds the model-hash gate. |
| Segment signaling (`uvsh`) | ✅ Done | `uvc_plan_segment` / `uvc_encode_segment` / `uvc_decode_segment`. `common/segment.c`. |
| Container (ISOBMFF-style) | ✅ Done | `ftyp`/`moov`+`mvhd`+`uvcm`/`mdat`, `uvsh` signaling box, big-endian, bit-exact. `common/container.c`. Real file I/O via `uvc_save_container` / `uvc_load_container` (milestone C). |
| Playback demo | ✅ Done | `tools/uvcplay_demo.c`, now a CMake target. See `docs/playing_videos.md`. |
| Self-test (`uvctest`) | ✅ Done | All subsystems + P1–P4 pipelines; `=== PASS (0 failures) ===`. |
| CI | ✅ Done | GitHub Actions: ubuntu clang/gcc + windows mingw. |

---

## 4. Gap vs. the spec's full vision

The scaffold is deliberately *not* the spec's final codec. Open gaps:

1. **No trained neural models.** P2/P3/P4 use integer analogues (wavelet,
   hash-permutation, coarse token map). These validate the wiring and
   round-trip deterministically, but are not the learned networks in §4–§8.
2. **No Neural-ALF in-loop filter** on P1 (§4.2).
3. **No inter-frame / motion / GOP** beyond a base+enhancement picture model
   (§3.1). Each segment is still effectively single-frame-at-a-time.
4. **No color (chroma)** — grayscale only.
5. **No real file I/O / GPU display** — integrator responsibility.
6. **No unified rate controller** (§11) or feedback loop.
7. **No model management** (§17.1) — the P4 "model hash" is a fixed constant
   (0x55667788), not a managed tokenizer.

These are the same items flagged in `README.md` (Scope → Stubs) and
`docs/playing_videos.md` (status table).

---

## 5. Proposed next milestones

Ordered by leverage (what unblocks the most spec behavior). Each should be its
own PR, committed when green.

| Milestone | Scope | Status |
|-----------|-------|--------|
| **A. Color / chroma path** | Per-plane Y+Cb+Cr routed through P1–P4. | 🔲 Pending |
| **B. Inter-frame GOP + motion** (§3.1) | P-picture prediction within a segment, P1 anchor. | 🔲 Pending |
| **C. Real container I/O** | `.uvc` read/write from disk via `stdio`. | ✅ Done — `uvc_save_container`/`uvc_load_container` in `common/container.c`; demo muxes→saves→reloads→demuxes from `movie.uvc`; self-test `test_container_fileio` asserts byte-identical save→load→demux→decode. |
| **D. Neural-ALF in-loop filter** (§4.2) | Fixed-point Wiener-style stub as first real learned slot. | 🔲 Pending |
| **E. Promote a scaffold to its NN** | e.g. replace P4 token map with trained PAT-VCM tokenizer, fixed bitstream. | 🔲 Pending |
| **F. Unified rate controller** (§11) | Analyzer↔quant-scale↔target-bitrate loop. | 🔲 Pending |

**A. Color / chroma path** — extend frames to planar Y+Cb+Cr and route the same
P1–P4 pipelines per plane. Low risk, high visible payoff (real "video").

**B. Inter-frame GOP + motion (spec §3.1)** — add P-picture prediction between
frames inside a segment, with the base P1 as the anchor. This is the biggest
step toward actual video compression ratios.

**C. Real container I/O** — read/write `.uvc` from disk via `stdio`/streaming
instead of in-memory buffers, so the demo muxes to and demuxes from a file.

**D. Neural-ALF in-loop filter (P1, §4.2)** — the first place a *real* learned
component would slot in; could start as a fixed-point Wiener-style filter stub.

**E. Promote a scaffold to its NN** — e.g. replace the P4 coarse token map with
a trained PAT-VCM tokenizer (§8), keeping the bitstream shape fixed so the
container/negotiation wiring is unchanged.

**F. Unified rate controller (§11)** — close the loop between analyzer output,
quant scale, and a target bitrate, replacing the fixed `scale_fp = 32768`.

---

## 6. Repo map (where things live)

```
common/   rans.c p2.c p3.c p4.c p1.h p2.h p3.h p4.h quant.h bitstream.h
          container.c container.h segment.c segment.h
encoder/  analyzer.c analyzer.h selector.c selector.h p1.c
decoder/  negotiate.c negotiate.h p1.c
tools/    uvctest.c  uvcplay_demo.c
docs/     ROADMAP.md  playing_videos.md  WIP_rans.md  WIP_p2.md
```

See `README.md` for the build/run quickstart and `UVC_Specification.md` for the
normative design.
