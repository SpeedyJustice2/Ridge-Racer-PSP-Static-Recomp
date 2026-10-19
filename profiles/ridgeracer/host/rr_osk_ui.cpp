#include "rr_osk_ui.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace rr {
namespace {

#include "rr_osk_sdf.inc"

constexpr int kCols = 10;
constexpr int kCharRows = 7;
constexpr int kRows = kCharRows + 1;   // last row: Space / Delete / OK
const char16_t kChars[] = u"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz.-_'!?&/";
static_assert(sizeof(kChars) / sizeof(kChars[0]) - 1 == kCols * kCharRows, "grid must be exactly 7 rows of 10");

constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u,
                        kLeft = 0x0080u, kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u;

struct Ui {
    bool active{false};
    std::u16string desc, text;
    std::uint32_t limit{15u};
    int row{0}, col{0};
    std::uint32_t prev_pad{0u}, held_frames{0u}, frames{0u};
    std::uint32_t events{0u};   // OskEvent bits since the last osk_ui_take_events()
    bool armed{false};   // buttons must be released once so the press that opened the dialog is not taken as input
} g;

void add_char(char16_t c) {
    if (g.text.size() < g.limit) { g.text.push_back(c); g.events |= kOskInsert; }
}
void backspace() {
    if (!g.text.empty()) { g.text.pop_back(); g.events |= kOskDelete; }
}

OskState press_cell() {
    if (g.row < kCharRows) {
        add_char(kChars[g.row * kCols + g.col]);
        return OskState::Active;
    }
    if (g.col < 3) { add_char(u' '); return OskState::Active; }
    if (g.col < 6) { backspace(); return OskState::Active; }
    return OskState::Accepted;
}

// --- drawing (coordinates are in 480x272 units, multiplied by the scale; everything is anti-aliased) ---
struct Rgb { float r, g, b; };
constexpr Rgb rgb(int r, int g, int b) { return {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)}; }
Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

