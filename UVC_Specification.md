# Universal Video Codec (UVC) — Implementation Specification

**Version:** 0.1.0-draft
**Date:** 2026-08-16
**Status:** Implementation-ready design specification
**License target:** Apache-2.0 (reference), implementer-friendly
**Model identity note:** This draft was authored under OpenCode Zen (hy3-free). All field layouts below are normative.

---

## 0. Scope and Introduction

UVC is a **unified, content-adaptive, multi-paradigm video coding framework**, not a single
algorithm. A single UVC bitstream carries one or more *coding layers*. Each layer is produced by
exactly one of four coding paradigms. The decoder selects, per device capability, which layers it
materializes.

The four paradigms are:

| ID | Paradigm | Purpose | Decode tier |
|----|----------|---------|-------------|
| P1 | Traditional++ | Block-based hybrid (VVC/HEVC/AV1 + neural in-loop) | Tier 1 (HW) |
| P2 | Neural Residual | Conditional residual NVC (DCVC-MB family) | Tier 2 (NPU/GPU) |
| P3 | Implicit Neural (INR) | Neural representation (MNeRV family) | Tier 2+ (parallel MLP) |
| P4 | Semantic / Task | Machine-vision scalable layer (VCM/PAT-VCM) | Tier 2+ (NPU) |

**Design goals (normative targets):**
- Decode *any* UVC bitstream on Tier-1 hardware (base layer only, no neural compute).
- Best-in-class RD for the active tier: P1+P2 targets 40–50% bitrate reduction vs VVC at equal quality.
- Single bitstream serves human viewing AND machine analytics (scalable layers).
- Cross-platform **bit-exact** decode (fixed-point reference + conformance vectors).
- Graceful degradation: missing model → decode base layer only.

This document is the authoritative source for implementers. Companion artifacts (conformance
vectors, reference model skeleton) are referenced by path in §17.

---

## 1. Normative Conformance Levels (Decoder Tiers)

```
Tier 1  LEGACY      P1 only. MUST decode base layer. HW VVC/HEVC/AV1 decoder.
                   ZERO neural compute. Browser/TV/IoT.
Tier 2  ENHANCED   P1 + P2 (+ optional P3/P4 if NPU present).
                   NPU/DSP/GPU, >=2 TOPS INT8. Mixed precision INT8/INT16.
Tier 3  FULL       P1 + P2 + P3 + P4. GPU FP16/BF16, >=50 TOPS.
                   Generative INR, semantic analytics, volumetric.
```

Every decoder SHALL advertise its tier and the set of model hashes it holds in the
`uvcC` configuration box (§4.3). An encoder SHALL only emit a layer whose required tier exceeds
the *lowest common denominator* of the target audience if the base layer remains independently
decodable.

---

## 2. Bitstream Container (ISOBMFF)

UVC uses ISO Base Media File Format (ISO/IEC 14496-12) with UVC-specific boxes. The major brand
is `uvc1`. Compatible brands MUST include at least one traditional brand for legacy players:
`avc1`, `hev1`, `hvc1`, `av01`.

### 2.1 Box tree

```
ftyp (major=uvc1, compatible=hev1,av01,...)
moov
  mvhd
  trak                          <- base layer track (P1, traditional NALUs)
    tkhd, mdia, minf, stbl
  trak                          <- enhancement track group (P2/P3/P4)
    tkhd, mdia, minf, stbl
      stsd
        uvcE                    <- UVC enhancement sample entry
  uvcC                          <- UVC configuration (capability advertisement)
  uvcm                          <- paradigm map (per-GOP layer assignment)
mdat                          <- interleaved samples
```

### 2.2 `uvcE` enhancement sample entry (stsd child)

```
class UVCEnhancementSampleEntry extends VisualSampleEntry {
  unsigned int(8)  paradigm_mask;        // bit0=P2, bit1=P3, bit2=P4
  unsigned int(8)  layer_count;
  for (i=0; i<layer_count; i++) {
    unsigned int(8)  paradigm_id;        // 2,3,4
    unsigned int(8)  required_tier;      // 1..3
    unsigned int(32) model_hash;        // FNV-1a of model weights
    unsigned int(16) model_patch_version;// semantic MAJOR.MINOR
  }
}
```

### 2.3 `uvcC` configuration box

