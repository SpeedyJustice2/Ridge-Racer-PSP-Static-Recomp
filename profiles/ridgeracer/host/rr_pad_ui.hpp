#pragma once

// Button pictures for the port's own pages (server browser, on-screen keyboard): the controller kind in use decides
// whether a PSP CROSS is shown as Xbox "A", PlayStation "X", Nintendo "B" (the same position) or the keyboard key.

#include <string>
#include <utility>
#include <vector>

#include "rr_pad.hpp"
#include "rr_ui_canvas.hpp"

namespace rr::ui {

enum class PadBtn { Cross, Circle, Triangle, Square, L, R, Start, Select, Dpad };

// Keyboard key for each PSP button (the default keys).
inline const char *keyboard_key(PadBtn b) {
    switch (b) {
    case PadBtn::Cross: return "Z";
    case PadBtn::Circle: return "X";
    case PadBtn::Triangle: return "S";
    case PadBtn::Square: return "A";
    case PadBtn::L: return "Q";
    case PadBtn::R: return "E";
    case PadBtn::Start: return "Enter";
    case PadBtn::Select: return "Bksp";
    default: return "Arrows";
    }
}

// Letter on the face button in that position (south = CROSS, east = CIRCLE, north = TRIANGLE, west = SQUARE).
inline const char *face_letter(PadBtn b, pad::Glyphs g) {
    if (g == pad::Glyphs::Nintendo) {
        switch (b) { case PadBtn::Cross: return "B"; case PadBtn::Circle: return "A"; case PadBtn::Triangle: return "X"; default: return "Y"; }
    }
    switch (b) { case PadBtn::Cross: return "A"; case PadBtn::Circle: return "B"; case PadBtn::Triangle: return "Y"; default: return "X"; }
}

inline const char *shoulder_text(PadBtn b, pad::Glyphs g) {
    const bool left = b == PadBtn::L;
    switch (g) {
    case pad::Glyphs::Xbox: return left ? "LB" : "RB";
    case pad::Glyphs::PlayStation: return left ? "L1" : "R1";
    default: return left ? "L" : "R";
    }
}

// Short name of a button as text ("A", "Cross", "LB", ...), for messages.
inline std::string button_name(PadBtn b, pad::Glyphs g) {
    if (g == pad::Glyphs::Keyboard) return keyboard_key(b);
    switch (b) {
    case PadBtn::Cross: case PadBtn::Circle: case PadBtn::Triangle: case PadBtn::Square:
        if (g == pad::Glyphs::PlayStation) return b == PadBtn::Cross ? "Cross" : b == PadBtn::Circle ? "Circle" : b == PadBtn::Triangle ? "Triangle" : "Square";
        return face_letter(b, g);
    case PadBtn::L: case PadBtn::R: return shoulder_text(b, g);
    case PadBtn::Start: return g == pad::Glyphs::Xbox ? "Menu" : g == pad::Glyphs::PlayStation ? "Options" : "+";
    case PadBtn::Select: return g == pad::Glyphs::Xbox ? "View" : g == pad::Glyphs::PlayStation ? "Share" : "-";
    default: return "D-pad";
    }
}

// Draws the picture with its left edge at x, centred vertically on cy; `size` = height in units.  Returns the width.
inline float pad_icon(const Canvas &cv, float x, float cy, PadBtn b, float size, pad::Glyphs g, const char *key_override = nullptr) {
    const Rgb white = rgb(255, 255, 255), dark = rgb(44, 44, 50);
    const float r = size * 0.5f;
    const auto keycap = [&](const char *t) {
        const float em = size * 0.72f;
        const float w = std::max(size, cv.text_width(std::string(t), em) + size * 0.55f);
        cv.solid(x, cy - r, w, size, size * 0.22f, rgb(70, 70, 76));
        cv.solid(x + 0.7f, cy - r + 0.7f, w - 1.4f, size - 1.9f, size * 0.18f, rgb(246, 246, 248));
        cv.text_centered(x + w * 0.5f, cy + em * 0.34f, t, em, rgb(40, 40, 46));
        return w;
    };
    const auto pill = [&](const char *t, Rgb bg) {
        const float em = size * 0.74f;
        const float w = std::max(size * 1.5f, cv.text_width(std::string(t), em) + size * 0.7f);
        cv.solid(x, cy - r, w, size, r, bg);
        cv.text_centered(x + w * 0.5f, cy + em * 0.36f, t, em, white);
        return w;
    };
    if (g == pad::Glyphs::Keyboard) return keycap(key_override != nullptr ? key_override : keyboard_key(b));
    const float cx = x + r;
    switch (b) {
    case PadBtn::Cross: case PadBtn::Circle: case PadBtn::Triangle: case PadBtn::Square: {
        if (g == pad::Glyphs::Xbox) {
            const Rgb col = b == PadBtn::Cross ? rgb(64, 168, 60) : b == PadBtn::Circle ? rgb(222, 52, 46) : b == PadBtn::Square ? rgb(44, 112, 218) : rgb(232, 172, 24);
            cv.circle(cx, cy, r, col);
            cv.text_centered(cx, cy + size * 0.29f, face_letter(b, g), size * 0.82f, white);
        } else if (g == pad::Glyphs::Nintendo) {
            cv.circle(cx, cy, r, dark);
            cv.text_centered(cx, cy + size * 0.29f, face_letter(b, g), size * 0.82f, white);
        } else {   // PlayStation shapes
            cv.circle(cx, cy, r, dark);
            const float k = r * 0.52f, t = std::max(0.9f, size * 0.11f);
            if (b == PadBtn::Cross) {
                const Rgb c = rgb(126, 176, 238);
                for (int s = -1; s <= 1; s += 2) {
                    const float dx = t * 0.5f * 0.7071f, dy = t * 0.5f * 0.7071f * s;
                    const float xs[4] = {cx - k - dx, cx - k + dx, cx + k + dx, cx + k - dx};
                    const float ys[4] = {cy - k * s + dy, cy - k * s - dy, cy + k * s - dy, cy + k * s + dy};
                    cv.polygon(xs, ys, 4, c);
                }
            } else if (b == PadBtn::Circle) {
                cv.ring(cx, cy, k * 0.95f, t, rgb(240, 96, 100));
            } else if (b == PadBtn::Triangle) {
                const Rgb c = rgb(70, 204, 160);
                const float px[3] = {cx, cx + k * 1.05f, cx - k * 1.05f}, py[3] = {cy - k * 1.05f, cy + k * 0.75f, cy + k * 0.75f};
                for (int i = 0; i < 3; ++i) {
                    const int j = (i + 1) % 3;
                    const float ex = px[j] - px[i], ey = py[j] - py[i], len = std::hypot(ex, ey), nx = -ey / len * t * 0.5f, ny = ex / len * t * 0.5f;
                    const float xs[4] = {px[i] + nx, px[j] + nx, px[j] - nx, px[i] - nx}, ys[4] = {py[i] + ny, py[j] + ny, py[j] - ny, py[i] - ny};
                    cv.polygon(xs, ys, 4, c);
                }
            } else {
                cv.frame(cx - k * 0.85f, cy - k * 0.85f, k * 1.7f, k * 1.7f, 0.3f, t, rgb(236, 120, 206));
            }
        }
        return size;
    }
    case PadBtn::L: case PadBtn::R: return pill(shoulder_text(b, g), dark);
    case PadBtn::Start: case PadBtn::Select: {
        if (g == pad::Glyphs::Nintendo) {
            cv.circle(cx, cy, r * 0.8f, dark);
            const float t = std::max(0.9f, size * 0.12f), k = r * 0.42f;
            cv.solid(cx - k, cy - t * 0.5f, k * 2.0f, t, 0.0f, white);
            if (b == PadBtn::Start) cv.solid(cx - t * 0.5f, cy - k, t, k * 2.0f, 0.0f, white);
            return size;
        }
        // Xbox: round Menu (three lines) and View (two windows) buttons; PlayStation: Options (three lines) and
        // Create (three rays) on a small capsule.
        const bool ps = g == pad::Glyphs::PlayStation;
        const float w = ps ? size * 1.35f : size;
        const float mx = x + w * 0.5f;
        if (ps) cv.solid(x, cy - r * 0.8f, w, size * 0.8f, r * 0.8f, dark); else cv.circle(cx, cy, r, dark);
        const float t = std::max(0.8f, size * 0.1f);
        if (b == PadBtn::Start) {
            for (int i = -1; i <= 1; ++i) cv.solid(mx - r * 0.42f, cy + i * size * 0.18f - t * 0.5f, r * 0.84f, t, 0.0f, white);
        } else if (!ps) {
            cv.frame(mx - r * 0.45f, cy - r * 0.38f, r * 0.62f, r * 0.55f, 0.2f, t, white);
            cv.solid(mx - r * 0.12f, cy - r * 0.12f, r * 0.62f, r * 0.55f, 0.2f, white);
        } else {
            for (int i = -1; i <= 1; ++i) {
                const float a = -1.5708f + i * 0.75f, ca = std::cos(a), sa = std::sin(a);
                const float x0 = mx + ca * r * 0.12f, y0 = cy + r * 0.32f + sa * r * 0.12f, x1 = mx + ca * r * 0.62f, y1 = cy + r * 0.32f + sa * r * 0.62f;
                const float nx = -sa * t * 0.5f, ny = ca * t * 0.5f;
                const float xs[4] = {x0 + nx, x1 + nx, x1 - nx, x0 - nx}, ys[4] = {y0 + ny, y1 + ny, y1 - ny, y0 - ny};
                cv.polygon(xs, ys, 4, white);
            }
        }
        return w;
    }
    default: {   // D-pad: a plus
        const float t = size * 0.34f;
        cv.solid(cx - r, cy - t * 0.5f, size, t, t * 0.2f, dark);
        cv.solid(cx - t * 0.5f, cy - r, t, size, t * 0.2f, dark);
        return size;
    }
    }
}

struct Hint {
    std::vector<PadBtn> buttons;
    std::string label;
    const char *key{};   // keyboard key to show instead of the default one (e.g. "Esc")
};

// A row of "picture label   picture label ..." starting at x, centred on cy.  Returns the end x.
inline float pad_hints(const Canvas &cv, float x, float cy, const std::vector<Hint> &items, float size, float em, Rgb text, pad::Glyphs g) {
    for (const auto &h : items) {
        for (std::size_t i = 0; i < h.buttons.size(); ++i) {
            if (g == pad::Glyphs::Keyboard && h.key != nullptr && i > 0) break;
            x += pad_icon(cv, x, cy, h.buttons[i], size, g, g == pad::Glyphs::Keyboard ? h.key : nullptr) + 1.5f;
        }
        x += 2.0f;
        cv.text(x, cy + em * 0.36f, h.label, em, text);
        x += cv.text_width(h.label, em) + 9.0f;
    }
    return x;
}

} // namespace rr::ui
