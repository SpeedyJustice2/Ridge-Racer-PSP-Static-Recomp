#!/usr/bin/env bash
# build-web.sh - Linux version of build-web.ps1: builds the browser version from already generated game code.
# Usage: ./build-web.sh GENERATED_DIR [OUT_DIR]      (env: EMCC=path to em++, JOBS=n, OPT=-O2, LINKOPT=-O1)
# Same flags as build-web.ps1.  Objects are cached in build-web/ (only changed sources recompile).
# The result contains the game's code translated to WebAssembly: for your own use only, never publish it.
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
gen=$(cd "${1:?generated dir}" && pwd)
dist=${2:-$root/web-dist}
build=$root/build-web
EMXX=${EMCC:-/opt/emtc/emscripten/em++}
EMC=${EMXX%++}cc; [ -x "$EMC" ] || EMC=$(dirname "$EMXX")/emcc
export EM_CONFIG=${EM_CONFIG:-/opt/emtc/.emscripten}
JOBS=${JOBS:-$(nproc)}
OPT=${OPT:--O2}
LINKOPT=${LINKOPT:--O1}
prof=$root/profiles/ridgeracer; hostdir=$prof/host; a3p=$prof/third_party/atrac3plus
common="$OPT -pthread -fwasm-exceptions -msimd128 -fno-strict-aliasing -fwrapv -ffp-contract=off -DNDEBUG -w"
cxx="-std=c++20 $common -I $root/include"
game="$cxx -I $hostdir -I $a3p -I $gen"
cflags="-std=c11 $common -I $a3p/compat -I $a3p"
mkdir -p "$build"/{obj-core,obj-host,obj-atrac,obj-gen} "$dist"
jobs_file=$build/jobs.txt; : > "$jobs_file"
add() {  # add SRC OBJDIR FLAGS COMPILER
    local src=$1 obj=$2/$(basename "${1%.*}").o
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "${REBUILD:-0}" = 1 ]; then printf '%s\t%s\t%s\t%s\n' "$4" "$3" "$src" "$obj" >> "$jobs_file"; fi
    echo "$obj" >> "$build/objects.lst"
}
: > "$build/objects.lst"
for f in "$root"/src/*.cpp; do add "$f" "$build/obj-core" "$cxx" "$EMXX"; done
for n in main rr_hle rr_log rr_ge rr_raster rr_gl rr_png rr_atrac rr_mpeg rr_pmf rr_audio rr_atrac_decoder rr_osk_ui \
         rr_lobby_ui rr_pad rr_glyph_patch rr_report rr_menusound rr_utility rr_settings rr_iso rr_net rr_http rr_relay rr_sysinfo rr_launcher_null rr_present_web; do
    add "$hostdir/$n.cpp" "$build/obj-host" "$game" "$EMXX"; done
# Movies: FFmpeg's H.264 decoder (third_party/ffmpeg-h264, LGPL) behind host/rr_video_ffmpeg.cpp
ff=$prof/third_party/ffmpeg-h264
add "$hostdir/rr_video_ffmpeg.cpp" "$build/obj-host" "$game -I $ff" "$EMXX"
ffc="-std=c17 -O2 -pthread -msimd128 -w -fno-math-errno -fno-signed-zeros -D_ISOC11_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_POSIX_C_SOURCE=200112 -D_XOPEN_SOURCE=600 -DHAVE_AV_CONFIG_H -Dff_vlc_init_from_lengths=ffh264_vlc_init_from_lengths -I $ff"
mkdir -p "$build/obj-ff-libavcodec" "$build/obj-ff-libavutil"
while read -r src; do
    [ -n "$src" ] || continue
    lib=${src%%/*}
    add "$ff/$src" "$build/obj-ff-$lib" "$ffc -DBUILDING_${lib#lib}" "$EMC"
done < "$ff/sources.txt"
for n in atrac atrac3plus atrac3plusdsp atrac3plusdec rr_compat; do add "$a3p/$n.c" "$build/obj-atrac" "$cflags" "$EMC"; done
for f in "$gen"/*.cpp; do add "$f" "$build/obj-gen" "$game" "$EMXX"; done
n=$(wc -l < "$jobs_file"); echo "== compiling $n file(s) with $JOBS jobs"
printf '#include <thread>\n#include <mutex>\nint main(){return 0;}\n' > "$build/warm.cpp"
"$EMXX" $cxx -c "$build/warm.cpp" -o "$build/warm.o"
fail=0
if [ "$n" -gt 0 ]; then
    tr '\t' '\037' < "$jobs_file" | xargs -P "$JOBS" -d '\n' -I{} bash -c '
        IFS=$'"'"'\037'"'"' read -r c fl src obj <<< "{}"
        if ! $c $fl -c "$src" -o "$obj" 2> "$obj.err"; then echo "FAILED: $src"; head -20 "$obj.err"; exit 1; fi
        rm -f "$obj.err"' || fail=1
fi
[ $fail = 0 ] || { echo "ERROR: compile failures"; exit 1; }
echo "== linking"
sed 's/.*/"&"/' "$build/objects.lst" > "$build/objects.rsp"
"$EMXX" @"$build/objects.rsp" $LINKOPT -pthread -fwasm-exceptions -o "$dist/RidgeRacer.js" \
  -sWASM_BIGINT=1 -sMODULARIZE=1 -sEXPORT_NAME=RRModule -sENVIRONMENT=worker \
  -sINITIAL_MEMORY=536870912 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 -sSTACK_SIZE=67108864 \
  -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1 -sGL_ENABLE_GET_PROC_ADDRESS=1 \
  --profiling-funcs -sFILESYSTEM=1 -lworkerfs.js -sEXPORTED_RUNTIME_METHODS=FS,GL,UTF8ToString,ENV -sEXIT_RUNTIME=1 -sJSPI=1 -sASSERTIONS=${ASSERTIONS:-0} ${LINKEXTRA:-}
cp "$root"/web/* "$dist"/
ls -la "$dist"
echo "== done: $dist"
