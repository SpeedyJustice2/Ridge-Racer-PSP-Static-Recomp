# ATRAC3+ decoder (from FFmpeg)

`atrac.c/.h`, `atrac3plus.c/.h`, `atrac3plus_data.h`, `atrac3plusdsp.c` are FFmpeg's ATRAC3+ decoder files, copied unchanged from
FFmpeg commit `c2bbf2b83d064313ca3739f81c651553cfbb26fb` (libavcodec/). Copyright (c) 2010-2013 Maxim Poliakovski and others,
licensed under the GNU Lesser General Public License, version 2.1 or later (see `COPYING.LGPLv2.1`).

`atrac3plusdec.c` is FFmpeg's `atrac3plusdec.c` with the AVCodec registration removed and a small public API
(`rr_atrac3p.h`) added at the end; the decoding steps are unchanged.

`rr_compat.c` and the `compat/` headers are new code for this project: tiny stand-ins for the FFmpeg internals the decoder
needs (bit reader, VLC table builder, float vector helpers, inverse MDCT, sine windows). They are also LGPL-2.1-or-later.

FFmpeg itself (https://ffmpeg.org) is not included and not needed. Replace or modify these files freely under the LGPL;
the port links this code as a separate static library from source, so rebuilding with your own version is straightforward.

Checked against FFmpeg 6.1.1-3ubuntu5's decoder on a real ATRAC3+ file: output differs by float rounding only (largest difference 4e-7).
