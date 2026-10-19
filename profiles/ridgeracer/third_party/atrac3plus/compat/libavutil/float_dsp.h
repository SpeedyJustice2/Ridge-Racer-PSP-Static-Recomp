#pragma once
/* The few float vector helpers the ATRAC3+ decoder uses (plain C, same maths as FFmpeg's C versions). */
typedef struct AVFloatDSPContext {
    void (*vector_fmul)(float *dst, const float *src0, const float *src1, int len);
    void (*vector_fmac_scalar)(float *dst, const float *src, float mul, int len);
    void (*vector_fmul_scalar)(float *dst, const float *src, float mul, int len);
    void (*vector_fmul_reverse)(float *dst, const float *src0, const float *src1, int len);
} AVFloatDSPContext;
AVFloatDSPContext *avpriv_float_dsp_alloc(int bit_exact);