struct Canvas {
    std::uint8_t *px;
    int w, h;
    float s;   // pixels per unit
    void put(int xx, int yy, Rgb c, float a) const {
        if (a <= 0.0f || xx < 0 || yy < 0 || xx >= w || yy >= h) return;
        std::uint8_t *p = px + (static_cast<std::size_t>(yy) * w + xx) * 4u;
        p[0] = static_cast<std::uint8_t>(p[0] + (c.r - p[0]) * a + 0.5f);
        p[1] = static_cast<std::uint8_t>(p[1] + (c.g - p[1]) * a + 0.5f);
        p[2] = static_cast<std::uint8_t>(p[2] + (c.b - p[2]) * a + 0.5f);
        p[3] = 255;
    }
    // Rounded rectangle with a vertical colour ramp (top -> bottom) and an optional horizontal alpha fade.
    template <class Fill> void rrect(float x, float y, float rw, float rh, float rad, Fill fill) const {
        const int x0 = std::max(0, static_cast<int>((x - 1) * s)), x1 = std::min(w, static_cast<int>((x + rw + 1) * s) + 1);
        const int y0 = std::max(0, static_cast<int>((y - 1) * s)), y1 = std::min(h, static_cast<int>((y + rh + 1) * s) + 1);
        const float cx = x + rw * 0.5f, cy = y + rh * 0.5f, hx = rw * 0.5f - rad, hy = rh * 0.5f - rad;
        for (int yy = y0; yy < y1; ++yy) {
            const float uy = (yy + 0.5f) / s;
            for (int xx = x0; xx < x1; ++xx) {
                const float ux = (xx + 0.5f) / s;
                const float qx = std::fabs(ux - cx) - hx, qy = std::fabs(uy - cy) - hy;
                const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;   // signed distance, units
                const float cov = clamp01(0.5f - d * s);
                if (cov <= 0.0f) continue;
                float a = 1.0f;
                const Rgb c = fill((ux - x) / rw, (uy - y) / rh, a);
                put(xx, yy, c, cov * a);
            }
        }
    }
    void solid(float x, float y, float rw, float rh, float rad, Rgb c, float alpha = 1.0f) const {
        rrect(x, y, rw, rh, rad, [&](float, float, float &a) { a = alpha; return c; });
    }
    void vgrad(float x, float y, float rw, float rh, float rad, Rgb top, Rgb bottom) const {
        rrect(x, y, rw, rh, rad, [&](float, float v, float &) { return mix(top, bottom, v); });
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
    static unsigned glyph_index(char16_t ch) { return (ch >= 32 && ch <= 126) ? static_cast<unsigned>(ch - 32) : static_cast<unsigned>('?' - 32); }
    float advance(char16_t ch, float em) const {   // whole pixels at the native size so letters sit on the pixel grid
        const float a = kSdfAdvance[glyph_index(ch)] / 16.0f * (em / kSdfEm);
        return s < 2.0f ? std::max(1.0f, std::round(a)) : a;
    }
    float text_width(const std::u16string &t, float em) const {
        float wd = 0.0f;
        for (char16_t c : t) wd += advance(c, em);
        return wd;
    }
    float text_width(const char *t, float em) const {
        float wd = 0.0f;
        for (; *t; ++t) wd += advance(static_cast<unsigned char>(*t), em);
        return wd;
    }
    // One glyph from the signed-distance atlas; `pen_x` / `base_y` in units, `em` = font size in units.
    void glyph(float pen_x, float base_y, char16_t ch, float em, Rgb c, float alpha = 1.0f) const {
        const unsigned idx = glyph_index(ch);
        const float f = em / kSdfEm;   // units per atlas pixel
        const float left = pen_x - kSdfOriginX * f, top = base_y - kSdfBase * f;
        const int x0 = std::max(0, static_cast<int>(left * s)), x1 = std::min(w, static_cast<int>((left + kSdfCellW * f) * s) + 1);
        const int y0 = std::max(0, static_cast<int>(top * s)), y1 = std::min(h, static_cast<int>((top + kSdfCellH * f) * s) + 1);
        const unsigned char *atlas = kSdf[idx];
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
    void text(float x, float base_y, const char *t, float em, Rgb c, float alpha = 1.0f) const {
        x = std::round(x); base_y = std::round(base_y);
        for (; *t; ++t) { const char16_t ch = static_cast<unsigned char>(*t); glyph(x, base_y, ch, em, c, alpha); x += advance(ch, em); }
    }
    void text_centered(float cx, float base_y, const char *t, float em, Rgb c, float alpha = 1.0f) const { text(cx - text_width(t, em) * 0.5f, base_y, t, em, c, alpha); }
    void text_centered(float cx, float base_y, const std::u16string &t, float em, Rgb c, float alpha = 1.0f) const { text(cx - text_width(t, em) * 0.5f, base_y, t, em, c, alpha); }
};

} // namespace

void osk_ui_begin(const std::u16string &description, const std::u16string &initial, std::uint32_t limit) {
    g = Ui{};
    g.active = true;
    g.desc = description;
    g.limit = limit != 0u ? std::min<std::uint32_t>(limit, 60u) : 15u;
    g.text = initial.substr(0, g.limit);
}

bool osk_ui_active() { return g.active; }
std::uint32_t osk_ui_take_events() { const std::uint32_t e = g.events; g.events = 0u; return e; }
const std::u16string &osk_ui_text() { return g.text; }
void osk_ui_end() { g.active = false; }

OskState osk_ui_update(const std::vector<char16_t> &typed, std::uint32_t pad) {
    if (!g.active) return OskState::Cancelled;
    ++g.frames;
    // Typed characters.  Ignore the very first frames: the Enter that opened the dialog may still be in the queue.
    if (g.frames > 2u) {
        for (char16_t c : typed) {
            if (c == 13) { g.events |= kOskAccept; return OskState::Accepted; }
            if (c == 27) { g.events |= kOskCancel; return OskState::Cancelled; }
            if (c == 8) backspace();
            else if (c >= 32 && c != 127) add_char(c);
        }
    }
    // Gamepad.
    if (!g.armed) {
        if (pad == 0u) g.armed = true;
        g.prev_pad = pad;
        return OskState::Active;
    }
    const std::uint32_t pressed = pad & ~g.prev_pad;
    std::uint32_t nav = pressed;
    const std::uint32_t dirs = kUp | kRight | kDown | kLeft;
    if (pad & dirs) {
        ++g.held_frames;
        if (g.held_frames > 20u && (g.held_frames % 5u) == 0u) nav |= pad & dirs;   // auto-repeat
    } else {
        g.held_frames = 0u;
    }
    g.prev_pad = pad;
    if (nav & dirs) g.events |= kOskMove;
    if (g.row == kCharRows) {
        // Bottom row: three wide buttons (Space / Delete / OK), so one press moves from one button to the next.
        static const int centre[3] = {1, 4, 7};
        const int cur = g.col < 3 ? 0 : (g.col < 6 ? 1 : 2);
        if (nav & kLeft) g.col = centre[(cur + 2) % 3];
        if (nav & kRight) g.col = centre[(cur + 1) % 3];
    } else {
        if (nav & kLeft) g.col = (g.col + kCols - 1) % kCols;
        if (nav & kRight) g.col = (g.col + 1) % kCols;
    }
    if (nav & kUp) g.row = (g.row + kRows - 1) % kRows;
    if (nav & kDown) g.row = (g.row + 1) % kRows;
    if (pressed & kCross) { if (press_cell() == OskState::Accepted) { g.events |= kOskAccept; return OskState::Accepted; } }
    if (pressed & kCircle) backspace();
    if (pressed & kTriangle) add_char(u' ');
    if (pressed & kStart) { g.events |= kOskAccept; return OskState::Accepted; }
    if (pressed & kSelect) { g.events |= kOskCancel; return OskState::Cancelled; }
    return OskState::Active;
}

std::uint64_t osk_ui_state_key() {
    std::uint64_t k = 1469598103934665603ull;
    const auto mixin = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ull; };
    mixin(g.active ? 1u : 0u); mixin(static_cast<std::uint64_t>(g.row)); mixin(static_cast<std::uint64_t>(g.col));
    mixin((g.frames / 30u) % 2u); mixin(g.limit);
    for (char16_t c : g.text) mixin(c);
    mixin(0xFFFFu);
    for (char16_t c : g.desc) mixin(c);
    return k;
}

