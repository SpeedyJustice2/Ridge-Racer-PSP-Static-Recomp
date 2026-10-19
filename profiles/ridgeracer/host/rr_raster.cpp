#include "rr_raster.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rr {

namespace {

inline std::uint32_t pack(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

inline std::uint32_t expand_565(std::uint32_t p) {
    const std::uint32_t r = p & 31u, g = (p >> 5u) & 63u, b = (p >> 11u) & 31u;
    return pack((r << 3u) | (r >> 2u), (g << 2u) | (g >> 4u), (b << 3u) | (b >> 2u), 255u);
}
inline std::uint32_t expand_5551(std::uint32_t p) {
    const std::uint32_t r = p & 31u, g = (p >> 5u) & 31u, b = (p >> 10u) & 31u;
    return pack((r << 3u) | (r >> 2u), (g << 3u) | (g >> 2u), (b << 3u) | (b >> 2u), (p & 0x8000u) ? 255u : 0u);
}
inline std::uint32_t expand_4444(std::uint32_t p) {
    return pack((p & 15u) * 17u, ((p >> 4u) & 15u) * 17u, ((p >> 8u) & 15u) * 17u, ((p >> 12u) & 15u) * 17u);
}
inline std::uint32_t expand(std::uint32_t format, std::uint32_t p) {
    switch (format) {
    case 0: return expand_565(p);
    case 1: return expand_5551(p);
    case 2: return expand_4444(p);
    default: return p;
    }
}
inline std::uint32_t compress(std::uint32_t format, std::uint32_t c) {
    const std::uint32_t r = c & 255u, g = (c >> 8u) & 255u, b = (c >> 16u) & 255u, a = c >> 24u;
    switch (format) {
    case 0: return (r >> 3u) | ((g >> 2u) << 5u) | ((b >> 3u) << 11u);
    case 1: return (r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u) | ((a >= 128u) ? 0x8000u : 0u);
    case 2: return (r >> 4u) | ((g >> 4u) << 4u) | ((b >> 4u) << 8u) | ((a >> 4u) << 12u);
    default: return c;
    }
}
inline std::uint32_t bytes_per_pixel(std::uint32_t format) { return format == 3u ? 4u : 2u; }

struct Target {
    std::uint8_t *fb{};
    std::uint32_t stride{}, format{};
    std::uint8_t *zb{};
    std::uint32_t zstride{};
    inline std::uint32_t read(std::int32_t x, std::int32_t y) const {
        const std::size_t at = (static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)) * bytes_per_pixel(format);
        if (format == 3u) { std::uint32_t v; std::memcpy(&v, fb + at, 4); return v; }
        std::uint16_t v; std::memcpy(&v, fb + at, 2); return expand(format, v);
    }
    inline void write(std::int32_t x, std::int32_t y, std::uint32_t c) const {
        const std::size_t at = (static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)) * bytes_per_pixel(format);
        if (format == 3u) { std::memcpy(fb + at, &c, 4); return; }
        const std::uint16_t v = static_cast<std::uint16_t>(compress(format, c));
        std::memcpy(fb + at, &v, 2);
    }
    inline std::uint16_t read_z(std::int32_t x, std::int32_t y) const {
        std::uint16_t v;
        std::memcpy(&v, zb + (static_cast<std::size_t>(y) * zstride + static_cast<std::size_t>(x)) * 2u, 2);
        return v;
    }
    inline void write_z(std::int32_t x, std::int32_t y, std::uint16_t v) const {
        std::memcpy(zb + (static_cast<std::size_t>(y) * zstride + static_cast<std::size_t>(x)) * 2u, &v, 2);
    }
};

struct Texture {
    const std::uint8_t *data{};
    const std::uint32_t *decoded{};  // optional pre-decoded RGBA8888 texels (width*height)
    bool valid{};
    std::uint32_t decoded_clut[256]{};
    bool clut_decoded{};
};

inline std::uint32_t clut_entry(const GpuDraw &d, std::uint32_t index) {
    index = ((index >> d.clut_shift) & d.clut_mask) | (d.clut_start << 4u);
    if (d.clut == nullptr) return 0xFFFFFFFFu;
    if (d.clut_format == 3u) {
        index &= 255u;
        std::uint32_t v; std::memcpy(&v, d.clut + index * 4u, 4); return v;
    }
    index &= 511u;
    std::uint16_t v; std::memcpy(&v, d.clut + index * 2u, 2);
    return expand(d.clut_format, v);
}

// Returns the byte offset (and nibble flag) of texel (x, y).
inline std::size_t texel_offset(const GpuDraw &d, std::uint32_t x, std::uint32_t y, std::uint32_t bits, bool &high_nibble) {
    const std::uint32_t row_bytes = d.tex_stride * bits / 8u;
    const std::uint32_t xb = x * bits / 8u;
    high_nibble = bits == 4u && (x & 1u);
    if (!d.tex_swizzled) return static_cast<std::size_t>(y) * row_bytes + xb;
    return static_cast<std::size_t>(y / 8u) * row_bytes * 8u + (xb / 16u) * 128u + (y % 8u) * 16u + (xb % 16u);
}

