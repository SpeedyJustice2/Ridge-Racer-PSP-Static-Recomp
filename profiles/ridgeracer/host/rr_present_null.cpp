#include "rr_present.hpp"

namespace rr {
bool present_open(const PresentOptions &) { return false; }
void present_frame(const std::uint8_t *, int, int) {}
void present_pump() {}
bool present_quit_requested() { return false; }
std::uint32_t present_pad_buttons() { return 0u; }
void present_pad_analog(std::uint8_t &lx, std::uint8_t &ly) { lx = 128; ly = 128; }
void present_take_typed(std::vector<char16_t> &out) { out.clear(); }
std::uint32_t present_gamepad_buttons() { return 0u; }
void present_close() {}
GlProc present_gl_open() { return nullptr; }
void present_gl_size(int &w, int &h) { w = 480; h = 272; }
void present_gl_swap() {}
void present_take_timing(double &swap_ms, double &pace_ms) { swap_ms = pace_ms = 0.0; }
void present_note_pad_read() {}
void present_take_latency(PresentLatency &out) { out = PresentLatency{}; }
void present_set_title(const char *) {}
bool present_screenshot_requested() { return false; }
bool present_ramdump_requested() { return false; }
void present_set_frame_period_us(int) {}
bool present_difftrace_requested() { return false; }
bool present_callprofile_requested() { return false; }
bool present_cartrace_requested() { return false; }
} // namespace rr
