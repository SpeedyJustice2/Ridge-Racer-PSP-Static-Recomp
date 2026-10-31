#pragma once

// The port's own overlay over the game picture: a frame-rate / performance counter (Recomp Options > Display > Performance
// overlay) and short messages ("Volume 80%", "2 players are waiting to join").  Drawn by every renderer (GL: a blended
// texture before the swap; software: into the 480x272 picture), so nothing depends on the window title of one platform.

#include <cstdint>
#include <string>
#include <vector>

namespace rr::overlay {

enum Mode { kOff = 0, kFps = 1, kDetailed = 2 };
void set_mode(int mode);
[[nodiscard]] int mode();

// Once every half second: pictures per second, frame time average / worst in ms, and detail lines (renderer, AA, ...).
void set_stats(double fps, double avg_ms, double max_ms, const std::vector<std::string> &details);
// A message at the bottom of the picture for `seconds`.
void toast(const std::string &text, double seconds = 2.5);

enum Panel { kStats = 0, kToast = 1 };
// The panel's image at `scale` pixels per 480x272 unit (transparent outside the box), or false when there is nothing to show.
// `alpha` = how opaque to draw it.
bool build(int panel, float scale, std::vector<std::uint8_t> &rgba, int &w, int &h, float &alpha);
// Changes whenever build() would draw something different (rebuild only then).
[[nodiscard]] std::uint64_t key(int panel, float scale);
// Where a panel goes inside the game picture (pixels from the picture's top-left), for a picture `pw` x `ph`.
void place(int panel, int w, int h, int pw, int ph, int &x, int &y);
// Software renderer: blends the panels into a 480x272 RGBA picture.
void blend_into(std::uint8_t *rgba480x272);

} // namespace rr::overlay