inline std::uint32_t fetch_texel(const GpuDraw &d, const Texture &t, std::int32_t x, std::int32_t y) {
    const std::int32_t w = static_cast<std::int32_t>(d.tex_width), h = static_cast<std::int32_t>(d.tex_height);
    if (d.clamp_u) x = std::clamp(x, 0, w - 1); else x &= (w - 1);
    if (d.clamp_v) y = std::clamp(y, 0, h - 1); else y &= (h - 1);
    if (t.decoded != nullptr) return t.decoded[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
    bool hi = false;
    switch (d.tex_format) {
    case 0: case 1: case 2: {
        std::uint16_t v; std::memcpy(&v, t.data + texel_offset(d, x, y, 16, hi), 2); return expand(d.tex_format, v);
    }
    case 3: { std::uint32_t v; std::memcpy(&v, t.data + texel_offset(d, x, y, 32, hi), 4); return v; }
    case 4: {
        const std::uint8_t b = t.data[texel_offset(d, x, y, 4, hi)];
        return clut_entry(d, hi ? (b >> 4u) : (b & 15u));
    }
    case 5: return clut_entry(d, t.data[texel_offset(d, x, y, 8, hi)]);
    case 6: { std::uint16_t v; std::memcpy(&v, t.data + texel_offset(d, x, y, 16, hi), 2); return clut_entry(d, v); }
    case 7: { std::uint32_t v; std::memcpy(&v, t.data + texel_offset(d, x, y, 32, hi), 4); return clut_entry(d, v); }
    default: return 0xFFFF00FFu;
    }
}

inline std::uint32_t texel_bits(std::uint32_t format) {
    switch (format) {
    case 0: case 1: case 2: case 6: return 16;
    case 3: case 7: return 32;
    case 4: return 4;
    case 5: return 8;
    default: return 0;
    }
}

inline std::uint32_t sample(const GpuDraw &d, const Texture &t, float u, float v) {
    if (!d.tex_linear) return fetch_texel(d, t, static_cast<std::int32_t>(std::floor(u)), static_cast<std::int32_t>(std::floor(v)));
    u -= 0.5f; v -= 0.5f;
    const float fu = std::floor(u), fv = std::floor(v);
    const std::int32_t x0 = static_cast<std::int32_t>(fu), y0 = static_cast<std::int32_t>(fv);
    const float ax = u - fu, ay = v - fv;
    const std::uint32_t c00 = fetch_texel(d, t, x0, y0), c10 = fetch_texel(d, t, x0 + 1, y0);
    const std::uint32_t c01 = fetch_texel(d, t, x0, y0 + 1), c11 = fetch_texel(d, t, x0 + 1, y0 + 1);
    std::uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        const float a = static_cast<float>((c00 >> s) & 255u), b = static_cast<float>((c10 >> s) & 255u);
        const float c = static_cast<float>((c01 >> s) & 255u), e = static_cast<float>((c11 >> s) & 255u);
        const float top = a + (b - a) * ax, bottom = c + (e - c) * ax;
        out |= static_cast<std::uint32_t>(std::clamp(top + (bottom - top) * ay + 0.5f, 0.0f, 255.0f)) << s;
    }
    return out;
}

