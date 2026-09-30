#ifndef NFSU2_MOVIE_VP6_H
#define NFSU2_MOVIE_VP6_H

#include <stdint.h>

/* 0 = the title's (lifted) decoder, 1 = FFmpeg (default), 2 = FFmpeg
 * checked against the lifted decoder. From NFSU2_NATIVE_VP6. */
int nfsu2_vp6_mode(void);

/* Decode one VP6 frame (the payload of an MV0K/MV0F chunk) into bottom-up
 * YUV 4:2:0 planes; y/u/v point at the picture's first stored row. `handle`
 * picks the decoder (one per title decoder). Returns 0, or -1 with the
 * planes untouched. */
int nfsu2_vp6_decode(uint32_t handle, const uint8_t *data, int size,
                     uint8_t *y, uint8_t *u, uint8_t *v,
                     int y_stride, int uv_stride, int width, int height);

#endif
