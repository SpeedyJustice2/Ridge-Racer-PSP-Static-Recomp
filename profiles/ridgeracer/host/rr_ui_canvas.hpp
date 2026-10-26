#pragma once

// Drawing helpers shared by the port's own menu pages (on-screen keyboard, server browser): anti-aliased rounded
// rectangles, circles, polygons and signed-distance text, drawn into an RGBA image.  Coordinates are in 480x272 "units"
// multiplied by a scale, so a page designed for the PSP screen can be drawn at any size.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace rr::ui {

// Glyph atlas (rr_osk_sdf.inc, defined in rr_osk_ui.cpp): ASCII 32..126.
constexpr int kSdfCellW = 32, kSdfCellH = 36, kSdfEm = 24, kSdfBase = 28, kSdfOriginX = 4;
constexpr float kSdfSpread = 4.0f;
const unsigned char *sdf_glyph(unsigned index);     // kSdfCellW * kSdfCellH distances (128 = edge)
unsigned sdf_advance(unsigned index);               // advance in 1/16 atlas pixels

struct Rgb { float r, g, b; };
constexpr Rgb rgb(int r, int g, int b) { return {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)}; }
inline Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

struct Canvas {
    std::uint8_t *px;
    int w, h;
    float s;   // pixels per unit
    // Optional clip rectangle (units); everything outside it is left alone.
    float clip_x0{-1e9f}, clip_y0{-1e9f}, clip_x1{1e9f}, clip_y1{1e9f};

