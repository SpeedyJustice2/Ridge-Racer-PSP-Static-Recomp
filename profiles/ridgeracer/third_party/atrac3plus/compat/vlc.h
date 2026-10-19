#pragma once
#include <stdint.h>
/* Flat (single level) VLC tables: one lookup of `bits` bits gives symbol and code length. */
typedef struct VLCElem { int16_t sym; int8_t len; } VLCElem;
typedef struct VLC { int bits; VLCElem *table; int table_size, table_allocated; } VLC;
#define VLC_INIT_USE_STATIC 1
int ff_vlc_init_from_lengths(VLC *vlc, int nb_bits, int nb_codes, const void *lens, int lens_wrap,
                             const void *symbols, int symbols_wrap, int symbols_size,
                             int offset, int flags, void *logctx);
