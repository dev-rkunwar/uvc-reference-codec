/* container.h - UVC bitstream container (spec §2/§3, ISOBMFF-style boxes).
 * A lightweight, spec-faithful box container for muxing UVC frame bitstreams
 * (any paradigm: P1 block-transform or P2 wavelet) into a single file.
 * Box = u32 size (big-endian) | u32 type | payload.
 * Top-level boxes: ftyp (brand), moov (mvhd + uvcm), mdat (frame data).
 * Integer-only, no float; the bytes are moved verbatim so decode is bit-exact.
 *
 * The per-frame paradigm map lives in the `uvcm` box. Pass a non-NULL
 * `paradigms` to uvc_mux to label each frame (1 = P1, 2 = P2, ...); pass NULL
 * for the legacy default of all-P1. uvc_demux fills `out_par` (when non-NULL
 * and sized >= nframes) with the per-frame paradigm ids. */
#ifndef UVC_CONTAINER_H
#define UVC_CONTAINER_H

#include <stdint.h>
#include <stddef.h>

#define UVC_BRAND_FTYP 0x55766331u   /* 'UVC1' */
#define UVC_PARADIGM_P1 1
#define UVC_PARADIGM_P2 2
#define UVC_PARADIGM_P3 3
#define UVC_PARADIGM_P4 4

/* Chroma formats (per spec §3.2). 0 = monochrome (Y only). */
#define UVC_CHROMA_MONO 0
#define UVC_CHROMA_420  3   /* Y: full, Cb/Cr: half-width, half-height */
#define UVC_CHROMA_422  2   /* Y: full, Cb/Cr: half-width, full-height */
#define UVC_CHROMA_444  1   /* Y/Cb/Cr: all full resolution */

/* Plane counts for each chroma format */
#define UVC_PLANES_MONO 1
#define UVC_PLANES_420  3
#define UVC_PLANES_422  3
#define UVC_PLANES_444  3

/* Locate a top-level box by type in a parsed container buffer. Returns the
 * payload pointer and length, or (NULL,0) if absent. Used to assert the
 * signaling header is present end-to-end. */
int uvc_container_find_box(const uint8_t *buf, size_t len,
                           uint32_t type, const uint8_t **out_payload,
                           size_t *out_len);

/* Mux: pack nframes bitstreams (frames[i] of frame_len[i] bytes) plus the
 * picture dimensions (w,h) and a per-frame paradigm map into one container
 * buffer. Returns total bytes written, or -1 on overflow. `paradigms` may be
 * NULL (all frames default to UVC_PARADIGM_P1); otherwise it holds nframes ids. */
/* Mux (legacy): no signaling header — used by existing tests. */
int uvc_mux(const uint8_t **frames, const int *frame_len, int nframes,
            int w, int h, const uint8_t *paradigms, uint8_t *out, int cap);

/* Mux (extended): writes a `uvsh` signaling header carrying the segment-level
 * paradigm_set (bitmask) and tier, in addition to the per-frame uvcm map. This
 * is the normative per-segment signaling (roadmap: bitstream header/signaling
 * for paradigm + tier). Returns total bytes, or -1 on overflow. */
int uvc_mux_ex(const uint8_t **frames, const int *frame_len, int nframes,
               int w, int h, const uint8_t *paradigms, uint32_t paradigm_set,
               uint8_t tier, uint8_t *out, int cap);

/* Demux: parse a container buffer. Sets *w,*h,*nframes. If out_frames /
 * out_lens are non-NULL (each sized >= nframes), they receive pointers into buf
 * and lengths for every frame. If out_par is non-NULL (sized >= nframes) it
 * receives the per-frame paradigm ids from the uvcm box. Returns nframes (>=0),
 * or -1 on parse error. */
int uvc_demux(const uint8_t *buf, size_t len, int *w, int *h, int *nframes,
              const uint8_t **out_frames, int *out_lens, uint8_t *out_par);

/* ---- Chroma support (Milestone A: color/chroma path) ---- */
/* Extended mux: same as uvc_mux_ex but also writes a `uvcp` chroma-params box
 * and expects plane-interleaved frames (Y0, Cb0, Cr0, Y1, Cb1, Cr1...).
 * chroma_fmt: one of UVC_CHROMA_MONO/420/422/444.
 * plane_frames: array of nframes * nplanes pointers to plane bitstreams.
 * plane_lens:   array of nframes * nplanes lengths.
 * Returns total bytes written, or -1 on overflow. */
int uvc_mux_chroma(const uint8_t **plane_frames, const int *plane_lens,
                   int nframes, int w, int h, int chroma_fmt,
                   const uint8_t *paradigms, uint32_t paradigm_set,
                   uint8_t tier, uint8_t *out, int cap);

/* Extended demux: parses the uvcp box to recover chroma_fmt. If
 * out_planes is non-NULL it receives the number of planes per frame.
 * If out_plane_frames/out_plane_lens are non-NULL (each sized >=
 * nframes * nplanes) they receive pointers and lengths for every plane
 * in frame-major order. If out_par is non-NULL it receives per-frame
 * paradigm ids. Returns nframes (>=0) or -1 on parse error. */
int uvc_demux_chroma(const uint8_t *buf, size_t len,
                     int *w, int *h, int *nframes, int *out_planes,
                     const uint8_t **out_plane_frames, int *out_plane_lens,
                     uint8_t *out_par);

/* ---- file I/O (real .uvc persistence; roadmap milestone C) ---- */
/* Write a container buffer to a file. Returns 0 on success, -1 on error. */
int uvc_save_container(const char *path, const uint8_t *buf, size_t len);

/* Read a container file into `out` (caller-allocated, cap bytes). On success
 * sets *out_len to the bytes read and returns 0; returns -1 on error or if the
 * file exceeds cap. Use uvc_demux on the result. */
int uvc_load_container(const char *path, uint8_t *out, size_t cap, size_t *out_len);

#endif /* UVC_CONTAINER_H */