// Look: the game's own menus (Options screen): white page with a faint "+" pattern, grey fade at the top, thin sans text,
// dark grey capsules, red highlight, a grey info bar along the bottom.
static void render_native(std::uint8_t *rgba) {
    const Canvas cv{rgba, 480, 272, 1.0f};
    const Rgb ink = rgb(58, 58, 62), faint = rgb(206, 206, 210), red_top = rgb(236, 28, 36), red_bot = rgb(190, 0, 10), white = rgb(255, 255, 255);

    // Page: white, grey fade from the top and from the left edge, faint plus marks.
    cv.fill_all([&](float x, float y) {
        float v = 255.0f;
        v -= 52.0f * clamp01(1.0f - y / 62.0f) * clamp01(1.0f - y / 62.0f);   // top fade
        v -= 30.0f * clamp01(1.0f - x / 80.0f);                                // left fade
        const float mx = std::fmod(x + 7.0f, 30.0f), my = std::fmod(y + 11.0f, 30.0f);
        const bool plus = (std::fabs(mx - 15.0f) < 0.45f && std::fabs(my - 15.0f) < 2.2f) || (std::fabs(my - 15.0f) < 0.45f && std::fabs(mx - 15.0f) < 2.2f);
        if (plus) v -= 16.0f;
        return Rgb{v, v, v};
    });

    // Header: small title top-left, big faded title top-right (like "AUDIO" / "OPTIONS").
    cv.text(10.0f, 19.0f, "NAME ENTRY", 11.0f, ink);
    {
        std::u16string big;
        for (char16_t c : g.desc) big.push_back((c >= 'a' && c <= 'z') ? static_cast<char16_t>(c - 32) : c);
        if (big.empty() || big.size() > 16u) big = u"ENTER NAME";
        cv.text(470.0f - cv.text_width(big, 26.0f), 34.0f, big, 26.0f, faint);
    }

    // Text field: dark grey capsule, white text, red caret.
    const float fx = 90.0f, fy = 56.0f, fw = 300.0f, fh = 28.0f;
    cv.solid(fx - 1.0f, fy - 1.0f, fw + 2.0f, fh + 2.0f, fh * 0.5f + 1.0f, rgb(210, 210, 214));
    cv.vgrad(fx, fy, fw, fh, fh * 0.5f, rgb(96, 96, 100), rgb(52, 52, 56));
    cv.text(fx + 16.0f, fy + 19.5f, g.text, 16.0f, white);
    if ((g.frames / 30u) % 2u == 0u) {
        const float cx = fx + 16.0f + cv.text_width(g.text, 16.0f) + 1.0f;
        cv.solid(cx, fy + 6.0f, 2.0f, 16.0f, 1.0f, rgb(236, 28, 36));
    }
    {
        const std::string a = std::to_string(g.text.size()) + "/" + std::to_string(g.limit);
        cv.text(fx + fw - 14.0f - cv.text_width(a.c_str(), 10.0f), fy + 18.0f, a.c_str(), 10.0f, rgb(180, 180, 186));
    }

    // Character grid.
    const float cell_w = 32.0f, cell_h = 17.0f, gx = 80.0f, gy = 94.0f;
    const auto red_pill = [&](float x, float y, float rw, float rh) {
        cv.vgrad(x, y, rw, rh, rh * 0.5f, red_top, red_bot);
        cv.rrect(x + 1.5f, y + 0.8f, rw - 3.0f, rh * 0.45f, rh * 0.22f, [&](float, float, float &a) { a = 0.20f; return white; });   // gloss
    };
    for (int r = 0; r < kCharRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const bool sel = r == g.row && c == g.col;
            const float x = gx + c * cell_w, y = gy + r * cell_h;
            const char16_t ch = kChars[r * kCols + c];
            if (sel) red_pill(x + 2.0f, y + 0.5f, cell_w - 4.0f, cell_h - 1.0f);
            cv.text_centered(x + cell_w * 0.5f, y + 12.8f, std::u16string(1, ch), 13.0f, sel ? white : ink);
        }
    }
    // Space / Delete / OK.
    {
        static const char *labels[3] = {"SPACE", "DELETE", "OK"};
        static const int cols0[3] = {0, 3, 6}, cols1[3] = {3, 6, 10};
        const float y = gy + kCharRows * cell_h + 5.0f, hh = 22.0f;
        for (int i = 0; i < 3; ++i) {
            const bool sel = g.row == kCharRows && g.col >= cols0[i] && g.col < cols1[i];
            const float x = gx + cols0[i] * cell_w + 2.0f, wd = (cols1[i] - cols0[i]) * cell_w - 4.0f;
            if (sel) red_pill(x, y, wd, hh);
            else cv.vgrad(x, y, wd, hh, hh * 0.5f, rgb(102, 102, 106), rgb(60, 60, 64));
            cv.text_centered(x + wd * 0.5f, y + 15.0f, labels[i], 12.0f, white);
        }
    }

    // Info bar along the bottom.
    cv.rrect(0.0f, 240.0f, 480.0f, 40.0f, 0.0f, [&](float, float v, float &) { return mix(rgb(176, 176, 180), rgb(136, 136, 140), v); });
    cv.text(12.0f, 255.0f, "Keyboard: type your name, Enter = OK, Esc = Cancel", 11.0f, white);
    cv.text(12.0f, 268.0f, "Pad: D-pad = move, A = pick, B = delete, Start = OK", 11.0f, white);
}


