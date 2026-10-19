#pragma once
#include <stdint.h>
#include "libavutil/log.h"
#include "libavutil/common.h"
#include "libavutil/attributes.h"
typedef struct AVChannelLayout { int nb_channels; } AVChannelLayout;
typedef struct AVCodecContext {
    AVChannelLayout ch_layout;
    int block_align;
    int sample_rate;
    int flags;
    void *priv_data;
} AVCodecContext;
#define AVERROR(e) (-(e))
#define AVERROR_INVALIDDATA (-1094995529)
#define AVERROR_PATCHWELCOME (-1163346256)
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
