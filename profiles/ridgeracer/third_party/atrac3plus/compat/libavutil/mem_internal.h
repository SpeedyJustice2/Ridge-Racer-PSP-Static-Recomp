#pragma once
#if defined(_MSC_VER)
#define DECLARE_ALIGNED(n, t, v) __declspec(align(n)) t v
#else
#define DECLARE_ALIGNED(n, t, v) t __attribute__((aligned(n))) v
#endif
#define LOCAL_ALIGNED(a, t, v, ...) DECLARE_ALIGNED(a, t, v) __VA_ARGS__
#define LOCAL_ALIGNED_32(t, v, ...) LOCAL_ALIGNED(32, t, v, __VA_ARGS__)
