/* common/chroma.h - Color space and chroma subsampling utilities.
 * Integer-only, spec §3.2 compliant. Used by Milestone A (color/chroma path).
 */
#ifndef UVC_CHROMA_H
#define UVC_CHROMA_H

#include <stdint.h>
#include <stddef.h>
#include "container.h"

/* Convert RGB (uint8, 0..255) to YCbCr (int16, full range 0..255 shifted
 * by 0 for Y, -128 for Cb/Cr). r,g,b are arrays of length w*h. Output planes
 * are int16 arrays of appropriate size. For 4:2:0/4:2:2, Cb/Cr are subsampled.
 * Returns number of planes written (1 for mono, 3 for color). */
int uvc_rgb_to_ycbcr(const uint8_t *r, const uint8_t *g, const uint8_t *b,
                     int w, int h, int chroma_fmt,
                     int16_t *y, int16_t *cb, int16_t *cr);

/* Inverse: YCbCr planes -> RGB. Input planes as produced by
 * uvc_rgb_to_ycbcr. Chroma is upsampled to match Y dimensions. */
int uvc_ycbcr_to_rgb(const int16_t *y, const int16_t *cb, const int16_t *cr,
                     int w, int h, int chroma_fmt,
                     uint8_t *r, uint8_t *g, uint8_t *b);

/* Get plane dimensions for a given chroma format and luma dimensions. */
static inline int uvc_plane_w(int w, int chroma_fmt, int plane) {
    if (chroma_fmt == UVC_CHROMA_420 || chroma_fmt == UVC_CHROMA_422) {
        return (plane == 0) ? w : (w + 1) / 2;
    }
    return w;
}
static inline int uvc_plane_h(int h, int chroma_fmt, int plane) {
    if (chroma_fmt == UVC_CHROMA_420) {
        return (plane == 0) ? h : (h + 1) / 2;
    }
    return h;
}

/* Allocate all plane buffers for a frame. Returns 0 on success, -1 on OOM.
 * Planes must be freed with uvc_free_planes(). */
int uvc_alloc_planes(int w, int h, int chroma_fmt, int16_t **y, int16_t **cb, int16_t **cr);
void uvc_free_planes(int16_t *y, int16_t *cb, int16_t *cr);

/* Pack three planes into a plane-interleaved frame array suitable for
 * uvc_mux_chroma. frames_out must have nplanes entries. */
static inline void uvc_pack_planes(const int16_t *y, const int16_t *cb, const int16_t *cr,
                                   int nplanes, const int16_t **frames_out) {
    frames_out[0] = y;
    if (nplanes > 1) { frames_out[1] = cb; frames_out[2] = cr; }
}

/* Encode a chroma frame using P1 per plane. plane_frames/out must have
 * nplanes entries. Returns total bytes, or -1 on error. */
int uvc_p1_encode_chroma_frame(const int16_t *const *plane_frames,
                               int nplanes, int w, int h, int chroma_fmt,
                               uint16_t scale_fp, uint8_t *out, int cap);

/* Decode a chroma frame using P1 per plane. plane_frames/out must have
 * nplanes entries. Returns 0 on success, -1 on error. */
int uvc_p1_decode_chroma_frame(const uint8_t *const *plane_frames,
                               const int *plane_lens, int nplanes,
                               int w, int h, int chroma_fmt,
                               uint16_t scale_fp, int16_t *out_y,
                               int16_t *out_cb, int16_t *out_cr);

#endif /* UVC_CHROMA_H */