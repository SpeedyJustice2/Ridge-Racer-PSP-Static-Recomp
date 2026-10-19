/*
 * Small replacements for the FFmpeg internals the ATRAC3+ decoder sources use (bit-exact where FFmpeg is bit-exact,
 * float-rounding level differences in the IMDCT, which here is a direct O(N^2) evaluation).
 * Original work for this project; licensed LGPL-2.1-or-later like the FFmpeg files next to it.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/float_dsp.h"
#include "libavutil/tx.h"
#include "sinewin.h"
#include "vlc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- sine windows (FFmpeg: sinewin_tablegen.h) ---- */
DECLARE_ALIGNED(32, float, ff_sine_64)[64];
DECLARE_ALIGNED(32, float, ff_sine_128)[128];

static void sine_window_init(float *window, int n) {
    for (int i = 0; i < n; i++) window[i] = sinf((i + 0.5) * (M_PI / (2.0 * n)));
}

void ff_init_ff_sine_windows(int index) {
    if (index == 6) sine_window_init(ff_sine_64, 64);
    else if (index == 7) sine_window_init(ff_sine_128, 128);
}

/* ---- VLC tables (flat, canonical codes from lengths, as FFmpeg's ff_vlc_init_from_lengths assigns them) ---- */
int ff_vlc_init_from_lengths(VLC *vlc, int nb_bits, int nb_codes, const void *lens_v, int lens_wrap,
                             const void *symbols, int symbols_wrap, int symbols_size,
                             int offset, int flags, void *logctx) {
    (void)flags; (void)logctx;
    const int size = 1 << nb_bits;
    if (!vlc->table || vlc->table_allocated < size) return -1;
    for (int i = 0; i < size; i++) { vlc->table[i].sym = -1; vlc->table[i].len = 1; }
    const int8_t *lens = (const int8_t *)lens_v;
    uint64_t code = 0;
    for (int i = 0; i < nb_codes; i++, lens += lens_wrap) {
        int len = *lens;
        if (len > 0) {
            unsigned sym = (unsigned)i;
            if (symbols) {
                const uint8_t *p = (const uint8_t *)symbols + (size_t)i * symbols_wrap;
                sym = symbols_size == 1 ? *p : symbols_size == 2 ? *(const uint16_t *)p : *(const uint32_t *)p;
            }
            sym += (unsigned)offset;
            if (len > nb_bits || (code & ((1ull << (32 - len)) - 1))) return -1;
            const uint32_t first = (uint32_t)(code >> (32 - nb_bits));
            const uint32_t count = 1u << (nb_bits - len);
            for (uint32_t j = 0; j < count; j++) {
                vlc->table[first + j].sym = (int16_t)sym;
                vlc->table[first + j].len = (int8_t)len;
            }
        } else if (len < 0) {
            len = -len;
        } else {
            continue;
        }
        code += 1ull << (32 - len);
        if (code > 0xFFFFFFFFull + 1ull) return -1;
    }
    vlc->bits = nb_bits;
    vlc->table_size = size;
    return 0;
}

/* ---- float vector helpers ---- */
static void vector_fmul_c(float *dst, const float *a, const float *b, int len) { for (int i = 0; i < len; i++) dst[i] = a[i] * b[i]; }
static void vector_fmac_scalar_c(float *dst, const float *src, float mul, int len) { for (int i = 0; i < len; i++) dst[i] += src[i] * mul; }
static void vector_fmul_scalar_c(float *dst, const float *src, float mul, int len) { for (int i = 0; i < len; i++) dst[i] = src[i] * mul; }
static void vector_fmul_reverse_c(float *dst, const float *a, const float *b, int len) { b += len - 1; for (int i = 0; i < len; i++) dst[i] = a[i] * b[-i]; }

AVFloatDSPContext *avpriv_float_dsp_alloc(int bit_exact) {
    (void)bit_exact;
    AVFloatDSPContext *f = (AVFloatDSPContext *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->vector_fmul = vector_fmul_c;
    f->vector_fmac_scalar = vector_fmac_scalar_c;
    f->vector_fmul_scalar = vector_fmul_scalar_c;
    f->vector_fmul_reverse = vector_fmul_reverse_c;
    return f;
}

/* ---- inverse MDCT ----
 * Matches libavutil/tx (float, inverse): without AV_TX_FULL_IMDCT the output holds `len` values
 *   out[n] = -scale * sum_k in[k] * cos(pi/len * (n + len/2 + 0.5 + len/2) * (k + 0.5)),
 * with AV_TX_FULL_IMDCT 2*len values: that block sits at offset len/2 and the rest follows by symmetry. */
struct AVTXContext {
    int len, full;
    float scale;
    float *tab;   /* len x len: cos(pi/len * (n + len/2 + 0.5 + len/2) * (k + 0.5)) for n = 0..len-1 */
};

static void mdct_inv(AVTXContext *s, void *_out, void *_in, ptrdiff_t stride) {
    const int n = s->len;
    float *out = (float *)_out;
    const char *in = (const char *)_in;
    float tmp_in[1024];
    for (int k = 0; k < n; k++) tmp_in[k] = *(const float *)(in + (ptrdiff_t)k * stride);
    float *dst = s->full ? out + n / 2 : out;
    for (int i = 0; i < n; i++) {
        const float *row = s->tab + (size_t)i * n;
        float acc = 0.0f;
        for (int k = 0; k < n; k++) acc += tmp_in[k] * row[k];
        dst[i] = -s->scale * acc;
    }
    if (s->full) {
        const int len = n << 1, len2 = len >> 1, len4 = len >> 2;
        for (int i = 0; i < len4; i++) {
            out[i] = -out[len2 - i - 1];
            out[len - i - 1] = out[len2 + i];
        }
    }
}

/* In the inverse transform the stride is the spacing of the input values in bytes. */
int av_tx_init(AVTXContext **ctx, av_tx_fn *tx, enum AVTXType type, int inv, int len, const void *scale, unsigned long long flags) {
    if (type != AV_TX_FLOAT_MDCT || !inv || len <= 0 || len > 1024) return -22;
    AVTXContext *s = (AVTXContext *)calloc(1, sizeof(*s));
    if (!s) return -12;
    s->len = len;
    s->full = (flags & AV_TX_FULL_IMDCT) != 0;
    s->scale = scale ? *(const float *)scale : 1.0f;
    s->tab = (float *)malloc(sizeof(float) * (size_t)len * len);
    if (!s->tab) { free(s); return -12; }
    for (int i = 0; i < len; i++)
        for (int k = 0; k < len; k++)
            s->tab[(size_t)i * len + k] = (float)cos(M_PI / len * (i + len / 2.0 + 0.5 + len / 2.0) * (k + 0.5));
    *ctx = s;
    *tx = mdct_inv;
    return 0;
}

void av_tx_uninit(AVTXContext **ctx) {
    if (!*ctx) return;
    free((*ctx)->tab);
    free(*ctx);
    *ctx = NULL;
}
