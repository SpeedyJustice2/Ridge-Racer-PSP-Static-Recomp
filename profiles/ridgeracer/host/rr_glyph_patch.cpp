// In-game button pictures for the controller in use.
//
// The game shows PSP buttons in two places:
//  - its two fonts (help lines etc.) carry CROSS / CIRCLE / TRIANGLE / SQUARE symbols;
//  - the Options > Controller screen is one 320x208 picture (CLUT8) with L, R and the four symbols drawn into the red
//    labels around a PSP (the action names are drawn over it with the font).
// Both textures live in main RAM.  The first time a draw uses one of them, the symbol cells are rewritten in guest memory
// (the original cells are kept, so switching controllers or going back to PlayStation restores them).  Every renderer
// re-reads texture memory by content hash, so nothing else needs to know.
//
//  - fonts: the symbol cell gets a copy of the font's own letter cell (A/B/X/Y, or the keyboard key Z/X/S/A);
//  - controller picture: the letters are drawn from the port's signed-distance font in the picture's own palette.
//
// Textures are recognised by their size/format and a hash of the original CIRCLE cell, so it does not matter where in
// memory the game loads them.  RR_GLYPH_PATCH=0 turns this off; RR_GLYPH_LOG=1 logs what is patched.

#include "rr_glyph_patch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "psprecomp/guest_memory.hpp"
#include "rr_log.hpp"
#include "rr_pad.hpp"
#include "rr_ui_canvas.hpp"

namespace rr {
namespace {

using pad::Glyphs;

struct Rect { int x, y, w, h; };

enum class Kind { BigFont, SmallFont, Picture };

struct Shape {
    Kind kind;
    std::uint32_t format, width, height, stride;
    Rect signature;
    std::uint64_t hash;
};
const Shape kShapes[] = {
    {Kind::BigFont, 4u, 512u, 512u, 512u, {0, 168, 14, 14}, 0xFAD80441B0BC49D5ull},
    {Kind::SmallFont, 4u, 256u, 256u, 256u, {0, 110, 11, 11}, 0x2E82D2E8567834BFull},
    {Kind::Picture, 5u, 512u, 256u, 320u, {194, 157, 13, 13}, 0x7ABBBDEEA9092EFBull},
};

// ---- texel access (swizzled or linear, 4 or 8 bits) ----
struct Tex {
    std::uint8_t *p;
    std::uint32_t stride, bits;
    bool swz;
    std::size_t offset(int x, int y) const {
        const std::uint32_t row_bytes = stride * bits / 8u, xb = static_cast<std::uint32_t>(x) * bits / 8u;
        if (!swz) return static_cast<std::size_t>(y) * row_bytes + xb;
        return static_cast<std::size_t>(y / 8) * row_bytes * 8u + (xb / 16u) * 128u + static_cast<std::uint32_t>(y % 8) * 16u + (xb % 16u);
    }
    std::uint8_t get(int x, int y) const {
        const std::uint8_t v = p[offset(x, y)];
        return bits == 4u ? static_cast<std::uint8_t>((x & 1) ? (v >> 4) : (v & 15u)) : v;
    }
    void set(int x, int y, std::uint8_t v) const {
        std::uint8_t &b = p[offset(x, y)];
        if (bits == 4u) b = static_cast<std::uint8_t>((x & 1) ? ((b & 0x0Fu) | (v << 4)) : ((b & 0xF0u) | (v & 15u)));
        else b = v;
    }
};

std::uint64_t hash_rect(const Tex &t, const Rect &r) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) h = (h ^ t.get(x, y)) * 0x100000001B3ull;
    return h;
}
std::vector<std::uint8_t> read_rect(const Tex &t, const Rect &r) {
    std::vector<std::uint8_t> v;
    v.reserve(static_cast<std::size_t>(r.w * r.h));
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) v.push_back(t.get(x, y));
    return v;
}
void write_rect(const Tex &t, const Rect &r, const std::uint8_t *v) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) t.set(x, y, *v++);
}