inline bool compare(std::uint32_t func, std::uint32_t a, std::uint32_t b) {
    switch (func) {
    case 0: return false;
    case 1: return true;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

struct Rgb { int r, g, b; };

// Alpha channel of a blend factor: colour factors become their alpha counterparts, a fixed colour counts as alpha 1.
inline int blend_alpha_factor(std::uint32_t f, bool is_src, int sa, int da) {
    switch (f) {
    case 0: return is_src ? da : sa;
    case 1: return is_src ? 255 - da : 255 - sa;
    case 2: return sa;
    case 3: return 255 - sa;
    case 4: return da;
    case 5: return 255 - da;
    case 6: return clamp255(sa * 2);
    case 7: return clamp255(255 - sa * 2);
    case 8: return clamp255(da * 2);
    case 9: return clamp255(255 - da * 2);
    default: return 255;
    }
}

inline Rgb blend_factor(std::uint32_t f, bool is_src, const Rgb &src, int sa, const Rgb &dst, int da, std::uint32_t fix) {
    const Rgb fixed{static_cast<int>(fix & 255u), static_cast<int>((fix >> 8u) & 255u), static_cast<int>((fix >> 16u) & 255u)};
    const auto splat = [](int v) { return Rgb{v, v, v}; };
    if (is_src) {
        switch (f) {
        case 0: return dst;
        case 1: return Rgb{255 - dst.r, 255 - dst.g, 255 - dst.b};
        case 2: return splat(sa);
        case 3: return splat(255 - sa);
        case 4: return splat(da);
        case 5: return splat(255 - da);
        case 6: return splat(clamp255(sa * 2));
        case 7: return splat(clamp255(255 - sa * 2));
        case 8: return splat(clamp255(da * 2));
        case 9: return splat(clamp255(255 - da * 2));
        default: return fixed;
        }
    }
    switch (f) {
    case 0: return src;
    case 1: return Rgb{255 - src.r, 255 - src.g, 255 - src.b};
    case 2: return splat(sa);
    case 3: return splat(255 - sa);
    case 4: return splat(da);
    case 5: return splat(255 - da);
    case 6: return splat(clamp255(sa * 2));
    case 7: return splat(clamp255(255 - sa * 2));
    case 8: return splat(clamp255(da * 2));
    case 9: return splat(clamp255(255 - da * 2));
    default: return fixed;
    }
}

struct Fragment { float z, inv_w, u, v, r, g, b, a; float fog{1.0f}; };

struct Pipeline {
    const GpuDraw &d;
    const Target &t;
    const Texture &tex;
    std::uint64_t &written;
    GpuDiag &diag;
    std::int32_t band_lo{0}, band_hi{INT_MAX};

    void shade(std::int32_t x, std::int32_t y, const Fragment &f) const {
        if (x < d.scissor_x0 || x > d.scissor_x1 || y < d.scissor_y0 || y > d.scissor_y1) return;
        if (y < band_lo || y > band_hi) return;
        const std::int32_t zi = static_cast<std::int32_t>(std::clamp(f.z + 0.5f, 0.0f, 65535.0f));
        if (d.clear_mode) {
            if (d.clear_color || d.clear_alpha) {
                std::uint32_t old = t.read(x, y);
                std::uint32_t color = pack(clamp255(static_cast<int>(f.r)), clamp255(static_cast<int>(f.g)),
                                           clamp255(static_cast<int>(f.b)), clamp255(static_cast<int>(f.a)));
                std::uint32_t keep = 0;
                if (!d.clear_color) keep |= 0x00FFFFFFu;
                if (!d.clear_alpha) keep |= 0xFF000000u;
                keep |= d.mask_rgb | (d.mask_alpha << 24u);
                t.write(x, y, (color & ~keep) | (old & keep));
                ++written;
            }
            if (d.clear_depth && t.zb != nullptr) t.write_z(x, y, static_cast<std::uint16_t>(zi));
            return;
        }
        float r = f.r, g = f.g, b = f.b, a = f.a;
        std::uint32_t sampled_texel = 0u;
        float sampled_u = 0.0f, sampled_v = 0.0f;
        if (d.tex_enable && tex.valid) {
            float u = f.u, v = f.v;
            if (f.inv_w != 1.0f) { const float w = 1.0f / f.inv_w; u *= w; v *= w; }
            const std::uint32_t c = sample(d, tex, u, v);
            sampled_texel = c; sampled_u = u; sampled_v = v;
            const float tr = static_cast<float>(c & 255u), tg = static_cast<float>((c >> 8u) & 255u);
            const float tb = static_cast<float>((c >> 16u) & 255u), ta = static_cast<float>(c >> 24u);
            switch (d.tex_func) {
            case 0: // modulate
                r = tr * r / 255.0f; g = tg * g / 255.0f; b = tb * b / 255.0f;
                if (d.tex_alpha) a = ta * a / 255.0f;
                break;
            case 1: // decal
                if (d.tex_alpha) {
                    r = tr * ta / 255.0f + r * (255.0f - ta) / 255.0f;
                    g = tg * ta / 255.0f + g * (255.0f - ta) / 255.0f;
                    b = tb * ta / 255.0f + b * (255.0f - ta) / 255.0f;
                } else { r = tr; g = tg; b = tb; }
                break;
            case 2: { // blend with environment color
                const float er = static_cast<float>(d.tex_env & 255u), eg = static_cast<float>((d.tex_env >> 8u) & 255u);
                const float eb = static_cast<float>((d.tex_env >> 16u) & 255u);
                r = r * (255.0f - tr) / 255.0f + er * tr / 255.0f;
                g = g * (255.0f - tg) / 255.0f + eg * tg / 255.0f;
                b = b * (255.0f - tb) / 255.0f + eb * tb / 255.0f;
                if (d.tex_alpha) a = ta * a / 255.0f;
                break;
            }
            case 3: // replace
                r = tr; g = tg; b = tb;
                if (d.tex_alpha) a = ta;
                break;
            default: // add
                r = std::min(r + tr, 255.0f); g = std::min(g + tg, 255.0f); b = std::min(b + tb, 255.0f);
                if (d.tex_alpha) a = ta * a / 255.0f;
                break;
            }
            if (d.tex_double) { r = std::min(r * 2.0f, 255.0f); g = std::min(g * 2.0f, 255.0f); b = std::min(b * 2.0f, 255.0f); }
        }
        if (d.fog) {
            const float k = std::clamp(f.fog, 0.0f, 1.0f), k1 = 1.0f - k;
            r = r * k + static_cast<float>(d.fog_color & 255u) * k1;
            g = g * k + static_cast<float>((d.fog_color >> 8u) & 255u) * k1;
            b = b * k + static_cast<float>((d.fog_color >> 16u) & 255u) * k1;
        }
        const int ir = clamp255(static_cast<int>(r + 0.5f)), ig = clamp255(static_cast<int>(g + 0.5f));
        const int ib = clamp255(static_cast<int>(b + 0.5f)), ia = clamp255(static_cast<int>(a + 0.5f));
        if (d.alpha_test && !compare(d.atest_func, static_cast<std::uint32_t>(ia) & d.atest_mask, d.atest_ref & d.atest_mask)) return;
        if (d.color_test) {
            const std::uint32_t cc = (static_cast<std::uint32_t>(ir) | (static_cast<std::uint32_t>(ig) << 8u) | (static_cast<std::uint32_t>(ib) << 16u)) & d.ctest_mask;
            const std::uint32_t cr = d.ctest_ref & d.ctest_mask;
            if (d.ctest_func == 0u || (d.ctest_func == 2u && cc != cr) || (d.ctest_func == 3u && cc == cr)) return;
        }
        // Stencil lives in the framebuffer's alpha channel.
        const bool sten = d.stencil_test && t.format != 0u;
        std::uint32_t sold = 0u, snew = 0u;
        if (sten) {
            sold = t.read(x, y);
            const std::uint32_t sv = sold >> 24u;
            const auto apply = [&](std::uint32_t op) -> std::uint32_t {
                switch (op) {
                case 1: return 0u;
                case 2: return d.stencil_ref;
                case 3: return ~sv & 0xFFu;
                case 4: return sv < 255u ? sv + 1u : sv;
                case 5: return sv > 0u ? sv - 1u : sv;
                default: return sv;
                }
            };
            const auto store = [&](std::uint32_t v) {
                const std::uint32_t nv = (v & ~d.mask_alpha) | (sv & d.mask_alpha);
                t.write(x, y, (sold & 0x00FFFFFFu) | (nv << 24u));
            };
            if (!compare(d.stencil_func, d.stencil_ref & d.stencil_mask, sv & d.stencil_mask)) { store(apply(d.stencil_sfail)); return; }
            if (d.depth_test && t.zb != nullptr &&
                !compare(d.depth_func, static_cast<std::uint32_t>(zi), static_cast<std::uint32_t>(t.read_z(x, y)))) {
                store(apply(d.stencil_zfail)); return;
            }
            snew = apply(d.stencil_zpass);
            snew = (snew & ~d.mask_alpha) | (sv & d.mask_alpha);
        } else if (d.depth_test && t.zb != nullptr) {
            if (!compare(d.depth_func, static_cast<std::uint32_t>(zi), static_cast<std::uint32_t>(t.read_z(x, y)))) return;
        }
        std::uint32_t old = 0;
        int out_r = ir, out_g = ig, out_b = ib, out_a = ia;
        const bool need_old = d.blend || d.mask_rgb != 0u || d.mask_alpha != 0u || t.format != 3u;
        if (need_old) old = t.read(x, y);
        if (d.blend) {
            const Rgb src{ir, ig, ib};
            const Rgb dst{static_cast<int>(old & 255u), static_cast<int>((old >> 8u) & 255u), static_cast<int>((old >> 16u) & 255u)};
            const int da = static_cast<int>(old >> 24u);
            const Rgb fs = blend_factor(d.blend_src, true, src, ia, dst, da, d.fix_a);
            const Rgb fd = blend_factor(d.blend_dst, false, src, ia, dst, da, d.fix_b);
            const auto combine = [&](int s, int fsv, int dv, int fdv) {
                const int sp = s * fsv / 255, dp = dv * fdv / 255;
                switch (d.blend_eq) {
                case 0: return clamp255(sp + dp);
                case 1: return clamp255(sp - dp);
                case 2: return clamp255(dp - sp);
                case 3: return std::min(s, dv);
                case 4: return std::max(s, dv);
                default: return std::abs(s - dv);
                }
            };
            out_r = combine(src.r, fs.r, dst.r, fd.r);
            out_g = combine(src.g, fs.g, dst.g, fd.g);
            out_b = combine(src.b, fs.b, dst.b, fd.b);
            // Alpha: 16-bit targets keep the source alpha (it feeds the stencil bits).  A 32-bit target blends its alpha with the
            // same factors (alpha counterparts) - the glare palette's alpha ramp survives a blend into it only this way.
            out_a = ia;
        }
        // Count purple that did NOT come straight from a purple vertex colour (pink UI art is legitimate).
        if (out_b >= out_r - 8 && out_r > 2 * out_g + 8 && out_b > 2 * out_g + 8 && !(f.r > f.g + 24.0f && f.b > f.g + 24.0f)) {
            ++diag.purple_pixels;
            if (!diag.have_sample) {
                diag.have_sample = true; diag.sample_texel = sampled_texel;
                diag.sample_vr = f.r; diag.sample_vg = f.g; diag.sample_vb = f.b;
                diag.sample_out_r = out_r; diag.sample_out_g = out_g; diag.sample_out_b = out_b;
                diag.sample_u = sampled_u; diag.sample_v = sampled_v;
            }
        }
        std::uint32_t color = pack(out_r, out_g, out_b, out_a);
        std::uint32_t keep = d.mask_rgb | (d.mask_alpha << 24u);
        // With the stencil test off the framebuffer's alpha/stencil bits are left alone on 16-bit targets.  The game
        // stores palettes in the unused margin of the screen buffer; alpha-clobbering them turned textures purple.
        if (!d.stencil_test && (t.format != 3u || d.blend)) { keep |= 0xFF000000u; if (!need_old) old = t.read(x, y); }
        if (sten) color = (color & 0x00FFFFFFu) | (snew << 24u);
        if (keep != 0u) color = (color & ~keep) | (old & keep);
        t.write(x, y, color);
        if (d.depth_test && d.depth_write && t.zb != nullptr) t.write_z(x, y, static_cast<std::uint16_t>(zi));
        ++written;
    }
};


// Small persistent worker pool used to split big draws into horizontal bands.
class BandPool {
public:
    static BandPool &get() { static BandPool pool; return pool; }
    unsigned threads() const { return static_cast<unsigned>(workers_.size()) + 1u; }
    // Runs fn(0..jobs-1); the calling thread participates.  Returns when every job has finished.
    void run(unsigned jobs, const std::function<void(unsigned)> &fn) {
        if (jobs == 0u) return;
        if (workers_.empty() || jobs == 1u) { for (unsigned i = 0; i < jobs; ++i) fn(i); return; }
        auto job = std::make_shared<Run>();
        job->fn = &fn; job->jobs = jobs; job->remaining.store(jobs);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = job; ++generation_;
        }
        cv_.notify_all();
        work(*job);
        std::unique_lock<std::mutex> lock(job->mutex);
        job->done_cv.wait(lock, [&] { return job->remaining.load() == 0u; });
    }
    ~BandPool() {
        { std::lock_guard<std::mutex> lock(mutex_); quit_ = true; }
        cv_.notify_all();
        for (std::thread &t : workers_) t.join();
    }
private:
    // One batch of jobs.  Workers only ever touch the Run they claimed, so a slow worker from an earlier batch
    // can never pick up work belonging to a later one.
    struct Run {
        const std::function<void(unsigned)> *fn{};
        unsigned jobs{};
        std::atomic<unsigned> next{0}, remaining{0};
        std::mutex mutex;
        std::condition_variable done_cv;
    };
    BandPool() {
        unsigned n = std::thread::hardware_concurrency();
        if (const char *e = std::getenv("RR_RASTER_THREADS")) { const int v = std::atoi(e); if (v > 0) n = static_cast<unsigned>(v); }
        n = std::clamp(n, 1u, 16u);
        for (unsigned i = 1; i < n; ++i) workers_.emplace_back([this] { loop(); });
    }
    static void work(Run &run) {
        for (;;) {
            const unsigned i = run.next.fetch_add(1);
            if (i >= run.jobs) return;
            (*run.fn)(i);
            if (run.remaining.fetch_sub(1) == 1u) { std::lock_guard<std::mutex> lock(run.mutex); run.done_cv.notify_all(); }
        }
    }
    void loop() {
        std::uint64_t seen = 0;
        for (;;) {
            std::shared_ptr<Run> run;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
                run = current_;
            }
            if (run) work(*run);
        }
    }
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<Run> current_;
    std::uint64_t generation_{};
    bool quit_{};
};

} // namespace

