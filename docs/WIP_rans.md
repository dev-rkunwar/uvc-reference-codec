# WIP: rANS entropy coder for UVC (roadmap item §6.2)

Status: **DONE — merged into `common/rans.c`; full self-test PASS (0 failures).**
Standalone verifier `tools/dbg_rans.c` round-trips + is deterministic across all
test distributions; the same coder is ported behind the existing `rans_*` API and
verified end-to-end through the P1 pipeline (DCT+quant+entropy) and the ISOBMFF
container (MAE=0 reconstruction).

## Why this is the next item
- P1 block-transform pipeline: merged via PR #2 (commit af606a1).
- ISOBMFF container mux/demux: merged via PR #3 (commit c373995).
- Next normative piece is the integer rANS coder replacing the Huffman placeholder in
  `common/rans.c`. `rans_dist_t` supports only 16 symbols (`freq[16]`), so the P1
  nibble alphabet (4 bits/symbol = 16 symbols) is the thing we entropy-code.

## Critical constraint learned this session (do NOT re-derive)
- ryg **32-bit** `rans_byte.h` (state L=2^23, byte emission) only works when EVERY
  symbol frequency satisfies `F >= M/256`. A peaked DCT-magnitude distribution (lots of
  zeros, few large coeffs) can NEVER satisfy that -> encoder emits a different byte
  count than the decoder pulls -> stream desyncs. FATAL for UVC.
- ryg **64-bit** `rans64.h` (state L=2^31, 32-bit WORD emission) has NO `F>=M/256`
  constraint -> works for ANY distribution. THIS is the reference to port verbatim.
- Only requirement: 64-bit MULHI. Our toolchain (GCC/Clang/MinGW, 64-bit) supports
  `__uint128_t`, so `Rans64MulHi(a,b) = (uint64_t)(((unsigned __int128)a*b)>>64)`.
  (MSVC would use `__umulh`, but we don't build with MSVC.)

## rANS mechanics confirmed
- LIFO: encoder processes symbols LAST->FIRST; decoder reads FIRST->LAST. No manual
  array reversal. The byte/word stream is written BACKWARD from the end of the buffer,
  so the final state ends up at the FRONT: layout = [state][renorm bytes...].
- Decoder reads the state first, then pulls renorm words/bytes forward.
- Start state = L (valid iff L >= M; with L=2^31 and M<=65536 that holds).

## Plan (next session)
1. Replace the hand-rolled coder in `tools/dbg_rans.c` with a verbatim port of ryg
   `rans64.h`: `Rans64EncPut` / `Rans64DecAdvance` / `Rans64EncInit` / `Rans64DecInit`.
   Adapt to byte stream via explicit little-endian 32-bit word read/write (cross-platform
   determinism). Keep build_dist (power-of-two M=2^p freq table, every freq>=1, sum=M,
   remainder folded into largest symbol, p capped at 16 so freqs fit uint16).
2. Keep the 5 test distributions: skewed4 {1000,100,10,1}, uniform16, dominant
   {9000 + 15x50}, single {50}, randcounts {1..200}. All MUST round-trip + be
   deterministic (encode same stream twice -> identical bytes).
3. Once green, port into `common/rans.c` matching the existing `rans_dist_t` API
   (`rans_dist_build`, `rans_enc`, `rans_dec`) so it slots behind the current callers.
4. Add a self-test to `tools/uvctest.c` (round-trip a nibble stream), confirm full
   `=== PASS (0 failures) ===`.
5. Commit on `feat/rans-coder`, open PR against `main`, wait for CI green, squash-merge
   (do NOT push to main directly — workflow from PR #2/#3).
6. Update `UVC_Specification.md` §21 and README status, regenerate `docs/index.html`,
   update issue #1.

## Reference (canonical, public domain — Fabian Giesen)
Key functions from https://github.com/rygorous/ryg_rans/blob/master/rans64.h :
- `RANS64_L (1ull << 31)`
- `Rans64EncPut`: `x_max = ((RANS64_L >> scale_bits) << 32) * freq;` if `x >= x_max`
  { write 32-bit word (x & 0xFFFFFFFF); x >>= 32; } then
  `x = ((x / freq) << scale_bits) + (x % freq) + start;`
- `Rans64DecAdvance`: `x = freq * (x >> scale_bits) + (x & mask) - start;` then
  if `x < RANS64_L` { x = (x << 32) | *word++; }
- `scale_bits = p` (M = 1<<p), `start = cum[s]`, `freq = freq[s]`.

## Current broken state (so you don't trust it)
`tools/dbg_rans.c` currently holds a hand-rolled 64-bit single-threshold variant that
STILL mismatches (skewed4/dominant/randcounts). It is scratch only — not in the build
(CMake globs `common/*.c`, not `tools/*.c`). Replace it per the plan above.
