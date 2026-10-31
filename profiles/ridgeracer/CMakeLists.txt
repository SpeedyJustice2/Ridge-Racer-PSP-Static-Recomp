# Ridge Racer (PSP, 2004) profile - Stage 1: boot-only host.
#
# The generated C++ corpus is NOT in this repository.  You create it yourself
# from your own game with psp_recomp (see README.md) and point this profile at
# it with PSPRECOMP_RIDGERACER_GENERATED_DIR.

option(RR_SPLIT_GAME "Build the converted game code as a separate library (RidgeRacerGame)" OFF)
set(RIDGERACER_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(PSPRECOMP_RIDGERACER_GENERATED_DIR "${CMAKE_SOURCE_DIR}/../RidgeRacerPSPFiles/generated" CACHE PATH
    "Directory holding the generated Ridge Racer AOT corpus (from psp_recomp --auto)")
set(RIDGERACER_GENERATED_DIR "${PSPRECOMP_RIDGERACER_GENERATED_DIR}")

set(PSPRECOMP_RIDGERACER_MP_JOBS "4" CACHE STRING
    "Max concurrent cl.exe front-ends for the Ridge Racer target (lower this if you run out of RAM)")
if(NOT PSPRECOMP_RIDGERACER_MP_JOBS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PSPRECOMP_RIDGERACER_MP_JOBS must be a non-negative integer")
endif()

set(RIDGERACER_REGISTRY "${RIDGERACER_GENERATED_DIR}/generated_registry.cpp")
if(NOT EXISTS "${RIDGERACER_REGISTRY}")
    message(FATAL_ERROR
        "Ridge Racer generated corpus is missing: ${RIDGERACER_REGISTRY}\n"
        "Generate it first with psp_recomp (see profiles/ridgeracer/README.md), or set "
        "-DPSPRECOMP_RIDGERACER_GENERATED_DIR=<folder>.")
endif()

file(GLOB RIDGERACER_GENERATED CONFIGURE_DEPENDS "${RIDGERACER_GENERATED_DIR}/*.cpp")
list(LENGTH RIDGERACER_GENERATED RIDGERACER_GENERATED_COUNT)
if(RIDGERACER_GENERATED_COUNT LESS 2)
    message(FATAL_ERROR "Ridge Racer generated corpus is incomplete (${RIDGERACER_GENERATED_COUNT} sources)")
endif()
message(STATUS "Ridge Racer: ${RIDGERACER_GENERATED_COUNT} generated sources from ${RIDGERACER_GENERATED_DIR}")

# Same settings the MotorStorm profile uses for a corpus generated with
# --builtin-accessors: no inlining (/Ob0) and /bigobj.
if(MSVC)
    if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
        set(RR_OPT "/Od")
    elseif(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "3")
        set(RR_OPT "/Ox")
    else()
        set(RR_OPT "/O${PSPRECOMP_GENERATED_OPT_LEVEL}")
    endif()
    set(RR_GENERATED_OPTIONS "${RR_OPT};/Ob0;/bigobj")
    if(PSPRECOMP_LTO)
        string(APPEND RR_GENERATED_OPTIONS ";/GL-")
    endif()
    set_source_files_properties(${RIDGERACER_GENERATED} PROPERTIES COMPILE_OPTIONS "${RR_GENERATED_OPTIONS}")
    if(PSPRECOMP_RIDGERACER_MP_JOBS STREQUAL "0")
        set(RR_MP_FLAG "/MP")
    elseif(PSPRECOMP_RIDGERACER_MP_JOBS STREQUAL "1")
        set(RR_MP_FLAG "")
    else()
        set(RR_MP_FLAG "/MP${PSPRECOMP_RIDGERACER_MP_JOBS}")
    endif()
else()
    set_source_files_properties(${RIDGERACER_GENERATED} PROPERTIES COMPILE_OPTIONS
        "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0;-fno-strict-aliasing;-fwrapv;-ffp-contract=off")
endif()

if(WIN32)
    set(RR_PRESENT_SOURCE host/rr_present_win32.cpp)
    set(RR_VIDEO_SOURCE host/rr_video_win32.cpp)
    set(RR_LAUNCHER_SOURCE host/rr_launcher_win32.cpp)
else()
    # Linux (and other SDL2 platforms): SDL2 provides the window, OpenGL 3.3 context, input and audio.
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2PC QUIET IMPORTED_TARGET sdl2)
        endif()
    endif()
    if(TARGET SDL2::SDL2 OR TARGET PkgConfig::SDL2PC)
        set(RR_HAVE_SDL2 ON)
        set(RR_PRESENT_SOURCE host/rr_present_sdl.cpp)
    else()
        message(WARNING "SDL2 not found: building without a window or sound (install libsdl2-dev)")
        set(RR_PRESENT_SOURCE host/rr_present_null.cpp)
    endif()
    set(RR_VIDEO_SOURCE host/rr_video_null.cpp)
    set(RR_LAUNCHER_SOURCE host/rr_launcher_null.cpp)
endif()

add_executable(RidgeRacerNative
    host/main.cpp
    host/rr_hle.cpp
    host/rr_log.cpp
    host/rr_ge.cpp
    host/rr_raster.cpp
    host/rr_gl.cpp
    host/rr_png.cpp
    host/rr_atrac.cpp
    host/rr_mpeg.cpp
    host/rr_pmf.cpp
    host/rr_audio.cpp
    host/rr_atrac_decoder.cpp
    ${RR_VIDEO_SOURCE}
    host/rr_osk_ui.cpp
    host/rr_menusound.cpp
    host/rr_utility.cpp
    host/rr_settings.cpp
    host/rr_iso.cpp
    host/rr_net.cpp
    host/rr_http.cpp
    host/rr_relay.cpp
    host/rr_lobby_ui.cpp
    host/rr_pad.cpp
    host/rr_glyph_patch.cpp
    host/rr_report.cpp
    host/rr_update.cpp
    host/rr_gen_loader.cpp
    host/rr_sysinfo.cpp
    ${RR_LAUNCHER_SOURCE}
    ${RR_PRESENT_SOURCE}
    $<$<NOT:$<BOOL:${RR_SPLIT_GAME}>>:${RIDGERACER_GENERATED}>
)
if(RR_SPLIT_GAME)
    # Split build: the converted game code becomes a separate library that is built on the player's PC (see gen/rr_gen_abi.hpp).
    target_compile_definitions(RidgeRacerNative PRIVATE RR_GAME_LIBRARY=1)
    add_library(RidgeRacerGame SHARED gen/rr_gen_shim.cpp ${RIDGERACER_GENERATED})
    target_include_directories(RidgeRacerGame PRIVATE "${RIDGERACER_GENERATED_DIR}" "${RIDGERACER_PROFILE_DIR}/gen" "${CMAKE_SOURCE_DIR}/include")
    set_target_properties(RidgeRacerGame PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
                          RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:RidgeRacerNative>" LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:RidgeRacerNative>")
    if(NOT MSVC)
        target_compile_options(RidgeRacerGame PRIVATE -fno-strict-aliasing -fwrapv -ffp-contract=off)
        target_link_options(RidgeRacerGame PRIVATE -Wl,--no-undefined)
        target_link_libraries(RidgeRacerGame PRIVATE m)
    endif()
    add_dependencies(RidgeRacerNative RidgeRacerGame)
    if(NOT WIN32)
        target_link_libraries(RidgeRacerNative PRIVATE dl)
    endif()
endif()
if(NOT MSVC)
    target_compile_options(RidgeRacerNative PRIVATE -fno-strict-aliasing -fwrapv -ffp-contract=off)
endif()
target_include_directories(RidgeRacerNative PRIVATE "${RIDGERACER_GENERATED_DIR}" "${RIDGERACER_PROFILE_DIR}/host")
target_link_libraries(RidgeRacerNative PRIVATE psprecomp_core)

# Built-in ATRAC3+ decoder (FFmpeg's LGPL atrac3plus sources plus a small compatibility layer; no FFmpeg needed).
enable_language(C)
add_library(rr_atrac3p STATIC
    third_party/atrac3plus/atrac.c
    third_party/atrac3plus/atrac3plus.c
    third_party/atrac3plus/atrac3plusdsp.c
    third_party/atrac3plus/atrac3plusdec.c
    third_party/atrac3plus/rr_compat.c)
target_include_directories(rr_atrac3p PRIVATE third_party/atrac3plus/compat third_party/atrac3plus
                           INTERFACE third_party/atrac3plus)
set_target_properties(rr_atrac3p PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_link_libraries(rr_atrac3p PRIVATE m)
endif()
target_link_libraries(RidgeRacerNative PRIVATE rr_atrac3p)
if(NOT WIN32 AND RR_HAVE_SDL2)
    if(TARGET SDL2::SDL2)
        target_link_libraries(RidgeRacerNative PRIVATE SDL2::SDL2)
    else()
        target_link_libraries(RidgeRacerNative PRIVATE PkgConfig::SDL2PC)
    endif()
    target_compile_definitions(RidgeRacerNative PRIVATE RR_AUDIO_SDL=1)
    target_link_libraries(RidgeRacerNative PRIVATE ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    target_link_libraries(RidgeRacerNative PRIVATE user32 gdi32 d3d11 dxgi d3dcompiler opengl32 mfplat mfuuid ole32 winmm comctl32 shell32 uuid ws2_32 winhttp bcrypt)
endif()

# Optional, developer use only: FFmpeg (shared build) for plain ATRAC3 files and for the RR_ATRAC_COMPARE check.
# The game's music is ATRAC3+ and is decoded by the built-in decoder above.  -DFFMPEG_ROOT=<dir with include/ lib/ bin/>
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg shared build root (include/libavcodec, lib, bin)")
if(FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    find_library(RR_AVCODEC avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(RR_AVUTIL avutil PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    if(RR_AVCODEC AND RR_AVUTIL)
        target_include_directories(RidgeRacerNative PRIVATE "${FFMPEG_ROOT}/include")
        target_link_libraries(RidgeRacerNative PRIVATE "${RR_AVCODEC}" "${RR_AVUTIL}")
        target_compile_definitions(RidgeRacerNative PRIVATE RR_HAVE_FFMPEG=1)
        file(GLOB RR_FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
        add_custom_command(TARGET RidgeRacerNative POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RR_FFMPEG_DLLS} "$<TARGET_FILE_DIR:RidgeRacerNative>")
        message(STATUS "Ridge Racer: FFmpeg ATRAC decoding enabled (${FFMPEG_ROOT})")
    else()
        message(WARNING "FFMPEG_ROOT set but avcodec/avutil libraries not found in ${FFMPEG_ROOT}/lib")
    endif()
else()
    message(STATUS "Ridge Racer: FFmpeg not configured - ATRAC3+ music uses the built-in decoder")
endif()
psprecomp_enable_host_avx(RidgeRacerNative)
set_target_properties(RidgeRacerNative PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

if(MSVC)
    if(RR_MP_FLAG)
        target_compile_options(RidgeRacerNative PRIVATE ${RR_MP_FLAG})
    endif()
    target_link_options(RidgeRacerNative PRIVATE /STACK:67108864)
endif()