void SoftwareBackend::draw(const GpuDraw &d_in, GpuPrim prim, const GpuVertex *v, std::size_t count) {
    if (count == 0u) return;
    // A buffer near the end of VRAM only has the rows that fit (the game keeps small work buffers there and draws into
    // the first few rows): clip to those rows instead of dropping the draw.
    std::uint32_t fit_rows = 272u;
    {
        const std::uint32_t row_bytes = d_in.fb_stride * bytes_per_pixel(d_in.fb_format);
        if (d_in.fb_addr >= 0x04000000u && d_in.fb_addr < 0x04200000u && row_bytes != 0u)
            fit_rows = std::min<std::uint32_t>(272u, (0x04200000u - d_in.fb_addr) / row_bytes);
    }
    static const bool no_short = std::getenv("RR_NO_SHORT_RT") != nullptr;   // A/B switch: old behaviour (drop)
    if (fit_rows == 0u || (no_short && fit_rows < 272u)) return;
    GpuDraw clipped;
    const GpuDraw *dp = &d_in;
    if (fit_rows < 272u) { clipped = d_in; clipped.scissor_y1 = std::min<std::int32_t>(clipped.scissor_y1, static_cast<std::int32_t>(fit_rows) - 1); dp = &clipped; }
    const GpuDraw &d = *dp;
    Target target;
    target.stride = d.fb_stride;
    target.format = d.fb_format;
    target.zstride = d.z_stride;
    const std::size_t fb_bytes = static_cast<std::size_t>(d.fb_stride) * fit_rows * bytes_per_pixel(d.fb_format);
    target.fb = memory_.raw_pointer(d.fb_addr, fb_bytes);
    if (target.fb == nullptr) return;
    if (d.depth_test || d.clear_depth) target.zb = memory_.raw_pointer(d.z_addr, static_cast<std::size_t>(d.z_stride) * 272u * 2u);

    Texture tex;
    if (d.tex_enable && !d.clear_mode) {
        const std::uint32_t bits = texel_bits(d.tex_format);
        if (bits != 0u) {
            const std::size_t bytes = static_cast<std::size_t>(std::max(d.tex_stride, d.tex_width)) * d.tex_height * bits / 8u;
            const std::uint8_t *p = static_cast<const psprecomp::GuestMemory &>(memory_).raw_pointer(d.tex_addr, bytes);
            if (p != nullptr) { tex.data = p; tex.valid = true; }
        }
        if (!tex.valid) ++unsupported_texture_draws_;
    }
    const std::int32_t fb_h = 272;
    const std::int32_t x_min = std::max(d.scissor_x0, 0), y_min = std::max(d.scissor_y0, 0);
    const std::int32_t x_max = std::min<std::int32_t>(d.scissor_x1, static_cast<std::int32_t>(d.fb_stride) - 1);
    const std::int32_t y_max = std::min(d.scissor_y1, fb_h - 1);


    // Estimate the work (sum of bounding-box areas) to decide on pre-decoding and threading.
    std::uint64_t work = 0;
    {
        const auto box_area = [&](float x0, float y0, float x1, float y1) {
            const float w = std::max(0.0f, std::min(x1, static_cast<float>(x_max + 1)) - std::max(x0, static_cast<float>(x_min)));
            const float h = std::max(0.0f, std::min(y1, static_cast<float>(y_max + 1)) - std::max(y0, static_cast<float>(y_min)));
            return static_cast<std::uint64_t>(w * h);
        };
        if (prim == GpuPrim::Sprites) {
            for (std::size_t i = 0; i + 1 < count; i += 2)
                work += box_area(std::min(v[i].x, v[i + 1].x), std::min(v[i].y, v[i + 1].y), std::max(v[i].x, v[i + 1].x), std::max(v[i].y, v[i + 1].y));
        } else if (prim == GpuPrim::Triangles) {
            for (std::size_t i = 0; i + 2 < count; i += 3)
                work += box_area(std::min({v[i].x, v[i + 1].x, v[i + 2].x}), std::min({v[i].y, v[i + 1].y, v[i + 2].y}),
                                 std::max({v[i].x, v[i + 1].x, v[i + 2].x}), std::max({v[i].y, v[i + 1].y, v[i + 2].y}));
        }
    }
    BandPool &pool = BandPool::get();

    // Pre-decode the texture (palettes, swizzle, 16-bit formats) once when enough pixels will sample it.
    static std::vector<std::uint32_t> decoded_texels;
    static const bool no_decode = std::getenv("RR_NO_DECODE") != nullptr;
    if (!no_decode && tex.valid && d.tex_width != 0u && d.tex_height != 0u && work * 2u >= static_cast<std::uint64_t>(d.tex_width) * d.tex_height &&
        d.tex_width <= 1024u && d.tex_height <= 1024u && (d.tex_width & (d.tex_width - 1u)) == 0u && (d.tex_height & (d.tex_height - 1u)) == 0u) {
        decoded_texels.resize(static_cast<std::size_t>(d.tex_width) * d.tex_height);
        const std::uint32_t tw = d.tex_width, th = d.tex_height;
        const unsigned jobs = (tw * th >= 16384u) ? std::min(pool.threads() * 2u, 16u) : 1u;
        pool.run(jobs, [&](unsigned job) {
            const std::uint32_t y0 = th * job / jobs, y1 = th * (job + 1u) / jobs;
            for (std::uint32_t y = y0; y < y1; ++y)
                for (std::uint32_t x = 0; x < tw; ++x)
                    decoded_texels[static_cast<std::size_t>(y) * tw + x] = fetch_texel(d, tex, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
        });
        tex.decoded = decoded_texels.data();
    }

    const auto render = [&](const Pipeline &pipe, std::int32_t y_lo, std::int32_t y_hi) {
        const std::int32_t ymin = std::max(y_min, y_lo), ymax = std::min(y_max, y_hi);
        if (ymin > ymax && (prim == GpuPrim::Sprites || prim == GpuPrim::Triangles)) return;
        if (prim == GpuPrim::Sprites) {
            for (std::size_t i = 0; i + 1 < count; i += 2) {
                GpuVertex a = v[i], b = v[i + 1];
                if (a.x > b.x) { std::swap(a.x, b.x); std::swap(a.u, b.u); }
                if (a.y > b.y) { std::swap(a.y, b.y); std::swap(a.v, b.v); }
                const float dx = b.x - a.x, dy = b.y - a.y;
                if (dx <= 0.0f || dy <= 0.0f) continue;
                const std::int32_t px0 = std::max(static_cast<std::int32_t>(std::ceil(a.x - 0.5f)), x_min);
                const std::int32_t px1 = std::min(static_cast<std::int32_t>(std::ceil(b.x - 0.5f)) - 1, x_max);
                const std::int32_t py0 = std::max(static_cast<std::int32_t>(std::ceil(a.y - 0.5f)), ymin);
                const std::int32_t py1 = std::min(static_cast<std::int32_t>(std::ceil(b.y - 0.5f)) - 1, ymax);
                const float du = (b.u - a.u) / dx, dv = (b.v - a.v) / dy;
                for (std::int32_t py = py0; py <= py1; ++py) {
                    for (std::int32_t px = px0; px <= px1; ++px) {
                        Fragment f{b.z, 1.0f, a.u + (px + 0.5f - a.x) * du, a.v + (py + 0.5f - a.y) * dv, b.r, b.g, b.b, b.a, b.fog};
                        pipe.shade(px, py, f);
                    }
                }
            }
            return;
        }
        if (prim == GpuPrim::Points) {
            for (std::size_t i = 0; i < count; ++i) {
                const Fragment f{v[i].z, 1.0f, v[i].u, v[i].v, v[i].r, v[i].g, v[i].b, v[i].a, v[i].fog};
                pipe.shade(static_cast<std::int32_t>(std::floor(v[i].x)), static_cast<std::int32_t>(std::floor(v[i].y)), f);
            }
            return;
        }
        if (prim == GpuPrim::Lines) {
            for (std::size_t i = 0; i + 1 < count; i += 2) {
                const GpuVertex &a = v[i], &b = v[i + 1];
                const float dx = b.x - a.x, dy = b.y - a.y;
                const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
                for (int s = 0; s <= steps; ++s) {
                    const float t = static_cast<float>(s) / static_cast<float>(steps);
                    const Fragment f{a.z + (b.z - a.z) * t, 1.0f, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t,
                                     a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t, a.fog + (b.fog - a.fog) * t};
                    pipe.shade(static_cast<std::int32_t>(std::floor(a.x + dx * t)), static_cast<std::int32_t>(std::floor(a.y + dy * t)), f);
                }
            }
            return;
        }
        // Triangles.
        for (std::size_t i = 0; i + 2 < count; i += 3) {
            const GpuVertex *p0 = &v[i], *p1 = &v[i + 1], *p2 = &v[i + 2];
            float area = (p1->x - p0->x) * (p2->y - p0->y) - (p2->x - p0->x) * (p1->y - p0->y);
            if (area == 0.0f || !std::isfinite(area)) continue;
            if (area < 0.0f) { std::swap(p1, p2); area = -area; }
            const float min_x = std::min({p0->x, p1->x, p2->x}), max_x = std::max({p0->x, p1->x, p2->x});
            const float min_y = std::min({p0->y, p1->y, p2->y}), max_y = std::max({p0->y, p1->y, p2->y});
            const std::int32_t bx0 = std::max(static_cast<std::int32_t>(std::floor(min_x)), x_min);
            const std::int32_t bx1 = std::min(static_cast<std::int32_t>(std::ceil(max_x)), x_max);
            const std::int32_t by0 = std::max(static_cast<std::int32_t>(std::floor(min_y)), ymin);
            const std::int32_t by1 = std::min(static_cast<std::int32_t>(std::ceil(max_y)), ymax);
            const float inv_area = 1.0f / area;
            // Top-left rule via a tiny bias on non-top-left edges.
            const auto edge_bias = [](const GpuVertex *a, const GpuVertex *b) {
                const bool top_left = (b->y == a->y && b->x < a->x) || (b->y < a->y);
                return top_left ? 0.0f : 1e-4f;
            };
            const float bias0 = edge_bias(p1, p2), bias1 = edge_bias(p2, p0), bias2 = edge_bias(p0, p1);
            for (std::int32_t py = by0; py <= by1; ++py) {
                for (std::int32_t px = bx0; px <= bx1; ++px) {
                    const float cx = px + 0.5f, cy = py + 0.5f;
                    const float w0 = (p2->x - p1->x) * (cy - p1->y) - (p2->y - p1->y) * (cx - p1->x);
                    const float w1 = (p0->x - p2->x) * (cy - p2->y) - (p0->y - p2->y) * (cx - p2->x);
                    const float w2 = (p1->x - p0->x) * (cy - p0->y) - (p1->y - p0->y) * (cx - p0->x);
                    if (w0 < bias0 || w1 < bias1 || w2 < bias2) continue;
                    const float l0 = w0 * inv_area, l1 = w1 * inv_area, l2 = w2 * inv_area;
                    const float iw = l0 * p0->inv_w + l1 * p1->inv_w + l2 * p2->inv_w;
                    Fragment f;
                    f.z = l0 * p0->z + l1 * p1->z + l2 * p2->z;
                    f.inv_w = iw;
                    if (iw != 1.0f || p0->inv_w != 1.0f) {
                        f.u = l0 * p0->u * p0->inv_w + l1 * p1->u * p1->inv_w + l2 * p2->u * p2->inv_w;
                        f.v = l0 * p0->v * p0->inv_w + l1 * p1->v * p1->inv_w + l2 * p2->v * p2->inv_w;
                        const float rw = 1.0f / iw;
                        f.r = (l0 * p0->r * p0->inv_w + l1 * p1->r * p1->inv_w + l2 * p2->r * p2->inv_w) * rw;
                        f.g = (l0 * p0->g * p0->inv_w + l1 * p1->g * p1->inv_w + l2 * p2->g * p2->inv_w) * rw;
                        f.b = (l0 * p0->b * p0->inv_w + l1 * p1->b * p1->inv_w + l2 * p2->b * p2->inv_w) * rw;
                        f.a = (l0 * p0->a * p0->inv_w + l1 * p1->a * p1->inv_w + l2 * p2->a * p2->inv_w) * rw;
                        f.fog = (l0 * p0->fog * p0->inv_w + l1 * p1->fog * p1->inv_w + l2 * p2->fog * p2->inv_w) * rw;
                    } else {
                        f.u = l0 * p0->u + l1 * p1->u + l2 * p2->u;
                        f.v = l0 * p0->v + l1 * p1->v + l2 * p2->v;
                        f.r = l0 * p0->r + l1 * p1->r + l2 * p2->r;
                        f.g = l0 * p0->g + l1 * p1->g + l2 * p2->g;
                        f.b = l0 * p0->b + l1 * p1->b + l2 * p2->b;
                        f.a = l0 * p0->a + l1 * p1->a + l2 * p2->a;
                        f.fog = l0 * p0->fog + l1 * p1->fog + l2 * p2->fog;
                    }
                    pipe.shade(px, py, f);
                }
            }
        }
    };

    constexpr std::uint64_t kThreadWorkThreshold = 12000u;
    if (pool.threads() > 1u && work >= kThreadWorkThreshold && (prim == GpuPrim::Sprites || prim == GpuPrim::Triangles) && y_max > y_min) {
        const unsigned bands = std::min<unsigned>(pool.threads() * 2u, static_cast<unsigned>(y_max - y_min + 1));
        std::vector<GpuDiag> band_diag(bands);
        std::vector<std::uint64_t> band_written(bands, 0u);
        const std::int32_t rows = y_max - y_min + 1;
        pool.run(bands, [&](unsigned band) {
            const std::int32_t lo = y_min + static_cast<std::int32_t>(static_cast<std::int64_t>(rows) * band / bands);
            const std::int32_t hi = y_min + static_cast<std::int32_t>(static_cast<std::int64_t>(rows) * (band + 1u) / bands) - 1;
            Pipeline pipe{d, target, tex, band_written[band], band_diag[band]};
            pipe.band_lo = lo; pipe.band_hi = hi;
            render(pipe, lo, hi);
        });
        for (unsigned band = 0; band < bands; ++band) {
            pixels_written_ += band_written[band];
            diag.purple_pixels += band_diag[band].purple_pixels;
            if (!diag.have_sample && band_diag[band].have_sample) {
                diag.have_sample = true;
                diag.sample_texel = band_diag[band].sample_texel;
                diag.sample_vr = band_diag[band].sample_vr; diag.sample_vg = band_diag[band].sample_vg; diag.sample_vb = band_diag[band].sample_vb;
                diag.sample_out_r = band_diag[band].sample_out_r; diag.sample_out_g = band_diag[band].sample_out_g; diag.sample_out_b = band_diag[band].sample_out_b;
                diag.sample_u = band_diag[band].sample_u; diag.sample_v = band_diag[band].sample_v;
            }
        }
        return;
    }
    const Pipeline pipe{d, target, tex, pixels_written_, diag};
    render(pipe, INT_MIN, INT_MAX);
}

std::uint32_t pixel_expand(std::uint32_t format, std::uint32_t packed) { return expand(format, packed); }
std::uint32_t pixel_compress(std::uint32_t format, std::uint32_t colour) { return compress(format, colour); }

bool decode_texture_rgba(const GpuDraw &d, const psprecomp::GuestMemory &memory, std::vector<std::uint32_t> &out) {
    const std::uint32_t bits = texel_bits(d.tex_format);
    if (bits == 0u || d.tex_width == 0u || d.tex_height == 0u) return false;
    const std::size_t bytes = static_cast<std::size_t>(std::max(d.tex_stride, d.tex_width)) * d.tex_height * bits / 8u;
    const std::uint8_t *p = memory.raw_pointer(d.tex_addr, bytes);
    if (p == nullptr) return false;
    Texture tex;
    tex.data = p;
    tex.valid = true;
    out.resize(static_cast<std::size_t>(d.tex_width) * d.tex_height);
    for (std::uint32_t y = 0; y < d.tex_height; ++y)
        for (std::uint32_t x = 0; x < d.tex_width; ++x)
            out[static_cast<std::size_t>(y) * d.tex_width + x] = fetch_texel(d, tex, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
    return true;
}

bool convert_frame_to_rgba(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t stride,
                           std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint8_t *rgba) {
    if (stride == 0u) return false;
    std::uint32_t a = address & 0x0FFFFFFFu;
    if ((a & 0x0F000000u) == 0u) a |= 0x04000000u;
    const std::size_t bytes = static_cast<std::size_t>(stride) * height * bytes_per_pixel(format);
    const std::uint8_t *src = memory.raw_pointer(a, bytes);
    if (src == nullptr) return false;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * stride + x) * bytes_per_pixel(format);
            std::uint32_t c;
            if (format == 3u) std::memcpy(&c, src + at, 4);
            else { std::uint16_t p; std::memcpy(&p, src + at, 2); c = expand(format, p); }
            std::uint8_t *o = rgba + (static_cast<std::size_t>(y) * width + x) * 4u;
            o[0] = static_cast<std::uint8_t>(c); o[1] = static_cast<std::uint8_t>(c >> 8u);
            o[2] = static_cast<std::uint8_t>(c >> 16u); o[3] = 255u;
        }
    }
    return true;
}

} // namespace rr
