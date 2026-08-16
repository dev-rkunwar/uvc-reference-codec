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

/* Mux: pack nframes bitstreams (frames[i] of frame_len[i] bytes) plus the
 * picture dimensions (w,h) and a per-frame paradigm map into one container
 * buffer. Returns total bytes written, or -1 on overflow. `paradigms` may be
 * NULL (all frames default to UVC_PARADIGM_P1); otherwise it holds nframes ids. */
int uvc_mux(const uint8_t **frames, const int *frame_len, int nframes,
            int w, int h, const uint8_t *paradigms, uint8_t *out, int cap);

/* Demux: parse a container buffer. Sets *w,*h,*nframes. If out_frames /
 * out_lens are non-NULL (each sized >= nframes), they receive pointers into buf
 * and lengths for every frame. If out_par is non-NULL (sized >= nframes) it
 * receives the per-frame paradigm ids from the uvcm box. Returns nframes (>=0),
 * or -1 on parse error. */
int uvc_demux(const uint8_t *buf, size_t len, int *w, int *h, int *nframes,
              const uint8_t **out_frames, int *out_lens, uint8_t *out_par);

#endif /* UVC_CONTAINER_H */
