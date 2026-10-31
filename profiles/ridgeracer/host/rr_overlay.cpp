#include "rr_overlay.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "rr_ui_canvas.hpp"

namespace rr::overlay {
namespace {

using ui::Canvas;
using ui::Rgb;
using ui::rgb;

std::mutex g_mutex;
int g_mode = kOff;
double g_fps = 0.0, g_avg = 0.0, g_max = 0.0;
std::vector<std::string> g_details;
std::uint64_t g_stats_gen = 0;
std::string g_toast;
std::chrono::steady_clock::time_point g_toast_until{};
std::uint64_t g_toast_gen = 0;

using Clock = std::chrono::steady_clock;

constexpr float kLine = 12.0f, kPad = 5.0f, kEm = 9.0f, kBigEm = 11.0f;

std::vector<std::string> stats_lines() {
    std::vector<std::string> l;
    char b[96];
    std::snprintf(b, sizeof b, "%.0f FPS", g_fps);
    l.emplace_back(b);
    if (g_mode == kDetailed) {
        std::snprintf(b, sizeof b, "%.1f ms  (worst %.1f ms)", g_avg, g_max);
        l.emplace_back(b);
        for (const auto &d : g_details) l.push_back(d);
    }
    return l;
}

void draw_box(const Canvas &cv, float w, float h) { cv.solid(0.0f, 0.0f, w, h, 4.0f, rgb(18, 18, 22)); }

} // namespace

void set_mode(int m) { std::lock_guard<std::mutex> lock(g_mutex); g_mode = std::clamp(m, 0, 2); ++g_stats_gen; }
int mode() { std::lock_guard<std::mutex> lock(g_mutex); return g_mode; }

void set_stats(double fps, double avg_ms, double max_ms, const std::vector<std::string> &details) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fps = fps; g_avg = avg_ms; g_max = max_ms; g_details = details;
    ++g_stats_gen;
}

void toast(const std::string &text, double seconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_toast = text;
    g_toast_until = Clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    ++g_toast_gen;
}

std::uint64_t key(int panel, float scale) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::uint64_t s = static_cast<std::uint64_t>(scale * 100.0f);
    if (panel == kStats) return g_mode == kOff ? 0u : (g_stats_gen << 16) ^ s ^ static_cast<std::uint64_t>(g_mode) << 60;
    return Clock::now() < g_toast_until ? (g_toast_gen << 16) ^ s ^ (1ull << 62) : 0u;
}

bool build(int panel, float scale, std::vector<std::uint8_t> &rgba, int &w, int &h, float &alpha) {
    std::lock_guard<std::mutex> lock(g_mutex);
    scale = std::max(1.0f, scale);
    if (panel == kStats) {
        if (g_mode == kOff) return false;
        const std::vector<std::string> lines = stats_lines();
        const Canvas probe{nullptr, 0, 0, scale};
        float tw = probe.text_width(lines[0], kBigEm);
        for (std::size_t i = 1; i < lines.size(); ++i) tw = std::max(tw, probe.text_width(lines[i], kEm));
        const float uw = std::ceil(tw + kPad * 2.0f), uh = std::ceil(kPad * 2.0f + (kBigEm + 2.0f) + (lines.size() - 1) * kLine);
        w = static_cast<int>(std::ceil(uw * scale)); h = static_cast<int>(std::ceil(uh * scale));
        rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
        const Canvas cv{rgba.data(), w, h, scale};
        draw_box(cv, uw, uh);
        const Rgb fps_col = g_fps >= 58.0 ? rgb(120, 230, 120) : g_fps >= 45.0 ? rgb(240, 210, 90) : rgb(240, 110, 100);
        cv.text(kPad, kPad + kBigEm - 1.0f, lines[0], kBigEm, fps_col);
        for (std::size_t i = 1; i < lines.size(); ++i) cv.text(kPad, kPad + kBigEm + 2.0f + i * kLine - 3.0f, lines[i], kEm, rgb(230, 230, 236));
        alpha = 0.82f;
        return true;
    }
    if (Clock::now() >= g_toast_until || g_toast.empty()) return false;
    const Canvas probe{nullptr, 0, 0, scale};
    const float uw = std::ceil(probe.text_width(g_toast, 10.0f) + 20.0f), uh = 20.0f;
    w = static_cast<int>(std::ceil(uw * scale)); h = static_cast<int>(std::ceil(uh * scale));
    rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    const Canvas cv{rgba.data(), w, h, scale};
    cv.solid(0.0f, 0.0f, uw, uh, uh * 0.5f, rgb(18, 18, 22));
    cv.text(10.0f, 13.5f, g_toast, 10.0f, rgb(255, 255, 255));
    // fade out over the last 0.4 s
    const double left = std::chrono::duration<double>(g_toast_until - Clock::now()).count();
    alpha = static_cast<float>(0.9 * std::clamp(left / 0.4, 0.0, 1.0));
    return true;
}

void place(int panel, int w, int h, int pw, int ph, int &x, int &y) {
    const int margin = std::max(4, ph / 60);
    if (panel == kStats) { x = margin; y = margin; return; }
    x = (pw - w) / 2;
    y = ph - h - std::max(margin * 3, ph / 9);   // (above the game's own help bar)
}

void blend_into(std::uint8_t *dst) {
    if (dst == nullptr) return;
    for (int panel = 0; panel < 2; ++panel) {
        std::vector<std::uint8_t> img;
        int w = 0, h = 0;
        float a = 1.0f;
        if (!build(panel, 1.0f, img, w, h, a)) continue;
        int x = 0, y = 0;
        place(panel, w, h, 480, 272, x, y);
        for (int yy = 0; yy < h; ++yy) {
            const int ty = y + yy;
            if (ty < 0 || ty >= 272) continue;
            for (int xx = 0; xx < w; ++xx) {
                const int tx = x + xx;
                if (tx < 0 || tx >= 480) continue;
                const std::uint8_t *s = img.data() + (static_cast<std::size_t>(yy) * w + xx) * 4u;
                const float k = s[3] / 255.0f * a;
                if (k <= 0.0f) continue;
                std::uint8_t *d = dst + (static_cast<std::size_t>(ty) * 480 + tx) * 4u;
                for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(d[c] + (s[c] - d[c]) * k + 0.5f);
            }
        }
    }
}

} // namespace rr::overlay
