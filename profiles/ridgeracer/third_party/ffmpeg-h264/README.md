# FFmpeg H.264 decoder (trimmed) - for the browser build's movies

A copy of the files from FFmpeg 7.1 (tag n7.1, https://github.com/FFmpeg/FFmpeg) that the H.264 *decoder* needs, nothing
else: no encoders, demuxers, filters, threads or assembly.  Licence: LGPL 2.1 or later (COPYING.LGPLv2.1); the files are
unmodified.

Used only by the browser build (host/rr_video_ffmpeg.cpp, built by build-web.ps1 / build-web.sh).  The Windows build
decodes movies with Media Foundation instead.

How it was made (Linux, Emscripten 3.1.74):

    ./configure --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --enable-cross-compile --target-os=none --arch=wasm32 \
      --cpu=generic --disable-everything --disable-programs --disable-doc --disable-network --disable-asm \
      --disable-inline-asm --disable-pthreads --disable-avdevice --disable-avformat --disable-swresample \
      --disable-swscale --disable-avfilter --disable-postproc --disable-debug --enable-decoder=h264 --enable-parser=h264
    make libavcodec/libavcodec.a libavutil/libavutil.a

then the object files a small test program actually linked (sources.txt) and every header their dependency files
named were copied here, together with the generated config.h, config_components.h, libavutil/avconfig.h,
libavcodec/codec_list.c and libavcodec/bsf_list.c.  The configuration is for wasm32 only (it disables threads, so the
decoder must only be used from one thread).

Check: decoding opening.pmf gives the same pixels as the system ffmpeg (all 3600 frames decode, ~1.6 ms per frame).

Build note: the ATRAC3+ decoder in third_party/atrac3plus also comes from FFmpeg and defines ff_vlc_init_from_lengths,
so these files are compiled with -Dff_vlc_init_from_lengths=ffh264_vlc_init_from_lengths (see build-web.ps1/.sh).
libavcodec/version.h is included for host code that uses the public API.
