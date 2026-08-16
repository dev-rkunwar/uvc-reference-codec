# UVC — Universal Video Codec Reference Scaffold

A spec-driven **universal video codec** reference implementation. UVC is a hybrid
codec that selects, per segment, between four paradigms (traditional block
transform, wavelet, learned/neural, and a fully neural generative path) and
aggressively degrades quality under constrained networks while preserving
semantic integrity. This repository is the **Tier-1 reference scaffold**:
buildable, verifiable, bit-exact primitives with a passing self-test, while the
full encode/decode pipelines and neural models are stubs (see Scope below).

See [`UVC_Specification.md`](./UVC_Specification.md) for the full design
(§0–§20).

## Status

| Check | State |
|-------|-------|
| Build (CMake + C11, MinGW / gcc / clang) | ✅ |
| Self-test (`uvctest`) — all subsystems | ✅ PASS (0 failures) |
| **P1 block-transform pipeline (DCT+quant+entropy round-trip)** | ✅ |
| **P2 integer wavelet pipeline (DWT+quant+entropy round-trip)** | ✅ |
| **P3 (INR) coord-hash scaffold pipeline** | ✅ |
| **Container (ISOBMFF-style mux/demux of P1/P2/P3 frames + `uvsh` signaling)** | ✅ |
| **Segment signaling header + selector→pipeline wiring (spec §9/§10/§1)** | ✅ |
| CI (GitHub Actions, Windows + Linux) | ✅ see `.github/workflows/ci.yml` |

The self-test verifies deterministic round-trips for the entropy coder,
quantizer, bitstream reader/writer, analyzer+selector, decoder tier
negotiation, **the full P1 encode→decode pipeline** (8×8 integer DCT →
INT8 quant → entropy → IDCT, bit-exact when the signal fits the quantizer),
**and the ISOBMFF-style container** mux/demux of P1 frames (bit-exact).

## Build

Requires a C11 toolchain (GCC/Clang) and CMake ≥ 3.16 (Ninja recommended).

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

On Windows with MinGW-w64 the binary is linked statically (`-static`, guarded to
`MINGW` only) so it has no external runtime DLL dependency.

## Run the self-test

```bash
ctest --test-dir build --output-on-failure
# or directly:
./build/uvctest
```

Expected output ends with:

```
=== PASS (0 failures) ===
```

## Project layout

```
common/      Codec primitives (spec §6 entropy, §12 bitstream, §3 quantize, §9 P1 DCT, §2 container)
encoder/     analyzer.c (complexity heuristic), selector.c (paradigm choice), p1.c (DCT+quant+entropy)
decoder/     negotiate.c (tier negotiation), p1.c (P1 decode pipeline)
tools/       uvctest.c — self-test / conformance harness
UVC_Specification.md   Full design specification
```

## Scope (Tier-1 scaffold)

**Implemented and verified:**
- **P1 block-transform pipeline** (`encoder/p1.c`, `decoder/p1.c`) — 8×8 integer
  DCT (fixed-point Q13, orthonormal, adjoint ≤ 1 LSB) → INT8 quant → entropy
  coding → IDCT. Full encode→decode round-trip is bit-exact when the signal fits
  the INT8 quantizer (verified by `test_p1_pipeline`).
- Entropy coder — integer **rANS** (64-bit state, word-based renormalization,
  L=2^31), a faithful port of Fabian Giesen's public-domain `rans64.h`. This is
  the spec's normative target (§6.2) and works for ANY distribution (unlike the
  32-bit byte variant, which requires every frequency F ≥ M/256 and would desync
  on the peaked DCT-magnitude distributions UVC produces). Bit-exact and
  deterministic across platforms (little-endian 32-bit words).
- P2 wavelet pipeline — an integer **LeGall 5/3** 2D discrete wavelet transform
  (`common/p2.c`, `encoder/p2.c`, `decoder/p2.c`). The transform is lossless
  (inverse is the exact inverse, adjoint error = 0 LSB). Per-level decomposition
  of the whole frame, INT8 quantization, then the same rANS entropy coder used by
  P1. Bit-exact reconstruction when the signal fits the quantizer (verified by
  `test_p2_pipeline`). The container labels each frame's paradigm in the `uvcm`
  box (1 = P1, 2 = P2).
- P3 INR scaffold — a **multi-resolution coordinate-hash permutation**
  (MLHB-style) (`common/p3.c`). The pixel grid is permuted by a deterministic,
  content-adaptive hash seeded from a frame checksum (so the mapping is
  frame-specific, like an INR network), then INT8-quantized and rANS-coded
  exactly like P1/P2. The permutation is a true bijection reconstructed from a
  4-byte seed header, so decode is bit-exact and inverts the mapping without
  shipping it. Tier-2; lossless at scale 1.0 (verified by `test_p3_pipeline`
  and `test_p3_segment`). Container paradigm id 3.
- Quantizer — INT8 / INT4 scalar quantization with round-trip + clamping.
- Bitstream — bit-exact `bw_put` / `br_get` writer/reader (64-bit accumulator,
  multi-byte safe).
- Analyzer — integer-only (Q8.8 fixed point, no floating point) spatial
  complexity heuristic.
- Selector — paradigm decision matrix over the analyzer output.
- Negotiate — decoder tier/capability negotiation stub.
- **Segment signaling + paradigm/tier wiring** (`common/segment.c`) — the
  per-segment orchestration layer (spec §9/§10/§1). `uvc_plan_segment()` runs
  the analyzer + selector to pick the paradigm set and required decode tier from
  content + encode targets; `uvc_encode_segment()` encodes each frame with the
  chosen codec (P2 wavelet or P1 base) and writes a `uvsh` signaling header box
  (paradigm set + tier) into the container; `uvc_decode_segment()` demuxes,
  reads the `uvsh` header, runs `uvc_negotiate_layers()` against the decoder's
  tier/model config, and routes each frame to the matching pipeline (P1 base,
  P2 wavelet, or P3 coord-hash). Frames the decoder cannot satisfy are REFUSED
  (per spec §7.3/§1 base-layer fallback) rather than silently mis-decoded.
  P4 is signaled and negotiates correctly but cannot be decoded in this Tier-1
  integer scaffold (no neural runtime).
- **Container** (`common/container.c`) — ISOBMFF-style box mux/demux (`ftyp` /
  `moov` + `mvhd` + `uvcm` / `mdat`) wrapping P1 frame bitstreams in one file;
  big-endian, integer-only, bit-exact round-trip (verified by `test_container`).

**Stubs (not yet implemented):**
- P4 (semantic token) encode/decode pipeline.
- Neural model inference.
- P4 is signaled and negotiates correctly but has no decode pipeline in this Tier-1 integer scaffold.

## Determinism

All codec math is integer/fixed-point (spec §13). The entropy coder is bit-exact
and deterministic across platforms, which is what makes the conformance
self-test reproducible.

## License

Reference scaffold — see repository for license terms.
