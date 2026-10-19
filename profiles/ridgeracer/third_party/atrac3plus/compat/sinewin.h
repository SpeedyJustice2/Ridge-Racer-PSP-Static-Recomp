#pragma once
#include "libavutil/mem_internal.h"
void ff_init_ff_sine_windows(int index);
extern DECLARE_ALIGNED(32, float, ff_sine_64)[64];
extern DECLARE_ALIGNED(32, float, ff_sine_128)[128];