// The page is drawn at the PSP's own 480x272 and enlarged the same way the game's menu text is (--ui-crisp): every texel
// stays a visible square pixel, but the step between neighbouring texels is a short smooth ramp instead of a hard edge.
void osk_ui_render(std::uint8_t *rgba, int w, int h) {
    if (!g.active || rgba == nullptr || w < 480 || h < 272) return;
    static std::vector<std::uint8_t> native(480u * 272u * 4u);
    render_native(native.data());
    const float k = std::max(1.0f, std::min(static_cast<float>(w) / 480.0f, static_cast<float>(h) / 272.0f));
    const float ramp = std::clamp(1.0f - 0.85f * 0.8f, 0.02f, 1.0f);   // same default as --ui-crisp 0.8
    const float r = std::max(ramp, 1.0f / k);                            // never narrower than one output pixel
    // Per-axis: for every output coordinate the two source texels and the blend weight between them.
    struct Tap { int i0, i1; float t; };
    const auto build = [&](int n, int src_n) {
        std::vector<Tap> taps(static_cast<std::size_t>(n));
        for (int o = 0; o < n; ++o) {
            const float q = (static_cast<float>(o) + 0.5f) / k - 0.5f;
            const float qi = std::floor(q);
            const float t = std::clamp((q - qi - 0.5f) / r + 0.5f, 0.0f, 1.0f);
            taps[static_cast<std::size_t>(o)] = {std::clamp(static_cast<int>(qi), 0, src_n - 1), std::clamp(static_cast<int>(qi) + 1, 0, src_n - 1), t};
        }
        return taps;
    };
    const std::vector<Tap> tx = build(w, 480), ty = build(h, 272);
    for (int y = 0; y < h; ++y) {
        const Tap &b = ty[static_cast<std::size_t>(y)];
        const std::uint8_t *row0 = native.data() + static_cast<std::size_t>(b.i0) * 480u * 4u, *row1 = native.data() + static_cast<std::size_t>(b.i1) * 480u * 4u;
        std::uint8_t *dst = rgba + static_cast<std::size_t>(y) * w * 4u;
        for (int x = 0; x < w; ++x, dst += 4) {
            const Tap &a = tx[static_cast<std::size_t>(x)];
            for (int c = 0; c < 3; ++c) {
                const float top = row0[a.i0 * 4 + c] + (row0[a.i1 * 4 + c] - row0[a.i0 * 4 + c]) * a.t;
                const float bot = row1[a.i0 * 4 + c] + (row1[a.i1 * 4 + c] - row1[a.i0 * 4 + c]) * a.t;
                dst[c] = static_cast<std::uint8_t>(top + (bot - top) * b.t + 0.5f);
            }
            dst[3] = 255;
        }
    }
}

} // namespace rr
