#pragma once
#include <atomic>

namespace rr {

// Live-tunable rendering knobs (changed with hotkeys, shown in the window title).
// Blended draws are alpha-tested with "alpha > 0", so texels with 1-3 % alpha (shadows, haze, tree fringes) pass and show as
// faint dark rectangles at high resolution. Blended fragments below this alpha (0-255) are dropped.
inline std::atomic<int> g_alpha_cutoff{0};

// Diagnostic (F4 cycles): hide one class of draw so a screenshot/eye can tell which class contains an artefact.
//  0 off, 1 blended+alpha-tested, 2 blended only, 3 alpha-tested only, 4 plain textured, 5 screen-space (UI) draws
inline std::atomic<int> g_hide_class{0};

// Audio diagnostics (Insert / Delete keys): Insert writes an AUDMARK line with every sounding voice to the log; Delete silences
// all SAS voices (a stuck sound that stops was a SAS voice; one that does not is something else).
inline std::atomic<int> g_audio_mark{0};
inline std::atomic<int> g_audio_silence{0};

// Diagnostic (F2 next / F3 previous / F7 width 8<->1): hide screen-space draws number [pos*width, pos*width+width) of each
// frame; pos -1 = off.
inline std::atomic<int> g_thru_pos{-1};
inline std::atomic<int> g_thru_width{8};

// Frame-rate experiment (F2 next / F3 previous / F7 keep): gate candidate subsystems to every other logic step while the
// raised rate is active.  g_gate_sel: -1 off, 1..g_gate_count one candidate, g_gate_count+1 all race systems; g_gate_kept: bit i set =
// candidate i stays gated whatever is selected.  rr_hle.cpp applies changes once per frame and shows them in the title.
inline std::atomic<int> g_gate_sel{-1};
inline std::atomic<int> g_gate_count{0};
inline std::atomic<unsigned long long> g_gate_kept{0};

// Anti-aliasing mode applied when the screen is presented (F1 cycles): 0 none, 1 FXAA, 2 TAA, 3 SSAA downsample filter.
// MSAA is separate and fixed at start-up (--msaa N / --aa msaa); g_msaa_samples reports what the renderer really got.
inline std::atomic<int> g_aa_mode{0};
inline std::atomic<int> g_msaa_samples{0};
// Anisotropic filtering level for 3D textures: 1, 2, 4, 8 or 16 (End cycles; --aniso N sets the start value).
inline std::atomic<int> g_aniso{8};
inline std::atomic<int> g_taa_blend{20};   // TAA: share of the new frame in the result, in percent (lower = smoother, more ghosting)

} // namespace rr
