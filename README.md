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
| **Container (ISOBMFF-style mux/demux of P1 frames)** | ✅ |
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
- Quantizer — INT8 / INT4 scalar quantization with round-trip + clamping.
- Bitstream — bit-exact `bw_put` / `br_get` writer/reader (64-bit accumulator,
  multi-byte safe).
- Analyzer — integer-only (Q8.8 fixed point, no floating point) spatial
  complexity heuristic.
- Selector — paradigm decision matrix over the analyzer output.
- Negotiate — decoder tier/capability negotiation stub.
- **Container** (`common/container.c`) — ISOBMFF-style box mux/demux (`ftyp` /
  `moov` + `mvhd` + `uvcm` / `mdat`) wrapping P1 frame bitstreams in one file;
  big-endian, integer-only, bit-exact round-trip (verified by `test_container`).

**Stubs (not yet implemented):**
- P3 (INR) and P4 (semantic token) encode/decode pipelines.
- Neural model inference.
- P2 is implemented as an integer wavelet pipeline; remaining work is the P3/P4 neural paradigms.

## Determinism

All codec math is integer/fixed-point (spec §13). The entropy coder is bit-exact
and deterministic across platforms, which is what makes the conformance
self-test reproducible.

## License

Reference scaffold — see repository for license terms.