    void put(int xx, int yy, Rgb c, float a) const {
        if (a <= 0.0f || xx < 0 || yy < 0 || xx >= w || yy >= h) return;
        const float ux = (xx + 0.5f) / s, uy = (yy + 0.5f) / s;
        if (ux < clip_x0 || ux >= clip_x1 || uy < clip_y0 || uy >= clip_y1) return;
        if (a > 1.0f) a = 1.0f;
        std::uint8_t *p = px + (static_cast<std::size_t>(yy) * w + xx) * 4u;
        p[0] = static_cast<std::uint8_t>(p[0] + (c.r - p[0]) * a + 0.5f);
        p[1] = static_cast<std::uint8_t>(p[1] + (c.g - p[1]) * a + 0.5f);
        p[2] = static_cast<std::uint8_t>(p[2] + (c.b - p[2]) * a + 0.5f);
        p[3] = 255;
    }
    // Calls fn(ux, uy) -> signed distance in units (negative inside) for every pixel of the box, and blends `fill`.
    template <class Dist, class Fill> void shape(float x0, float y0, float x1, float y1, Dist dist, Fill fill) const {
        const int px0 = std::max(0, static_cast<int>((x0 - 1) * s)), px1 = std::min(w, static_cast<int>((x1 + 1) * s) + 1);
        const int py0 = std::max(0, static_cast<int>((y0 - 1) * s)), py1 = std::min(h, static_cast<int>((y1 + 1) * s) + 1);
        for (int yy = py0; yy < py1; ++yy) {
            const float uy = (yy + 0.5f) / s;
            for (int xx = px0; xx < px1; ++xx) {
                const float ux = (xx + 0.5f) / s;
                const float cov = clamp01(0.5f - dist(ux, uy) * s);
                if (cov <= 0.0f) continue;
                float a = 1.0f;
                const Rgb c = fill(ux, uy, a);
                put(xx, yy, c, cov * a);
            }
        }
    }
    // Rounded rectangle; fill(u, v, alpha&) gets the position inside the box (0..1).
    template <class Fill> void rrect(float x, float y, float rw, float rh, float rad, Fill fill) const {
        const float cx = x + rw * 0.5f, cy = y + rh * 0.5f, hx = rw * 0.5f - rad, hy = rh * 0.5f - rad;
        shape(x, y, x + rw, y + rh,
              [&](float ux, float uy) {
                  const float qx = std::fabs(ux - cx) - hx, qy = std::fabs(uy - cy) - hy;
                  const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                  return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
              },
              [&](float ux, float uy, float &a) { return fill((ux - x) / rw, (uy - y) / rh, a); });
    }
    void solid(float x, float y, float rw, float rh, float rad, Rgb c, float alpha = 1.0f) const {
        rrect(x, y, rw, rh, rad, [&](float, float, float &a) { a = alpha; return c; });
    }
    void vgrad(float x, float y, float rw, float rh, float rad, Rgb top, Rgb bottom, float alpha = 1.0f) const {
        rrect(x, y, rw, rh, rad, [&](float, float v, float &a) { a = alpha; return mix(top, bottom, v); });
    }
    // Outline of a rounded rectangle, `t` units thick.
    void frame(float x, float y, float rw, float rh, float rad, float t, Rgb c, float alpha = 1.0f) const {
        const float cx = x + rw * 0.5f, cy = y + rh * 0.5f, hx = rw * 0.5f - rad, hy = rh * 0.5f - rad;
        shape(x, y, x + rw, y + rh,
              [&](float ux, float uy) {
                  const float qx = std::fabs(ux - cx) - hx, qy = std::fabs(uy - cy) - hy;
                  const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                  const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
                  return std::fabs(d + t * 0.5f) - t * 0.5f;
              },
              [&](float, float, float &a) { a = alpha; return c; });
    }
    void circle(float cx, float cy, float r, Rgb c, float alpha = 1.0f) const {
        shape(cx - r, cy - r, cx + r, cy + r, [&](float ux, float uy) { return std::hypot(ux - cx, uy - cy) - r; },
              [&](float, float, float &a) { a = alpha; return c; });
    }
    // Ring (circle outline) of radius r and thickness t; only the part with uy < cut_y when cut_y is given (arcs).
    void ring(float cx, float cy, float r, float t, Rgb c, float alpha = 1.0f, float cut_y = 1e9f) const {
        shape(cx - r - t, cy - r - t, cx + r + t, cy + r + t,
              [&](float ux, float uy) {
                  const float d = std::fabs(std::hypot(ux - cx, uy - cy) - r) - t * 0.5f;
                  return uy > cut_y ? std::max(d, uy - cut_y) : d;
              },
              [&](float, float, float &a) { a = alpha; return c; });
    }
    // Filled convex or concave polygon (even-odd), `n` points.
    void polygon(const float *xs, const float *ys, int n, Rgb c, float alpha = 1.0f) const {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < n; ++i) { x0 = std::min(x0, xs[i]); x1 = std::max(x1, xs[i]); y0 = std::min(y0, ys[i]); y1 = std::max(y1, ys[i]); }
        shape(x0, y0, x1, y1,
              [&](float ux, float uy) {
                  float d = 1e9f;
                  bool inside = false;
                  for (int i = 0, j = n - 1; i < n; j = i++) {
                      const float ex = xs[i] - xs[j], ey = ys[i] - ys[j], wx = ux - xs[j], wy = uy - ys[j];
                      const float t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
                      d = std::min(d, std::hypot(wx - ex * t, wy - ey * t));
                      if (((ys[i] > uy) != (ys[j] > uy)) && (ux < (xs[j] - xs[i]) * (uy - ys[i]) / (ys[j] - ys[i]) + xs[i])) inside = !inside;
                  }
                  return inside ? -d : d;
              },
              [&](float, float, float &a) { a = alpha; return c; });
    }
    // Five-pointed star centred on (cx, cy) with outer radius r.
    void star(float cx, float cy, float r, Rgb c, float alpha = 1.0f) const {
        float xs[10], ys[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -1.5707963f + static_cast<float>(i) * 0.6283185f, rr = (i % 2 == 0) ? r : r * 0.45f;
            xs[i] = cx + std::cos(a) * rr; ys[i] = cy + std::sin(a) * rr;
        }
        polygon(xs, ys, 10, c, alpha);
    }
    // Plain fill of the whole canvas with a per-pixel function of (u, v) in units.
    template <class Fn> void fill_all(Fn fn) const {
        for (int yy = 0; yy < h; ++yy) {
            const float uy = (yy + 0.5f) / s;
            for (int xx = 0; xx < w; ++xx) {
                std::uint8_t *p = px + (static_cast<std::size_t>(yy) * w + xx) * 4u;
                const Rgb c = fn((xx + 0.5f) / s, uy);
                p[0] = static_cast<std::uint8_t>(c.r + 0.5f); p[1] = static_cast<std::uint8_t>(c.g + 0.5f);
                p[2] = static_cast<std::uint8_t>(c.b + 0.5f); p[3] = 255;
            }
        }
    }

    // ---- text ----
    static unsigned glyph_index(char16_t ch) { return (ch >= 32 && ch <= 126) ? static_cast<unsigned>(ch - 32) : static_cast<unsigned>('?' - 32); }
    float advance(char16_t ch, float em) const {   // whole pixels at the native size so letters sit on the pixel grid
        const float a = static_cast<float>(sdf_advance(glyph_index(ch))) / 16.0f * (em / kSdfEm);
        return s < 2.0f ? std::max(1.0f, std::round(a)) : a;
    }
    float text_width(const std::u16string &t, float em) const {
        float wd = 0.0f;
        for (char16_t c : t) wd += advance(c, em);
        return wd;
    }
    float text_width(const std::string &t, float em) const {
        float wd = 0.0f;
        for (unsigned char c : t) wd += advance(c, em);
        return wd;
    }
    // One glyph from the signed-distance atlas; `pen_x` / `base_y` in units, `em` = font size in units.
    void glyph(float pen_x, float base_y, char16_t ch, float em, Rgb c, float alpha = 1.0f) const {
        const unsigned idx = glyph_index(ch);
        const float f = em / kSdfEm;   // units per atlas pixel
        const float left = pen_x - kSdfOriginX * f, top = base_y - kSdfBase * f;
        const int x0 = std::max(0, static_cast<int>(left * s)), x1 = std::min(w, static_cast<int>((left + kSdfCellW * f) * s) + 1);
        const int y0 = std::max(0, static_cast<int>(top * s)), y1 = std::min(h, static_cast<int>((top + kSdfCellH * f) * s) + 1);
        const unsigned char *atlas = sdf_glyph(idx);
        const float scr = f * s;   // screen pixels per atlas pixel
        for (int yy = y0; yy < y1; ++yy) {
            const float ay = ((yy + 0.5f) / s - top) / f - 0.5f;
            const int iy = static_cast<int>(std::floor(ay));
            const float fy = ay - iy;
            if (iy < -1 || iy >= kSdfCellH) continue;
            for (int xx = x0; xx < x1; ++xx) {
                const float ax = ((xx + 0.5f) / s - left) / f - 0.5f;
                const int ix = static_cast<int>(std::floor(ax));
                const float fx = ax - ix;
                const auto at = [&](int cx, int cy) { return (cx < 0 || cy < 0 || cx >= kSdfCellW || cy >= kSdfCellH) ? 0.0f : static_cast<float>(atlas[cy * kSdfCellW + cx]); };
                const float v = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
                const float d_atlas = (v - 128.0f) / 127.0f * kSdfSpread;   // + inside
                const float cov = clamp01(0.5f + d_atlas * scr * (s < 2.0f ? 1.7f : 1.0f));   // tighter edge at 1x: crisper strokes
                put(xx, yy, c, cov * alpha);
            }
        }
    }
    void text(float x, float base_y, const std::u16string &t, float em, Rgb c, float alpha = 1.0f) const {
        x = std::round(x); base_y = std::round(base_y);
        for (char16_t ch : t) { glyph(x, base_y, ch, em, c, alpha); x += advance(ch, em); }
    }
    void text(float x, float base_y, const std::string &t, float em, Rgb c, float alpha = 1.0f) const {
        x = std::round(x); base_y = std::round(base_y);
        for (unsigned char ch : t) { glyph(x, base_y, ch, em, c, alpha); x += advance(ch, em); }
    }
    void text(float x, float base_y, const char *t, float em, Rgb c, float alpha = 1.0f) const { text(x, base_y, std::string(t), em, c, alpha); }
    void text_centered(float cx, float base_y, const std::string &t, float em, Rgb c, float alpha = 1.0f) const { text(cx - text_width(t, em) * 0.5f, base_y, t, em, c, alpha); }
    void text_centered(float cx, float base_y, const std::u16string &t, float em, Rgb c, float alpha = 1.0f) const { text(cx - text_width(t, em) * 0.5f, base_y, t, em, c, alpha); }
    void text_centered(float cx, float base_y, const char *t, float em, Rgb c, float alpha = 1.0f) const { text_centered(cx, base_y, std::string(t), em, c, alpha); }
    void text_right(float rx, float base_y, const std::string &t, float em, Rgb c, float alpha = 1.0f) const { text(rx - text_width(t, em), base_y, t, em, c, alpha); }
    // `t` shortened with "..." so it fits in `max_w` units.
    std::string fit(const std::string &t, float em, float max_w) const {
        if (text_width(t, em) <= max_w) return t;
        std::string out = t;
        while (!out.empty() && text_width(out + "...", em) > max_w) out.pop_back();
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out + "...";
    }
};

// The page is drawn at the PSP's own 480x272 and enlarged the same way the game's menu text is (--ui-crisp): every texel
// stays a visible square pixel, but the step between neighbouring texels is a short smooth ramp instead of a hard edge.
void upscale_crisp(const std::uint8_t *native480x272, std::uint8_t *rgba, int w, int h);

} // namespace rr::ui