```
class UVCDecoderConfigBox extends FullBox('uvcC', 0, 0) {
  unsigned int(8)  spec_version_major;
  unsigned int(8)  spec_version_minor;
  unsigned int(8)  spec_version_patch;
  unsigned int(8)  min_decoder_tier;     // minimum tier to decode anything
  unsigned int(16) flags;                 // bit0: deterministic_int_only
  unsigned int(8)  entropy_coding_id;     // 0=CABAC,1=rANS,2=learned-AR
  unsigned int(16) max_picture_width;
  unsigned int(16) max_picture_height;
  unsigned int(8)  chroma_format;         // 0=400,1=420,2=422,3=444
  unsigned int(8)  bit_depth;             // 8 or 10
}
```

---

## 3. Sample / NALU Structure

### 3.1 Picture timing model

A **GOP** (Group of Pictures) in UVC is the unit of paradigm assignment. The `uvcm` box maps each
GOP index to a `(paradigm_set, bit_budget_split)` tuple. Within a GOP:
- Exactly one **base picture** (P1), decodable standalone.
- Zero or more **enhancement pictures** (P2 residual / P3 weights / P4 tokens).

### 3.2 UVC NALU header (P2/P3/P4 payloads)

```
nal_unit_header {
  forbidden_zero_bit   u(1) = 0
  nal_layer_id        u(6)   // 0=base(P1), 1=P2, 2=P3, 3=P4
  nal_type            u(3)   // 0=IDR,1=inter,2=param,3=model,4=token
  temporal_id         u(3)
  reserved            u(3) = 0
}
```

### 3.3 Layer dependency rule

```
Layer 0 (P1) : no dependency. MUST be present.
Layer 1 (P2) : depends on Layer 0 of same picture + referenced P1 pictures.
Layer 2 (P3) : self-contained weights + frame index table; independent of P1.
Layer 3 (P4) : depends on Layer 0 (base pixels) for human view; machine view
               independent if base pixels reconstructed from P4 tokens alone.
```

---

## 4. Paradigm P1 — Traditional++ (Enhanced VVC base)

### 4.1 Encoder pipeline (normative block order)

```
Input YUV (bit_depth B, chroma C)
  -> CTU partition: QTMT + NN-guided split (§4.4)
  -> Intra/Inter prediction (VVC tools: MRL, Affine, SbTMVP, GPM)
  -> Residual
  -> Transform (MTS) + Quant (perceptual QP offset, §4.5)
  -> Entropy: CABAC (entropy-conserving binarization, §6.1)
  -> In-loop: Deblock -> SAO -> ALF -> CCALF -> Neural-ALF (§4.6)
  -> Reconstructed frame buffer
```

### 4.2 Neural in-loop filter (Neural-ALF)

Replace/extend VVC ALF with a lightweight CNN:

```
NeuralALF {
  input:  reconstructed luma (7x7 window) + QP + slice type (one-hot)
  arch:   3 conv layers, 16/32/3 channels, 3x3, ReLU, residual
  params: ~12k floats -> INT8 (per-sequence, sent in param NALU)
  compute: applied per CTU, fused with CCALF
}
```

Reference: VVC neural in-loop filtering [2607.05737, 2607.12186]; DLF survey [2607.16319].

### 4.3 Perceptual bit allocation

Per-frame QP offset derived from a learned JND map:

```
qp_offset(pixel_block) = round( lambda * (1 - JND_confidence(block)) )
// JND_confidence in [0,1], from a tiny saliency net (MobileNetV3-SSIM head)
```

Reference: bit-allocation transfer / perceptual RDO [2608.07799].

### 4.4 NN-guided CTU split (optional, encoder-only)

A 2-class MLP proposes split/merge at QTMT node; final decision still RDO-closed
(RD cost MUST be computed; NN is a candidate generator only, never changes decoded bits).

### 4.5 Quantization

Standard VVC quantizer. QP range 0..63. Perceptual offset clamped to [-8, +8].

### 4.6 Determinism

P1 decode is integer-exact by construction (VVC fixed-point). No floating point in the
decoder path. Compliant with VVC/H.266 fixed-point spec.

---

## 5. Paradigm P2 — Neural Residual (Conditional NVC)

### 5.1 Encoder architecture (DCVC-MB family)