// ---- fonts: symbol cells and the letter cells copied into them ----
struct FontLayout {
    int cell;
    Rect circle, cross, triangle, square;   // symbol cells
    struct { int x, y; } a, b, x, y, z, s;   // letter cells (top-left)
};
const FontLayout kBig{14, {0, 168, 14, 14}, {14, 168, 14, 14}, {28, 168, 14, 14}, {112, 168, 14, 14},
                      {0, 14}, {14, 14}, {322, 14}, {0, 28}, {14, 28}, {252, 14}};
const FontLayout kSmall{11, {0, 110, 11, 11}, {11, 110, 11, 11}, {22, 110, 11, 11}, {88, 110, 11, 11},
                        {11, 0}, {22, 0}, {44, 11}, {55, 11}, {66, 11}, {209, 0}};

// ---- the controller picture ----
struct PicGlyph {
    Rect clear;      // what is wiped (the original symbol plus a pixel around it)
    Rect text;       // where the new letters go: x, top, max width, cap height
    bool center;     // centre in text.w (face buttons) or start at text.x (shoulders)
};
const PicGlyph kPicL{{18, 12, 11, 14}, {15, 13, 14, 11}, false};   // label text starts at x 33
const PicGlyph kPicR{{163, 12, 14, 14}, {160, 13, 15, 11}, false};  // label text starts at x 180
const PicGlyph kPicTriangle{{195, 42, 15, 13}, {196, 43, 13, 11}, true};
const PicGlyph kPicSquare{{58, 157, 13, 13}, {59, 158, 11, 11}, true};
const PicGlyph kPicCircle{{193, 156, 15, 15}, {194, 158, 13, 11}, true};
const PicGlyph kPicCross{{127, 184, 15, 16}, {128, 186, 13, 11}, true};

struct Entry {
    Kind kind;
    std::vector<Rect> rects;                     // every rect this texture's patch touches
    std::vector<std::vector<std::uint8_t>> orig; // their original texels
    Glyphs applied{Glyphs::PlayStation};
    std::vector<std::uint8_t> applied_sig;       // signature rect as written for `applied`
    unsigned calls{};
};
std::unordered_map<std::uint32_t, Entry> g_entries;
std::unordered_map<std::uint32_t, unsigned> g_negative;   // address -> calls until the next look

bool g_log = false;

