#pragma once
#include <stdlib.h>
#include <string.h>
static inline void *av_malloc(size_t n) { return malloc(n); }
static inline void *av_mallocz(size_t n) { return calloc(1, n ? n : 1); }
static inline void *av_calloc(size_t n, size_t s) { return calloc(n ? n : 1, s ? s : 1); }
static inline void av_free(void *p) { free(p); }
static inline void av_freep(void *pp) { void **p = (void **)pp; free(*p); *p = NULL; }
