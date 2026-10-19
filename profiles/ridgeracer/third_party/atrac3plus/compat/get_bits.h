#pragma once
#include <stdint.h>
#include "vlc.h"
/* Big-endian bit reader with the subset of FFmpeg's get_bits API used by the ATRAC3+ decoder.  Reads past the end return zeros. */
typedef struct GetBitContext { const uint8_t *buffer; int size_in_bits; int index; } GetBitContext;
static inline int init_get_bits8(GetBitContext *s, const uint8_t *buf, int bytes) {
    if (bytes < 0 || !buf) return -1;
    s->buffer = buf; s->size_in_bits = bytes * 8; s->index = 0; return 0;
}
static inline int get_bits_count(const GetBitContext *s) { return s->index; }
static inline int get_bits_left(const GetBitContext *s) { return s->size_in_bits - s->index; }
static inline unsigned show_bits(const GetBitContext *s, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        const int p = s->index + i;
        const unsigned bit = p < s->size_in_bits ? (s->buffer[p >> 3] >> (7 - (p & 7))) & 1u : 0u;
        v = (v << 1) | bit;
    }
    return v;
}
static inline void skip_bits(GetBitContext *s, int n) { s->index += n; }
static inline unsigned get_bits(GetBitContext *s, int n) { const unsigned v = n ? show_bits(s, n) : 0u; s->index += n; return v; }
static inline unsigned get_bits1(GetBitContext *s) { return get_bits(s, 1); }
static inline unsigned get_bitsz(GetBitContext *s, int n) { return n ? get_bits(s, n) : 0u; }
static inline int get_vlc2(GetBitContext *s, const VLCElem *table, int bits, int max_depth) {
    (void)max_depth;
    const VLCElem e = table[show_bits(s, bits)];
    s->index += e.len;
    return e.sym;
}
