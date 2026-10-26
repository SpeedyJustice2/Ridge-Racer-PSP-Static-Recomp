#pragma once

// Output window, keyboard input and frame pacing.  The Win32 implementation
// opens a scalable window; the null implementation (non-Windows builds, tests)
// does nothing.

#include <cstdint>
#include <vector>

namespace rr {

struct PresentOptions {
    bool window{true};
    int scale{3};          // window size = scale * 480x272
    bool throttle{true};   // pace presentation to 59.94 Hz
    bool fullscreen{false};   // start in borderless full screen (F11 or Alt+Enter toggles it while running)
    int frame_period_us{16683};   // pacing period (one vblank)
    bool low_latency{false};  // wait for the GPU to finish each frame right after presenting it, so the driver never queues frames ahead
    bool latency_log{false};  // measure input-to-screen timing (see present_take_latency)
};

bool present_open(const PresentOptions &options);
// Changes the pacing period (one vblank) while running.
void present_set_frame_period_us(int period_us);
// Shows a 480x272 RGBA frame (and paces to real time when throttling).
void present_frame(const std::uint8_t *rgba, int width, int height);
// Processes window events; call regularly.
void present_pump();
[[nodiscard]] bool present_quit_requested();
// PSP button bits (SCE_CTRL_*) and analog stick (0..255, 128 = centre).
[[nodiscard]] std::uint32_t present_pad_buttons();
void present_pad_analog(std::uint8_t &lx, std::uint8_t &ly);
// Characters typed since the last call (UTF-16; 8 = backspace, 13 = Enter, 27 = Esc).  Used by the on-screen keyboard.
void present_take_typed(std::vector<char16_t> &out);
// Mouse since the last call (used by the server browser): pointer position in 480x272 picture units (the letterboxed
// picture, whatever the window size), left-button presses and wheel steps (+ = away from the user).
struct PresentMouse { bool inside{false}; float x{0.0f}, y{0.0f}; int clicks{0}; int wheel{0}; };
void present_take_mouse(PresentMouse &out);
// Window pixel position -> 480x272 picture units for a picture letterboxed into a win_w x win_h window.
inline bool present_map_pointer(int px, int py, int win_w, int win_h, float &x, float &y) {
    if (win_w <= 0 || win_h <= 0) return false;
    float vw = static_cast<float>(win_w), vh = vw * 272.0f / 480.0f;
    if (vh > static_cast<float>(win_h)) { vh = static_cast<float>(win_h); vw = vh * 480.0f / 272.0f; }
    const float ox = (static_cast<float>(win_w) - vw) * 0.5f, oy = (static_cast<float>(win_h) - vh) * 0.5f;
    x = (static_cast<float>(px) + 0.5f - ox) * 480.0f / vw;
    y = (static_cast<float>(py) + 0.5f - oy) * 272.0f / vh;
    return x >= 0.0f && y >= 0.0f && x < 480.0f && y < 272.0f;
}
// Gamepad-only PSP button bits (no keyboard), so typed letters are not mistaken for buttons.
[[nodiscard]] std::uint32_t present_gamepad_buttons();
// The kind of controller used last (rr::pad::Family as int, 0 = none connected) and whether its left stick is out of the
// dead zone: decides which button pictures are shown.
[[nodiscard]] int present_pad_family();
[[nodiscard]] bool present_pad_stick_active();
void present_close();

// OpenGL output (used by the GPU renderer).  present_gl_open() creates a GL 3.3 core context on the window and makes
// it current on the calling thread; it returns the function loader, or nullptr when unavailable (call it instead of
// presenting 480x272 frames).  present_gl_swap() swaps buffers and paces to real time when throttling.
using GlProc = void *(*)(const char *);
GlProc present_gl_open();
void present_gl_size(int &width, int &height);
void present_gl_swap();
// Frame skip: a frame that is not drawn is not shown either; this keeps the window responsive and the frame pacing going
// without a swap.  present_set_cpu_pacing(true) makes pacing run on the CPU clock (no vsync wait), needed while frames are skipped.
void present_skip_frame();
// Same without the pacing wait: used while the game is catching up with real time (automatic frame skip).
void present_skip_frame_fast();
void present_set_cpu_pacing(bool on);
// Milliseconds spent inside SwapBuffers and in frame-pacing sleeps since the last call (for the timing heartbeat).
void present_take_timing(double &swap_ms, double &pace_ms);
// Input latency measurement: call present_note_pad_read() whenever the game reads the controller. present_take_latency()
// returns, since the last call, averages and maxima of: time from the frame's start to the pad read, time from the pad read
// to the frame being handed to the display, and (sampled on some frames) how long the GPU still had to work after that.
struct PresentLatency { double wake_to_pad_avg{}, wake_to_pad_max{}, pad_to_swap_avg{}, pad_to_swap_max{}, gpu_backlog_avg{}, gpu_backlog_max{}; int frames{}, probes{}; };
void present_note_pad_read();
void present_take_latency(PresentLatency &out);
// Window title (used for the live fps readout).
void present_set_title(const char *title);
// True once each time F12 is pressed (screenshot request).
[[nodiscard]] bool present_screenshot_requested();
[[nodiscard]] bool present_bug_report_requested();   // Ctrl+F12 (F12 alone is a screenshot)
// True once each time Insert is pressed (RAM snapshot request, for finding game variables).
[[nodiscard]] bool present_ramdump_requested();
// True once each time Delete is pressed (car write trace, see rr_hle.cpp).
[[nodiscard]] bool present_cartrace_requested();
// True once each time Home is pressed (RAM diff trace, see rr_hle.cpp).
[[nodiscard]] bool present_difftrace_requested();
[[nodiscard]] bool present_callprofile_requested();

} // namespace rr