// Colour of palette entry `i` as the draw sees it.
std::uint32_t clut_rgba(const GpuDraw &d, std::uint32_t i) {
    i = ((i >> d.clut_shift) & d.clut_mask) | (d.clut_start << 4u);
    if (d.clut == nullptr) return 0xFFFFFFFFu;
    if (d.clut_format == 3u) { std::uint32_t v; std::memcpy(&v, d.clut + (i & 255u) * 4u, 4); return v; }
    std::uint16_t v; std::memcpy(&v, d.clut + (i & 511u) * 2u, 2);
    std::uint32_t r, g, b, a;
    switch (d.clut_format) {
    case 0: r = (v & 31u) << 3; g = ((v >> 5) & 63u) << 2; b = ((v >> 11) & 31u) << 3; a = 255u; break;
    case 1: r = (v & 31u) << 3; g = ((v >> 5) & 31u) << 3; b = ((v >> 10) & 31u) << 3; a = (v >> 15) ? 255u : 0u; break;
    default: r = (v & 15u) * 17u; g = ((v >> 4) & 15u) * 17u; b = ((v >> 8) & 15u) * 17u; a = ((v >> 12) & 15u) * 17u; break;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Coverage (0..1 per texel, w*h) of `text` drawn in the signed-distance font: cap height `cap` texels, bold.
std::vector<float> render_letters(const std::string &text, int w, int h, int cap, int max_w, bool center, int &used_w) {
    using namespace ui;
    // Cap height of the atlas font, measured on 'H'.
    static int cap_top = -1, cap_bot = -1;
    if (cap_top < 0) {
        const unsigned char *a = sdf_glyph('H' - 32);
        for (int y = 0; y < kSdfCellH; ++y)
            for (int x = 0; x < kSdfCellW; ++x)
                if (a[y * kSdfCellW + x] > 128) { if (cap_top < 0) cap_top = y; cap_bot = y; }
        if (cap_top < 0) { cap_top = 8; cap_bot = kSdfBase; }
    }
    const float k = static_cast<float>(cap) / static_cast<float>(cap_bot - cap_top + 1);   // texels per atlas pixel
    const float gap = 1.0f / k;                                                           // 1 texel between letters (atlas px)
    float adv_total = 0.0f;
    std::vector<float> adv;
    for (char c : text) { adv.push_back(static_cast<float>(sdf_advance(static_cast<unsigned>(c - 32))) / 16.0f); adv_total += adv.back(); }
    adv_total += gap * static_cast<float>(text.size() > 1 ? text.size() - 1 : 0);
    // Ink width is a bit less than the advances (side bearings); trim about one atlas pixel per side.
    const float ink = adv_total - 2.0f;
    float kx = k;
    if (ink * kx > static_cast<float>(max_w)) kx = static_cast<float>(max_w) / ink;   // condense to fit
    used_w = static_cast<int>(std::ceil(ink * kx));
    const float x0 = center ? (static_cast<float>(w) - ink * kx) * 0.5f : 0.0f;
    const float bold = 0.55f;   // atlas pixels added to every stroke edge
    std::vector<float> cov(static_cast<std::size_t>(w * h), 0.0f);
    constexpr int kSub = 4;
    for (int ty = 0; ty < h; ++ty) {
        for (int tx = 0; tx < w; ++tx) {
            int inside = 0;
            for (int sy = 0; sy < kSub; ++sy) {
                for (int sx = 0; sx < kSub; ++sx) {
                    const float px = static_cast<float>(tx) + (sx + 0.5f) / kSub, py = static_cast<float>(ty) + (sy + 0.5f) / kSub;
                    const float ay = py / k + static_cast<float>(cap_top) - 0.5f;
                    float pen = x0 / kx - 1.0f;   // atlas units; -1 = the trimmed left bearing
                    float best = -1e9f;
                    for (std::size_t i = 0; i < text.size(); ++i) {
                        const float lx = px / kx - pen + static_cast<float>(kSdfOriginX) - 0.5f;
                        const unsigned char *a = sdf_glyph(static_cast<unsigned>(text[i] - 32));
                        const int ix = static_cast<int>(std::floor(lx)), iy = static_cast<int>(std::floor(ay));
                        const float fx = lx - ix, fy = ay - iy;
                        const auto at = [&](int cx, int cy) { return (cx < 0 || cy < 0 || cx >= kSdfCellW || cy >= kSdfCellH) ? 0.0f : static_cast<float>(a[cy * kSdfCellW + cx]); };
                        const float v = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
                        best = std::max(best, (v - 128.0f) / 127.0f * kSdfSpread);
                        pen += adv[i] + gap;
                    }
                    if (best + bold > 0.0f) ++inside;
                }
            }
            cov[static_cast<std::size_t>(ty * w + tx)] = static_cast<float>(inside) / (kSub * kSub);
        }
    }
    return cov;
}

// Letters for one picture glyph (empty = keep the original symbol).
std::string picture_text(int which, Glyphs g) {   // 0 L, 1 R, 2 triangle, 3 square, 4 circle, 5 cross
    switch (g) {
    case Glyphs::Xbox: { static const char *t[6] = {"LB", "RB", "Y", "X", "B", "A"}; return t[which]; }
    case Glyphs::Nintendo: { static const char *t[6] = {"", "", "X", "Y", "A", "B"}; return t[which]; }
    case Glyphs::Keyboard: { static const char *t[6] = {"Q", "E", "S", "A", "X", "Z"}; return t[which]; }
    default: return "";
    }
}

void apply_font(const Tex &t, Entry &e, Glyphs g, const FontLayout &L) {
    // restore everything first, then copy letter cells over the symbols
    for (std::size_t i = 0; i < e.rects.size(); ++i) write_rect(t, e.rects[i], e.orig[i].data());
    if (g == Glyphs::PlayStation) return;
    struct Pair { Rect dst; int sx, sy; };
    std::vector<Pair> pairs;
    if (g == Glyphs::Keyboard) pairs = {{L.cross, L.z.x, L.z.y}, {L.circle, L.x.x, L.x.y}, {L.triangle, L.s.x, L.s.y}, {L.square, L.a.x, L.a.y}};
    else if (g == Glyphs::Nintendo) pairs = {{L.cross, L.b.x, L.b.y}, {L.circle, L.a.x, L.a.y}, {L.triangle, L.x.x, L.x.y}, {L.square, L.y.x, L.y.y}};
    else pairs = {{L.cross, L.a.x, L.a.y}, {L.circle, L.b.x, L.b.y}, {L.triangle, L.y.x, L.y.y}, {L.square, L.x.x, L.x.y}};
    for (const auto &p : pairs) {
        const auto src = read_rect(t, Rect{p.sx, p.sy, L.cell, L.cell});
        write_rect(t, p.dst, src.data());
    }
}

void apply_picture(const Tex &t, Entry &e, Glyphs g, const GpuDraw &d) {
    for (std::size_t i = 0; i < e.rects.size(); ++i) write_rect(t, e.rects[i], e.orig[i].data());
    if (g == Glyphs::PlayStation) return;
    const PicGlyph *gl[6] = {&kPicL, &kPicR, &kPicTriangle, &kPicSquare, &kPicCircle, &kPicCross};
    // Palette: the label red (most common index in the cleared area) and the symbol white (brightest index in it).
    const auto lum = [&](std::uint32_t c) { return (c & 255u) * 30u + ((c >> 8) & 255u) * 59u + ((c >> 16) & 255u) * 11u; };
    for (int which = 0; which < 6; ++which) {
        const std::string text = picture_text(which, g);
        if (text.empty()) continue;
        const PicGlyph &pg = *gl[which];
        const auto orig = read_rect(t, pg.clear);
        unsigned count[256] = {};
        std::uint8_t white = orig[0];
        for (std::uint8_t v : orig) { ++count[v]; if (lum(clut_rgba(d, v)) > lum(clut_rgba(d, white))) white = v; }
        const std::uint8_t red = static_cast<std::uint8_t>(std::max_element(count, count + 256) - count);
        const std::uint32_t cr = clut_rgba(d, red), cw = clut_rgba(d, white);
        // palette index for each coverage step
        std::uint8_t level[17];
        for (int l = 0; l <= 16; ++l) {
            const float f = l / 16.0f;
            float want[4];
            for (int ch = 0; ch < 4; ++ch) want[ch] = ((cr >> (ch * 8)) & 255u) * (1.0f - f) + ((cw >> (ch * 8)) & 255u) * f;
            float best = 1e30f; int bi = red;
            const int n = d.tex_format == 4u ? 16 : 256;
            for (int i = 0; i < n; ++i) {
                const std::uint32_t c = clut_rgba(d, static_cast<std::uint32_t>(i));
                float dist = 0.0f;
                for (int ch = 0; ch < 4; ++ch) { const float q = static_cast<float>((c >> (ch * 8)) & 255u) - want[ch]; dist += q * q * (ch == 3 ? 4.0f : 1.0f); }
                if (dist < best) { best = dist; bi = i; }
            }
            level[l] = static_cast<std::uint8_t>(bi);
        }
        for (int y = pg.clear.y; y < pg.clear.y + pg.clear.h; ++y)
            for (int x = pg.clear.x; x < pg.clear.x + pg.clear.w; ++x) t.set(x, y, red);
        // shoulders: one letter starts where the original L / R did, two letters use the room to the left as well
        const int x0 = (!pg.center && text.size() == 1) ? pg.clear.x + 1 : pg.text.x;
        const int box_w = pg.text.x + pg.text.w - x0;
        int used = 0;
        const auto cov = render_letters(text, box_w, pg.text.h + 1, pg.text.h, box_w, pg.center, used);
        for (int y = 0; y <= pg.text.h; ++y)
            for (int x = 0; x < box_w; ++x) {
                const float c = cov[static_cast<std::size_t>(y * box_w + x)];
                if (c <= 0.0f) continue;
                t.set(x0 + x, pg.text.y + y, level[static_cast<int>(std::lround(c * 16.0f))]);
            }
    }
}

void apply(const Tex &t, Entry &e, Glyphs g, const GpuDraw &d) {
    if (e.kind == Kind::Picture) apply_picture(t, e, g, d);
    else apply_font(t, e, g, e.kind == Kind::BigFont ? kBig : kSmall);
    e.applied = g;
    for (const auto &s : kShapes) if (s.kind == e.kind) e.applied_sig = read_rect(t, s.signature);
    if (g_log) {
        char b[160];
        std::snprintf(b, sizeof b, "[PAD] in-game button pictures: %s -> %s", e.kind == Kind::Picture ? "controller screen" : e.kind == Kind::BigFont ? "large font" : "small font", pad::glyphs_name(g));
        log_line(category::kGe, b);
    }
}

Entry make_entry(const Tex &t, Kind kind) {
    Entry e;
    e.kind = kind;
    if (kind == Kind::Picture) {
        for (const PicGlyph *p : {&kPicL, &kPicR, &kPicTriangle, &kPicSquare, &kPicCircle, &kPicCross}) {
            e.rects.push_back(p->clear);
            e.rects.push_back(Rect{p->text.x, p->text.y, p->text.w, p->text.h + 1});
        }
    } else {
        const FontLayout &L = kind == Kind::BigFont ? kBig : kSmall;
        e.rects = {L.circle, L.cross, L.triangle, L.square};
    }
    for (const auto &r : e.rects) e.orig.push_back(read_rect(t, r));
    return e;
}

} // namespace

void glyph_patch_draw(psprecomp::GuestMemory &memory, const GpuDraw &d) {
    static const bool off = [] { const char *e = std::getenv("RR_GLYPH_PATCH"); return e != nullptr && e[0] == '0'; }();
    if (off) return;
    g_log = [] { static const bool l = std::getenv("RR_GLYPH_LOG") != nullptr; return l; }();
    const Shape *shape = nullptr;
    for (const auto &s : kShapes)
        if (d.tex_format == s.format && d.tex_width == s.width && d.tex_height == s.height && d.tex_stride == s.stride) { shape = &s; break; }
    if (shape == nullptr) return;
    const std::uint32_t bits = shape->format == 4u ? 4u : 8u;
    const std::size_t bytes = static_cast<std::size_t>(shape->stride) * shape->height * bits / 8u;
    std::uint8_t *p = memory.raw_pointer(d.tex_addr, bytes);
    if (p == nullptr) return;
    const Tex t{p, shape->stride, bits, d.tex_swizzled};
    const Glyphs want = pad::glyphs();

    auto it = g_entries.find(d.tex_addr);
    if (it != g_entries.end()) {
        Entry &e = it->second;
        if (e.kind != shape->kind) { g_entries.erase(it); return; }
        if (e.applied == want && (++e.calls & 63u) != 0u) return;   // re-check the memory now and then
        const auto now = read_rect(t, shape->signature);
        if (now == e.applied_sig) { if (e.applied != want) apply(t, e, want, d); return; }
        if (hash_rect(t, shape->signature) == shape->hash) {   // the game loaded the texture again
            e = make_entry(t, shape->kind);
            apply(t, e, want, d);
            return;
        }
        g_entries.erase(it);   // something else lives here now
        return;
    }
    unsigned &neg = g_negative[d.tex_addr];
    if (neg > 0u) { --neg; return; }
    if (hash_rect(t, shape->signature) != shape->hash) { neg = 240u; return; }
    g_negative.erase(d.tex_addr);
    Entry e = make_entry(t, shape->kind);
    apply(t, e, want, d);
    g_entries.emplace(d.tex_addr, std::move(e));
}

} // namespace rr
