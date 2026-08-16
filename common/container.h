/* common/container.h - UVC bitstream container (spec §2/§3, ISOBMFF-style boxes).
 * A lightweight, spec-faithful box container for muxing UVC_P1 frame bitstreams
 * into a single file. Box = u32 size (big-endian) | u32 type | payload.
 * Top-level boxes: ftyp (brand), moov (mvhd + uvcm), mdat (frame data).
 * Integer-only, no float; the bytes are moved verbatim so decode is bit-exact.
 */
#ifndef UVC_CONTAINER_H
#define UVC_CONTAINER_H

#include <stdint.h>
#include <stddef.h>

#define UVC_BRAND_FTYP 0x55766331u   /* 'UVC1' */

/* Mux: pack nframes P1 bitstreams (frames[i] of frame_len[i] bytes) plus the
 * picture dimensions (w,h) and a per-frame paradigm map into one container
 * buffer. Returns total bytes written, or -1 on overflow. */
int uvc_mux(const uint8_t **frames, const int *frame_len, int nframes,
            int w, int h, uint8_t *out, int cap);

/* Demux: parse a container buffer. Sets *w,*h,*nframes. If out_frames /
 * out_lens are non-NULL (each sized >= nframes), they receive pointers into buf
 * and lengths for every frame. Returns nframes (>=0), or -1 on parse error. */
int uvc_demux(const uint8_t *buf, size_t len, int *w, int *h, int *nframes,
              const uint8_t **out_frames, int *out_lens);

#endif /* UVC_CONTAINER_H */