```
For each frame t (except IDR):
  mv_hat, mv_latent  = MotionEncoder( ref_frames, feature_prev )
  mv_latent_q        = Quantize( mv_latent, q_step_mv )
  mv_refined         = MVRefinement( mv_hat, MultiHypothesis(refs) )
  res                = Frame_t - Warp( ref, mv_refined )
  ctx                = ContextFusion( features, SSM/Mamba state )
  res_latent         = ResidualEncoder( res, ctx )
  res_latent_q       = Quantize( res_latent, q_step_res )
  # Autoregressive context (MoE gating) for entropy
  z_hat              = AutoregressiveContext( res_latent_q )   # decoder-side model
  bits               = EntropyModel( res_latent_q | z_hat, mv_latent_q )
```

Reference: DCVC [CVPR'22]; DCVC-MB (Mamba B-frame) [2607.14305];
multi-hypothesis [2510.12479]; MoE context [2608.10947].

### 5.2 Cross-platform deterministic quantization (NORMATIVE)

To guarantee bit-exact decode across x86/ARM/GPU/ASIC:

```
1. ALL tensor ops in P2 decoder are FIXED-POINT (Qm.n, integer-only).
2. Model weights: INT8 symmetric per-channel, scale stored as INT16 fixed-point.
3. Activations: INT16. Accumulators: INT32.
4. Entropy coder: rANS with 31-bit state, fixed LUT tables (§6.2).
5. NO LayerNorm/Softmax in float; use integer power-iteration approximations.
6. Rounding: stochastic during TRAINING ONLY; inference uses round-half-to-even.
```

Reference: MLVC cross-platform [2606.28027]; Streamable NVC determinism [2608.00483];
JOMP mixed-precision [2606.13110].

### 5.3 Quantization schedule (JOMP)

Joint optimization of mixed-precision per layer. Stored as a 1-byte precision map per model
(0=INT4,1=INT8,2=INT16). Sent in model NALU (§3.2 type=3).

### 5.4 Decode (Tier 2)

```
mv_latent_q -> MVDecoder -> mv_hat -> Warp(ref) -> pred
res_latent_q -> EntModel -> res_hat
recon = pred + res_hat  (INT16, clipped to [0, (1<<B)-1])
optional: Neural-ALF refinement (shared with P1)
```

### 5.5 Complexity budget

P2 encode: 10–50× VVC (cloud/server only). P2 decode: 2–8 ms/frame 1080p on 4 TOPS NPU.

---

## 6. Entropy Coding (normative)

### 6.1 P1: CABAC

Standard VVC CABAC. No change. Binarization uses entropy-conserving transform coding
[2606.23753] (optional tool, signaled in slice header).

### 6.2 P2/P3/P4: rANS (range asymmetric numeral system)

```
rANS state: uint32, 31-bit modulus (R = 1<<23 typical)
encoder:
  push(state, sym):
    freq = CDF[sym+1] - CDF[sym]
    state = (state / freq) * RANGE + (state % freq) + CDF[sym]
    while state >= 1<<31: emit(state & 0xFF); state >>= 8
decoder:
  sym = search(CDF, state % RANGE)
  state = (state / RANGE) * freq + (state % RANGE) - CDF[sym]
  while state < RANGE: state = (state<<8) | getbyte()
```

CDF tables: learned, sent per-GOP in param NALU (max 1 KB). All arithmetic integer-only.

### 6.3 P4 semantic tokens: variable-length + rANS

Task tokens use the same rANS with task-specific CDF.

---

## 7. Paradigm P3 — Implicit Neural Representation (INR)

### 7.1 Encoder (MNeRV family, per-video overfit)

```
Video clip (N frames) ->
  ContentAnalyzer -> INR config (depth, width, embedding dim)
  Embeddings E[0..N-1]  (per-frame latent vectors)
  Weight optimization: minimize Σ_t || Decoder(E_t) - Frame_t || + λ*||weights||
  Low-rank convolution factorization [2603.18261]
  Progressive layering: L layers; layer k adds detail (§7.3)
  Weight quantization: HAMP-LIC [2608.12239]
```

Reference: NeRV [ECCV'22]; ENeRV; HNeRV; MNeRV [2407.07347];
LRConv-NeRV [2603.18261]; wavefront parallel decode [2607.19082];
NeRV-Diffusion generative [2509.24353].

### 7.2 Bitstream (P3)

```
model NALU (type=3):
  arch_id u(8)           // selects decoder graph
  embedding_dim u(16)
  num_frames u(32)
  precision_map (JOMP)
  quantized_weights[]    // INT8/INT4 payload
frame_index NALU (type=2):
  for t in frames: embedding vector (quantized)
```

### 7.3 Progressive / scalable decode

```
Decoder(k_layers):
  for l in 0..k: frame_t += Layer_l(E_t)
  quality Q_k increases monotonically with k
  Tier-1 fallback: NONE (P3 needs NPU); if absent, skip P3, use P1.
```

### 7.4 Decode speed

Parallel MLP: all frames decoded independently → 1000× faster than autoregressive NVC.
<1 MB model for 1080p30 short clip.

---

## 8. Paradigm P4 — Semantic / Task-Oriented (VCM)

### 8.1 Scalable human+machine

```
Base layer (P1): human-viewable, low bitrate, legacy HW decode.
Semantic layer (P4): auxiliary tokens for machine tasks.
  PAT-VCM plug-and-play tokens [2604.13294]:
    token_stream = TokenEncoder( base_features, task_heads )
    machine_task_output = TaskModel( base_features + tokens )
  Multi-task JRD [2604.09421]:
    optimize R for {human PSNR, task mAP, task F1} with visibility thresholds
```

Reference: Scalable human+machine [2208.02512]; VNVC [2306.10681];
PAT-VCM [2604.13294]; MT-JRD [2604.09421]; task JRD [2607.19515];
HPC volumetric [2407.09026]; event fusion [2607.28020].

### 8.2 Bitstream (P4)

```
token NALU (type=4):
  task_mask u(16)         // which tasks tokens support
  token_dim u(16)
  precision_map
  quantized_tokens[]      // INT8
```

### 8.3 Machine-only mode

If `task_mask` excludes human and base pixels not needed, decoder MAY reconstruct only
feature maps (no pixel decode) → 70–90% bitrate vs HEVC for equal task accuracy.

---

## 9. Content Analyzer (normative interface)

```
ContentAnalyzer (lightweight, runs encoder-side; decoder does NOT need it):
  input:  segment (16-32 frames, downscaled to 256x256)
  backbone: MobileNetV3-small (4.2M params, INT8)
  heads:
    scene_type     : {natural, screen, animation, volumetric, synthetic, lowlight}
    motion_complex : {static, slow, fast, complex}
    texture_rich   : {flat, textured, detailed, synthetic}
    target_use     : {human, machine, both}
    quality_pref   : {speed, balance, best}
  latency: <5 ms/segment on CPU
  output: ContentProfile struct (§9.1)
```

### 9.1 ContentProfile struct

```
struct ContentProfile {
  uint8 scene_type;
  uint8 motion_complex;
  uint8 texture_rich;
  uint8 target_use;
  uint8 quality_pref;
  float spatial_complex;     // 0..1
  float temporal_complex;    // 0..1
  float semantic_complex;    // 0..1
};
```

---

## 10. Paradigm Selector (normative decision matrix)

```
select_paradigms(profile, target_bitrate, target_quality,
                 target_compute, target_tasks) -> paradigm_set:

  # Hard constraints first
  if target_compute == REAL_TIME_HW:
      return {P1}                      # only HW-decodable
  if target_tasks != NONE and target_use == MACHINE:
      base = {P1, P4}
  else:
      base = {P1}

  # Content-driven augmentation
  switch (profile.scene_type):
    case NATURAL:
      if profile.temporal_complex > 0.6:
          add P2                    # neural wins on high motion
      else:
          # P1 suffices
    case SCREEN:
      add P3 if target_compute >= TIER2   # INR great for graphics
    case ANIMATION:
      add P3
    case VOLUMETRIC:
      add P3 (HPC variant)
    case LOWLIGHT:
      add P2 (event-fusion optional)
    case SYNTHETIC:
      add P3

  # Ultra-low bitrate
  if target_bitrate < 0.1 bpp:
      add P2 (discrete VQ variant [2607.02562])

  # Progressive requested
  if target_quality == PROGRESSIVE:
      add P3

  return base | augment
```

Decision table (summary, encoder-side reference):

| Profile | Primary | Secondary | Bitrate |
|---------|---------|-----------|---------|
| Natural+HiMotion+Human | P2 | P1 | 1–10 Mbps |
| Natural+LoMotion+Human | P1 | — | 0.5–5 Mbps |
| Screen/Graphics | P1 | P3 | 0.1–2 Mbps |
| Animation | P3 | P1 | 0.5–3 Mbps |
| Volumetric/NeRF | P3(HPC) | — | 1–50 Mbps |
| Low-light/Event | P2 | — | 0.5–5 Mbps |
| Machine-only | P4 | P1 | 0.01–1 Mbps |
| Human+Machine | P4 | P1/P2 | 0.5–10 Mbps |
| Ultra-low (<0.1bpp) | P2(dVQ) | P4 | 0.01–0.1 |
| Realtime-HW | P1 | — | any |
| Progressive | P3 | P4 | any |

---

## 11. Unified Rate Controller (normative algorithm)

```
allocate(active_paradigms, target_bitrate, profile) -> budget_map:

  # 1. Cost models (learned offline, table lookup at runtime)
  for p in active_paradigms:
     cost[p] = LookupCostModel(p, profile)   # -> {bits, psnr, msssim, time}

  # 2. Lagrangian allocation per GOP
  R_rem = target_bitrate
  T_rem = target_compute_budget
  for p in active_paradigms (priority order P1>P4>P2>P3):
     r_p = SolveSingleParadigmRate(R_rem, lambda, cost[p])
     if time(p, r_p) <= T_rem:
        budget[p] = r_p
        R_rem -= expected_bits(p, r_p)
        T_rem -= time(p, r_p)
     else:
        budget[p] = 0   # drop layer if over compute

  # 3. Feedback loop [2604.20104]
  actual_bits = encode_and_measure()
  if actual_bits > target_bitrate * 1.05:
     lambda *= 1.1; re-allocate (next GOP)
  elif actual_bits < target_bitrate * 0.95:
     lambda *= 0.95; re-allocate
```

### 11.1 Quality metrics

| Target | Metric | Used in |
|--------|--------|---------|
| Human | VMAF, MS-SSIM, LPIPS, PSNR-HVS | P1/P2/P3 RDO |
| Machine | mAP, F1, Top-1, cosine-sim | P4 JRD |
| Generative | FVD, FID | NeRV-Diffusion |
| Scalable | layer-wise RD | P3 progressive |

---

## 12. Quantization Specification (normative)

### 12.1 Weight quantization (all neural paradigms)

```
per-channel symmetric INT8:
  scale_c = max(|W_c|) / 127
  W_q = round(W_c / scale_c)              # clamp [-128,127]
  store scale_c as INT16 fixed (Q1.15)
per-channel INT4 (JOMP-selected layers):
  scale_c = max(|W_c|) / 7
  W_q = round(W_c / scale_c)              # clamp [-8,7]

HAMP-LIC [2608.12239] for INR weight compression (mixed per-tensor).
```

### 12.2 Activation quantization

INT16 dynamic per-tensor (min/max tracked at runtime, sent in NALU header).

### 12.3 Rounding

Inference: round-half-to-even. Training: stochastic (Straight-Through Estimator).

---

## 13. Cross-Platform Determinism (normative, CRITICAL)

A decoder at tier >= required_tier, given the same bitstream and model hash, SHALL produce
**bit-identical** output across all conforming implementations.

Enforcement:
1. Fixed-point reference model (Python + C++ integer) in §17.
2. Conformance vectors: 10 clips × 3 tiers, expected MD5 of decoded YUV.
3. No `float`, `double`, or platform-intrinsics in decode math.
4. rANS CDF tables and LUTs are part of the bitstream (no implementation freedom).
5. Model hash (FNV-1a over quantized weights) MUST match `uvcC.model_hash`.
6. Any non-determinism found in testing → implementation bug, not spec ambiguity.

Reference: MLVC [2606.28027], Streamable NVC [2608.00483].

---

## 14. Reference Software Architecture

```
uvc/
  encoder/
    analyzer.py            # ContentAnalyzer (MobileNetV3)
    selector.py            # paradigm decision matrix
    p1_vvc/                # wraps VVenC + Neural-ALF plugin
    p2_nvc/                # DCVC-MB (PyTorch -> ONNX -> fixed-point)
    p3_inr/                # MNeRV optimizer
    p4_vcm/                # PAT-VCM token encoder
    mux.py                 # ISOBMFF writer (ftyp, moov, uvcC, uvcm, mdat)
    rate_ctrl.py           # unified allocator
  decoder/
    demux.py               # ISOBMFF parser, layer extractor
    p1_vvc/                # VVC HW/SW decoder (libvvenc-decode)
    p2_nvc_fixed/          # INTEGER-ONLY reference decoder (C++)
    p3_inr_fixed/          # integer MLP decoder
    p4_vcm/                # token -> task features
    negotiate.py           # tier + model-hash capability check
  common/
    rans.c/.h              # integer rANS
    quant.c/.h             # INT8/INT4 dequant
    bitstream.c/.h         # NALU reader/writer
    conformance/           # test vectors + MD5
  tools/
    uvcconform.py          # runs conformance suite
    bench.py               # RD + complexity benchmark
```

### 14.1 Build toolchain (already installed on this machine)

- GCC 16.1.0 (MinGW-w64, WinLibs) — verified compiling C++20.
- Clang/LLVM 22.1.8 — verified.
- CMake 4.0.1, Ninja 1.13.
- Python 3.11 (uv-managed venvs).
- Reference NVC in PyTorch 2.7 (CPU) for training/export; ONNX for inference.

### 14.2 Build commands

```
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DUVC_TIER=3
cmake --build build
```

---

## 15. API Definition (C ABI, normative)

```c
typedef struct UVCContentProfile {
  uint8_t scene_type, motion_complex, texture_rich, target_use, quality_pref;
  float   spatial_complex, temporal_complex, semantic_complex;
} UVCContentProfile;

typedef struct UVCDecoderConfig {
  uint8_t tier;                 // 1,2,3
  uint32_t held_model_hashes[8];
  uint8_t  held_model_count;
} UVCDecoderConfig;

// Encoder
uvc_encoder* uvc_encoder_create(const char* preset); // "realtime","quality","machine"
void uvc_encoder_set_target(uvc_encoder*, float bitrate_mbps,
                            UVCQualityMode mode, UVCTaskSet tasks);
int  uvc_encoder_push(uvc_encoder*, const uint8_t* yuv, int w, int h,
                      UVCContentProfile* prof);
int  uvc_encoder_pull_packet(uvc_encoder*, uint8_t* out, int* size);

// Decoder
uvc_decoder* uvc_decoder_create(UVCDecoderConfig* cfg);
int  uvc_decoder_push_packet(uvc_decoder*, const uint8_t* pkt, int size);
// returns decoded YUV for the highest tier the config allows
int  uvc_decoder_pull_frame(uvc_decoder*, uint8_t* yuv, int* w, int* h);
// machine output (P4): returns feature tensor for requested task
int  uvc_decoder_pull_task(uvc_decoder*, uint16_t task_id, float* feat, int* dim);
```

---

## 16. Conformance & Test Plan

```
uvcconform.py:
  for each clip in conformance_set:        # 10 clips, mixed content
    for each tier in [1,2,3]:
      decode -> yuv
      assert md5(yuv) == expected_md5[clip][tier]
  for each NVC model:
    assert fnv1a(weights) == bitstream.model_hash
  report PASS/FAIL with first mismatch offset
```

Conformance clips (to be captured): `city_4k`, `screen_ui`, `anime_clip`, `nerf_spin`,
`lowlight_cam`, `surveil_1080p`, `talking_head`, `gaming_cap`, `medical_4k`, `synthetic_gen`.

---

## 17. Deployment & Distribution

### 17.1 Model management

- Encoder (cloud): continuous training; semantic versioning MAJOR.MINOR.PATCH.
- Decoder: base models shipped frozen with OS/app; delta updates via CDN (<5 MB).
- Capability negotiation via `uvcC`; missing model → decode base layer only.

### 17.2 Hardware co-design

- Tier-1: reuse existing VVC/HEVC/AV1 fixed-function decoders (no new silicon).
- Tier-2: NPU runs P2/P3/P4 integer graphs (INT8/INT4).
- Tier-3: GPU FP16 for generative INR + analytics.

### 17.3 Standardization path

1. Open-source reference (Apache-2.0) → prove gains.
2. Submit to MPEG/VCEG as "Unified Multi-Paradigm Video Coding".
3. IETF draft for `uvcC` / `uvc1` ISOBMFF brands.

---

## 18. Implementation Priority (first 12 weeks)

| Week | Task | Ref |
|------|------|-----|
| 1–2 | ContentAnalyzer + Selector (CPU) | §9,§10 |
| 2–4 | P1: VVenC + Neural-ALF plugin | §4, [2402.08397] |
| 4–6 | uvcC / uvcm / ISOBMFF mux+demux | §2,§3 |
| 4–8 | P2: DCVC-MB → ONNX → fixed-point | §5, [2607.14305] |
| 6–10 | Cross-platform determinism + conformance | §13, [2606.28027] |
| 8–12 | P3: MNeRV progressive decoder | §7, [2407.07347] |
| 10–12 | Unified rate controller + feedback | §11, [2604.20104] |
| 12+ | P4: PAT-VCM tokens | §8, [2604.13294] |

---

## 19. Key References (normative citations)

| Topic | Paper / ID |
|-------|-----------|
| Cross-platform NVC determinism | MLVC [2606.28027] |
| DCVC-MB (Mamba B-frame) | [2607.14305] |
| MNeRV (progressive INR) | [2407.07347] |
| PAT-VCM (semantic tokens) | [2604.13294] |
| Perceptual bit allocation | [2608.07799] |
| JOMP mixed-precision | [2606.13110] |
| HAMP-LIC weight quant | [2608.12239] |
| Streamable NVC determinism | [2608.00483] |
| Neural in-loop VVC filter | [2607.05737], [2607.12186] |
| DLF survey | [2607.16319] |
| Hybrid VVC+NN framework | [2402.08397] |
| Scalable human+machine | [2208.02512] |
| VNVC | [2306.10681] |
| HPC volumetric | [2407.09026] |
| Event-camera fusion | [2607.28020] |
| Feedback rate control | [2604.20104] |
| Ultra-low bitrate dVQ | [2607.02562] |
| NeRV-Diffusion | [2509.24353] |
| LRConv-NeRV | [2603.18261] |
| Multi-task JRD | [2604.09421] |
| Entropy-conserving binarization | [2606.23753] |

---

## 20. Open Questions (research backlog)

1. Universal motion field spanning P1/P2/P3/P4.
2. Joint RD-Task Lagrangian for multiple simultaneous machine tasks.
3. Seamless mid-stream paradigm switching without artifacts.
4. Pixel-vs-generative boundary for NeRV-Diffusion storage.
5. Privacy-preserving encrypted semantic tokens.
6. Neural scaling laws to predict RD/complexity without encoding.

---

*End of specification. Implementers SHOULD begin with §14 (toolchain already present),
§2/§3 (container), and §9/§10 (analyzer + selector) to produce a minimal viable encoder
that emits a Tier-1-decodable P1 base layer, then layer P2/P3/P4 incrementally.*

---

## 21. Reference Scaffold Status (companion C code)

The companion `C11` reference scaffold (this repo) currently implements the
**verifiable core** of the design; the neural paradigms (P2–P4) and the ISOBMFF
container are intentionally stubbed pending the roadmap in issue #1.

| Component | Status in scaffold | Notes |
|-----------|--------------------|-------|
| Entropy coder (`common/rans.*`) | Substitution | Spec §6.2 mandates integer rANS; the scaffold ships a **canonical Huffman** coder behind the same `rans_*` API for verifiability. The normative rANS is a drop-in replacement. |
| Quantizer (`common/quant.h`) | Implemented | Fixed INT8/INT4; integer-only, `uint16` Q1.15 scale (real_scale < 2). |
| Bitstream (`common/bitstream.*`) | Implemented | MSB-first writer/reader; 64-bit accumulator. |
| Analyzer (`encoder/analyzer.*`) | Implemented | Integer Q8.8 heuristic, no float. |
| Selector (`encoder/selector.*`) | Implemented | Q8.8 decision matrix. |
| **P1 pipeline** (`encoder/p1.*`, `decoder/p1.*`) | **Implemented (round-trip)** | Integer 8×8 DCT (fixed-point Q13, orthonormal, adjoint ≤ 1 LSB) → INT8 quant → entropy (nibble alphabet) → IDCT. Self-test proves bit-exact reconstruction when the signal fits int8. |
| Tier negotiation (`decoder/negotiate.*`) | Implemented | Stub tier map (LEGACY/ENHANCED/FULL). |
| **Container** (`common/container.*`) | **Implemented (round-trip)** | ISOBMFF-style box mux/demux (`ftyp`/`moov`+`mvhd`+`uvcm`/`mdat`) wrapping P1 frame bitstreams; big-endian boxes, integer-only. |
| P2 / P3 / P4 | Stub | Roadmap issue #1. |

Self-tests (`tools/uvctest.c`) cover rANS round-trip + determinism, quant
round-trip, bitstream round-trip, analyzer+selector, tier negotiation, the
**P1 block-transform pipeline**, and the **ISOBMFF-style container mux/demux**
end-to-end. Run `ctest` (or `uvctest` directly).
