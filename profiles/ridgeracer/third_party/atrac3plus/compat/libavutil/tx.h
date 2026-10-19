#pragma once
#include <stddef.h>
/* Inverse MDCT replacements for libavutil/tx (only the two uses of the ATRAC3+ decoder). */
typedef struct AVTXContext AVTXContext;
typedef void (*av_tx_fn)(AVTXContext *s, void *out, void *in, ptrdiff_t stride);
enum AVTXType { AV_TX_FLOAT_MDCT = 1 };
#define AV_TX_FULL_IMDCT (1ULL << 2)
int av_tx_init(AVTXContext **ctx, av_tx_fn *tx, enum AVTXType type, int inv, int len, const void *scale, unsigned long long flags);
void av_tx_uninit(AVTXContext **ctx);
