#pragma once
/* ATRAC3+ decoder built from FFmpeg's libavcodec/atrac3plus*.c (LGPL-2.1-or-later), without the rest of FFmpeg. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RrAt3p RrAt3p;
/* channels 1..8, block_align = bytes per frame (from the WAV fmt chunk). */
RrAt3p *rr_at3p_open(int channels, int block_align);
void rr_at3p_close(RrAt3p *d);
int rr_at3p_channels(const RrAt3p *d);
/* Decodes one frame.  `out` points to `channels` buffers of 2048 floats (planar, range about -1..1).
   Returns 2048 on success, a negative value on error. */
int rr_at3p_decode(RrAt3p *d, const uint8_t *data, int size, float *const *out);
#ifdef __cplusplus
}
#endif
