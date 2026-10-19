#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
static inline int sign_extend(int val, unsigned bits) { const unsigned shift = 8 * sizeof(int) - bits; return (int)((unsigned)val << shift) >> shift; }
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FFSWAP(type, a, b) do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)
static inline int av_log2(unsigned v) { int n = 0; while (v >>= 1) n++; return n; }
static inline unsigned av_zero_extend(unsigned a, unsigned p) { return a & ((1u << p) - 1u); }
