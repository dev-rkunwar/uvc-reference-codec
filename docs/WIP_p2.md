# WIP: P2 wavelet pipeline for UVC (roadmap item in issue #1)

Status: **DONE — merged; full self-test PASS (0 failures).**

## Why this is the next item
- P1 block-transform pipeline: merged via PR #2 (commit af606a1).
- ISOBMFF container mux/demux: merged via PR #3 (commit c373995).
- rANS entropy coder (§6.2): merged via PR #4 (commit on main).
- Next roadmap item (issue #1): **P2 pipeline** ("wavelet pipeline").

## What was built
The spec names P2 "Neural Residual" (DCVC-MB), but this integer-only scaffold has
no neural runtime. The verifiable analogue we can actually build, test, and keep
deterministic is an **integer wavelet (DWT) pipeline** mirroring P1's structure:

- `common/p2.h` — shared P2 declarations (mirrors `common/p1.h`).
- `common/p2.c` — integer **LeGall 5/3** 2D DWT (`uvc_p2_fdwt` / `uvc_p2_idwt`).
  Lossless lifting (inverse is the exact inverse; adjoint error = 0 LSB). Per-level
  (default 3) decomposition of the whole frame into LL/HL/LH/HH subbands, Mallat
  (2x2 interleaved) packing.
- `encoder/p2.c` — `uvc_p2_build_dist` + `uvc_p2_encode_frame`: DWT → INT8 quant
  (shared `quant.h`) → rANS entropy (same nibble alphabet as P1). 32-byte freq
  header + rANS stream, identical bitstream shape to P1 so the container is agnostic.
- `decoder/p2.c` — `uvc_p2_decode_frame`: rANS → dequant → iDWT.

## Container support
- `common/container.c` / `.h` extended: `uvc_mux` now takes an optional `paradigms`
  array (NULL → all P1); `uvc_demux` fills an optional `out_par` with the per-frame
  paradigm ids from the `uvcm` box (1 = P1, 2 = P2). Existing signature callers pass
  NULL and remain valid (backward compatible).

## Verification (real execution, not just a clean compile)
- `tools/uvctest.c` new tests:
  - `test_p2_pipeline`: 64x64 @ level 3 → MAE=0, worst=4 (same INT8 clipping bound
    as P1), DWT adjoint exact (0 LSB).
  - `test_container_paradigm`: mux P1+P2 frames with a paradigm map, demux, confirm
    the P1/P2 labels round-trip.
- Full self-test: `=== PASS (0 failures) ===`.
- CI (clang / gcc / mingw) green.

## Docs
- `UVC_Specification.md` §21: P2 now "Implemented (integer DWT scaffold)".
- `README.md`, `docs/index.html`: P2 wavelet pipeline described; P3/P4 remain stubs.

## Next
- P3 (INR) and P4 (semantic token) remain stubs (require neural runtime / AR models).
