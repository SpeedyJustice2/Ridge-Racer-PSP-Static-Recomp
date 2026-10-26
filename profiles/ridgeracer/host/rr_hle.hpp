#pragma once

// Ridge Racer (PSP) user-mode HLE: cooperative PSP scheduler, guest file I/O
// against disc0:/, memory partitions, UMD/timing services.  Derived from the
// kernel layer of the MotorStorm profile with all game-specific code removed.
// Anything not implemented stops execution with a loud [HLE MISSING] report.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace psprecomp {
class Runtime;
}

namespace rr {

struct HleOptions {
    // Directory mounted as disc0:/ (the folder that contains PSP_GAME).
    std::filesystem::path disc_root;
    bool trace_imports{true};
    bool trace_filesystem{true};
    // Per-import trace budget before that import goes quiet (0 = unlimited).
    std::uint64_t import_trace_limit{4};
    // Output window (Windows builds) and frame capture.
    bool window{true};
    int window_scale{3};
    int draw_cells{0};          // draw distance: also draw what is visible from this many cells before and after the current one
    bool throttle{true};
    // GPU (OpenGL) renderer: draws the screen at internal_scale x the PSP resolution.
    bool gpu{false};
    int internal_scale{3};
    // Visual enhancements of the GPU renderer.
    bool low_latency{false};   // wait for the GPU after presenting each frame (no queued frames)
    bool latency_log{false};   // log input-to-screen timing
    int aa_mode{0};   // 0 none, 1 FXAA, 2 TAA, 3 SSAA filter (see rr_tuning.hpp g_aa_mode)
    int msaa{0};      // MSAA sample count (0 = off)
    float sharpen{0.0f};
    bool mipmaps{true};
    int aniso{8};
    double fps_mult{1.0};
    int fps_patch{0};
    bool fps_race_only{true};
    bool menu_native{false};   // with fps_mult: menus (the UI engine) run at the raised rate too, with the UI time step scaled and its animations blended between ticks
    bool menu_interp{false};   // with fps_mult outside races: show in-between pictures of the 59.94 Hz menus at the raised rate (GPU renderer)
    std::vector<std::uint32_t> fps_gates;   // game functions to run every other step at the raised rate (see rr_hle.cpp gates)   // with fps_mult: raise the rate only while a race is running (menus stay at 59.94 Hz)
    bool trace_tools{false};   // Delete key: car write trace (slows every guest store a little)       // with fps_mult: rescale the game's 1/60 and 60 constants (1), also its 30-based ones (2)   // experiment: raise the display's vblank rate to this multiple of 59.94 Hz
    int frame_skip{1};   // 1 = every frame drawn; N>1 = during races only every Nth frame is drawn and shown (the game logic still runs every frame); 0 = automatic: pictures are skipped only while the game is behind real time
    bool fullscreen{false};
    int alpha_cutoff{0};     // see rr_tuning.hpp
    float volume{1.0f};      // master volume (1 = the game's own level)
    float menu_volume{1.0f}; // keyboard / menu click volume
    bool smooth_cutouts{false};   // bilinear filtering for alpha-tested cut-outs such as trees
    float ui_crisp{0.8f};   // crisp magnification of text and icons (0 = off, 1 = crispest)
    std::filesystem::path shot_dir{"screenshots"};   // where F12 screenshots go
    int shot_mode{0};                                    // F12: 0 = the picture as shown (filters included), 1 = native 480x272, 2 = both, 3 = raw internal-resolution render without filters
    std::filesystem::path screenshot_dir;
    std::uint64_t screenshot_every{0};   // save every Nth presented frame
    std::uint64_t screenshot_max{50};
    // Deterministic test runs: replay button presses recorded in a log ("PAD buttons ... (read #N, frame F)")
    // and stop after a frame count.
    std::filesystem::path replay_input;
    std::uint64_t stop_after_frame{0};
};

void install_hle(psprecomp::Runtime &runtime, std::uint32_t user_arena_start, const HleOptions &options);
void register_thread_return_target(psprecomp::Runtime &runtime);
// End-of-run census: import histogram, file I/O counters, scheduler state.
void report_summary();
// Where the performance report is written (empty = no report).  See --perf-report.
void set_perf_report(const std::filesystem::path &file);
[[nodiscard]] std::uint64_t guest_time_us();

} // namespace rr
