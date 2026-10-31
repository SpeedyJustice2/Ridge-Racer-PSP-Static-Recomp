#pragma once

// Held-direction auto-repeat for the port's own pages (on-screen keyboard, server browser, RECOMP OPTIONS), timed by the clock
// rather than by frames: the pages are updated once per presented frame, so a frame count meant 330 ms at 60 fps but only
// 83 ms at 240 fps, and an ordinary tap of the D-pad moved the cursor twice.

#include <chrono>
#include <cstdint>

namespace rr::ui {

struct NavRepeat {
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kDelay{380}, kRate{110};
    bool held{false};
    Clock::time_point next{};
    // `pad` = buttons held now, `dirs` = the direction bits.  Returns the directions to repeat this frame (not the first press).
    std::uint32_t step(std::uint32_t pad, std::uint32_t dirs) {
        if ((pad & dirs) == 0u) { held = false; return 0u; }
        const auto now = Clock::now();
        if (!held) { held = true; next = now + kDelay; return 0u; }
        if (now < next) return 0u;
        next += kRate;
        if (next < now) next = now + kRate;
        return pad & dirs;
    }
};

// A page's clock in 60 Hz ticks since its first update (1, 2, 3, ... at 60 fps, the same pace at any frame rate): the pages'
// timers (messages, spinner, caret blink, "every 3 s") were written as frame counts.
struct Ticks60 {
    bool started{false};
    std::chrono::steady_clock::time_point t0{};
    std::uint32_t next() {
        const auto now = std::chrono::steady_clock::now();
        if (!started) { started = true; t0 = now; }
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() * 60 / 1000) + 1u;
    }
};

} // namespace rr::ui
