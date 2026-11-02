#include "rr_texhd.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "rr_log.hpp"

namespace fs = std::filesystem;

namespace rr::texhd {
namespace {

struct Entry {
    std::uint64_t hash{};
    std::uint32_t w{}, h{};
    std::vector<std::uint32_t> rgba;    // full size
    std::vector<std::uint8_t> thumb;    // (w/2 x h/2) RGB box average, made on first use
};

std::mutex g_mu;
fs::path g_dir;
bool g_disk_scanned = false;
std::deque<Entry> g_entries;               // newest at the back
std::unordered_set<std::uint64_t> g_known; // hashes in memory or on disk
std::unordered_map<std::uint64_t, fs::path> g_on_disk;   // not loaded yet
std::size_t g_bytes = 0;
constexpr std::size_t kMaxBytes = 192u << 20;

std::uint64_t hash_of(const std::uint32_t *p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

bool candidate_size(std::uint32_t w, std::uint32_t h) { return w >= 64u && h >= 32u && w * h >= 128u * 64u && w <= 1024u && h <= 1024u; }

void make_thumb(Entry &e) {
    if (!e.thumb.empty()) return;
    const std::uint32_t tw = e.w / 2u, th = e.h / 2u;
    e.thumb.resize(static_cast<std::size_t>(tw) * th * 3u);
    for (std::uint32_t y = 0; y < th; ++y)
        for (std::uint32_t x = 0; x < tw; ++x) {
            unsigned s[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t c = e.rgba[static_cast<std::size_t>(2u * y + (k >> 1)) * e.w + 2u * x + (k & 1)];
                s[0] += c & 255u; s[1] += (c >> 8) & 255u; s[2] += (c >> 16) & 255u;
            }
            std::uint8_t *t = &e.thumb[(static_cast<std::size_t>(y) * tw + x) * 3u];
            t[0] = static_cast<std::uint8_t>(s[0] / 4u); t[1] = static_cast<std::uint8_t>(s[1] / 4u); t[2] = static_cast<std::uint8_t>(s[2] / 4u);
        }
}

void add_entry(Entry &&e) {
    g_bytes += e.rgba.size() * 4u;
    g_known.insert(e.hash);
    g_entries.push_back(std::move(e));
    while (g_bytes > kMaxBytes && g_entries.size() > 1u) {
        g_bytes -= g_entries.front().rgba.size() * 4u + g_entries.front().thumb.size();
        g_entries.pop_front();   // (still on disk if it was saved: loaded again when needed)
    }
}

// texcache/<w>x<h>_<hash16>.rgba : raw RGBA
void scan_disk() {
    if (g_disk_scanned || g_dir.empty()) return;
    g_disk_scanned = true;
    std::error_code ec;
    for (const auto &f : fs::directory_iterator(g_dir, ec)) {
        const std::string n = f.path().filename().string();
        unsigned w = 0, h = 0; unsigned long long hs = 0;
        if (std::sscanf(n.c_str(), "%ux%u_%16llx.rgba", &w, &h, &hs) == 3 && candidate_size(w, h)) { g_on_disk[hs] = f.path(); g_known.insert(hs); }
    }
}

void load_size(std::uint32_t w, std::uint32_t h) {   // everything of that size from disk into memory
    std::vector<std::uint64_t> done;
    for (const auto &[hs, p] : g_on_disk) {
        unsigned fw = 0, fh = 0; unsigned long long x = 0;
        if (std::sscanf(p.filename().string().c_str(), "%ux%u_%16llx.rgba", &fw, &fh, &x) != 3 || fw != w || fh != h) continue;
        bool in_mem = false;
        for (const Entry &e : g_entries) if (e.hash == hs) { in_mem = true; break; }
        done.push_back(hs);
        if (in_mem) continue;
        Entry e; e.hash = hs; e.w = w; e.h = h; e.rgba.resize(static_cast<std::size_t>(w) * h);
        std::ifstream in(p, std::ios::binary);
        if (!in.read(reinterpret_cast<char *>(e.rgba.data()), static_cast<std::streamsize>(e.rgba.size() * 4u))) continue;
        add_entry(std::move(e));
    }
    for (const std::uint64_t hs : done) g_on_disk.erase(hs);
}


// ---- the showroom textures straight from the disc (RRP.DAT) ----------------------------------------------------------
// RRP.DAT holds sector-aligned chunks, each a compressed stream padded with zeros in front so that it ends on a 2048-byte
// boundary (the game reads a chunk and decompresses from its first non-zero byte: 0x08806FE8).  Format: 16-bit tokens;
// t > 0xFEFF: (t & 0xFF) + 1 literal bytes follow; else copy (t & 0x1F) + 3 bytes from (t >> 5) + 1 back.
// Car models are ".RCO" files with a ".RTS" texture table (0x10-byte header, u16 count at +6, 0x20-byte entries: +0 id,
// +8 u16 width, +0xA u16 height, +0x10 data end, +0x18 data start, both relative to the entry; 8-bit indices, swizzled in
// 16x8-byte blocks) and a ".RCS" palette table (entries of 0x10 bytes: +0 texture id; 256 RGBA colours each after them).
struct FileTex {
    std::uint32_t w{}, h{}, rows{};            // w x h as drawn (power of two), rows actually stored
    std::vector<std::uint8_t> idx;             // w x rows, unswizzled
    std::uint32_t pal[256]{};
    bool own_pal{false};                       // pal came from the model's .RCS table (decal colours exact; 0..95 = paint, set at run time)
    std::string name;
};
std::vector<FileTex> g_files;
bool g_disc_started = false;
bool g_disc_done = false;
std::atomic<int> g_generation{0};   // bumped when the disc textures arrive (the renderer retries textures it tried before)
fs::path g_disc_path;

bool lz_decode(const std::uint8_t *d, std::size_t n, std::vector<std::uint8_t> &out, std::size_t max_out) {
    out.clear();
    std::size_t i = 0;
    while (i + 1 < n && out.size() < max_out) {
        const unsigned t = d[i] | (d[i + 1] << 8); i += 2;
        if (t > 0xFEFFu) {
            const std::size_t c = (t & 0xFFu) + 1u;
            if (i + c > n) return false;
            out.insert(out.end(), d + i, d + i + c); i += c;
        } else {
            const std::size_t len = (t & 0x1Fu) + 3u, dist = (t >> 5) + 1u;
            if (dist > out.size()) return false;
            for (std::size_t k = 0; k < len; ++k) out.push_back(out[out.size() - dist]);
        }
    }
    return true;
}
std::uint32_t rd32(const std::vector<std::uint8_t> &v, std::size_t o) { return o + 4 <= v.size() ? (v[o] | (v[o + 1] << 8) | (v[o + 2] << 16) | (static_cast<std::uint32_t>(v[o + 3]) << 24)) : 0u; }
std::uint32_t rd16(const std::vector<std::uint8_t> &v, std::size_t o) { return o + 2 <= v.size() ? static_cast<std::uint32_t>(v[o] | (v[o + 1] << 8)) : 0u; }
std::size_t find_magic(const std::vector<std::uint8_t> &v, const char *m) {
    for (std::size_t i = 0; i + 4 <= v.size(); i += 4) if (std::memcmp(&v[i], m, 4) == 0) return i;
    return std::string::npos;
}

void parse_rco(const std::vector<std::uint8_t> &o, std::uint64_t file_off, std::vector<FileTex> &out) {
    const std::size_t rts = find_magic(o, ".RTS");
    if (rts == std::string::npos) return;
    const std::size_t rcs = find_magic(o, ".RCS");
    const std::uint32_t n = rd16(o, rts + 6);
    // Only the car models (machine select): 4-5 textures, among them the 512x416 body and the 256x64 wheels.  Other models
    // (scenery) gave false matches on trees.
    {
        bool body = false, wheels = false;
        for (std::uint32_t i = 0; i < n && i < 64u; ++i) {
            const std::size_t e = rts + 0x10u + i * 0x20u;
            const std::uint32_t w = rd16(o, e + 8), h = rd16(o, e + 10);
            body |= w == 512u && h == 416u; wheels |= w == 256u && h == 64u;
        }
        if (n < 4u || n > 6u || !body || !wheels) return;
    }
    for (std::uint32_t i = 0; i < n && i < 64u; ++i) {
        const std::size_t e = rts + 0x10u + i * 0x20u;
        const std::uint32_t id = rd32(o, e), w = rd16(o, e + 8), rows = rd16(o, e + 10);
        const std::size_t start = e + rd32(o, e + 0x18), end = e + rd32(o, e + 0x10);
        if (w < 64u || rows < 32u || w > 1024u || rows > 1024u || (w & 15u) != 0u || (rows & 7u) != 0u) continue;
        if (end != start + static_cast<std::size_t>(w) * rows || end > o.size()) continue;   // (8-bit textures only)
        FileTex t;
        t.w = w; t.rows = rows; t.h = 1; while (t.h < rows) t.h <<= 1;
        t.idx.resize(static_cast<std::size_t>(w) * rows);
        const std::uint32_t bw = w / 16u;
        for (std::uint32_t b = 0; b < (w * rows) / 128u; ++b) {   // 16-byte x 8-row blocks
            const std::uint32_t by = b / bw, bx = b % bw;
            for (std::uint32_t y = 0; y < 8u; ++y) std::memcpy(&t.idx[static_cast<std::size_t>(by * 8u + y) * w + bx * 16u], &o[start + b * 128u + y * 16u], 16u);
        }
        for (std::uint32_t k = 0; k < 256u; ++k) t.pal[k] = 0xff000000u | (k * 0x010101u);   // (grey until a palette is found)
        if (rcs != std::string::npos) {
            const std::uint32_t pn = rd16(o, rcs + 6);
            for (std::uint32_t k = 0; k < pn && k < 64u; ++k)
                if ((rd32(o, rcs + 0x10u + k * 0x10u) & 0xffffu) == (id & 0xffffu)) {
                    const std::size_t pal = rcs + 0x10u + pn * 0x10u + k * 1024u;
                    if (pal + 1024u <= o.size()) { for (std::uint32_t c = 0; c < 256u; ++c) t.pal[c] = rd32(o, pal + c * 4u); t.own_pal = true; }
                }
        }
        char nm[64]; std::snprintf(nm, sizeof nm, "RRP.DAT+%llu tex %u (%ux%u)", static_cast<unsigned long long>(file_off), i, w, rows);
        t.name = nm;
        out.push_back(std::move(t));
    }
}

void load_disc(fs::path path) {
    std::vector<FileTex> found;
    std::ifstream f(path, std::ios::binary);
    if (f) {
        f.seekg(0, std::ios::end);
        const std::uint64_t size = static_cast<std::uint64_t>(f.tellg());
        // chunk starts: sectors that begin with 32 zero bytes right after a non-zero byte
        std::vector<std::uint64_t> starts;
        std::vector<char> buf(1u << 20);
        char prev = 1;
        for (std::uint64_t base = 0; base < size; base += buf.size()) {
            f.seekg(static_cast<std::streamoff>(base));
            const std::size_t got = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - base));
            f.read(buf.data(), static_cast<std::streamsize>(got));
            for (std::size_t s = 0; s + 32u <= got; s += 2048u) {
                bool zero = true;
                for (int k = 0; k < 32 && zero; ++k) zero = buf[s + k] == 0;
                const char before = s == 0 ? prev : buf[s - 1];
                if (zero && (base + s == 0 || before != 0)) starts.push_back(base + s);
            }
            if (got > 0) prev = buf[got - 1];
        }
        starts.push_back(size);
        std::vector<std::uint8_t> chunk, out;
        for (std::size_t c = 0; c + 1 < starts.size(); ++c) {
            const std::uint64_t a = starts[c], b = starts[c + 1];
            if (b - a > (16u << 20)) continue;
            // the first sectors: skip the zero padding and look for ".RCO"
            chunk.resize(static_cast<std::size_t>(std::min<std::uint64_t>(b - a, 8192u)));
            f.clear(); f.seekg(static_cast<std::streamoff>(a)); f.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            std::size_t s = 0; while (s < chunk.size() && chunk[s] == 0) ++s;
            if (s + 8 > chunk.size() || !lz_decode(&chunk[s], chunk.size() - s, out, 8) || out.size() < 4 || std::memcmp(out.data(), ".RCO", 4) != 0) continue;
            chunk.resize(static_cast<std::size_t>(b - a));
            f.clear(); f.seekg(static_cast<std::streamoff>(a)); f.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            if (!lz_decode(&chunk[s], chunk.size() - s, out, 32u << 20)) continue;
            parse_rco(out, a, found);
        }
    }
    std::size_t big = 0; for (const FileTex &t : found) big += t.w >= 256u;
    log_line(category::kHle, "TEXHD read " + std::to_string(found.size()) + " model textures (" + std::to_string(big) + " of 256+ wide) from " + path.string());
    std::lock_guard<std::mutex> lk(g_mu);
    g_files = std::move(found);
    g_disc_done = true;
    ++g_generation;
}

// A disc texture at twice the size of a race texture, coloured like it: each palette index gets the average race colour
// where a 2x2 block of the big texture is all that index (this carries the car's paint colour and livery palette over).
int g_reject_reason = 0;   // (diagnostics) why decode_like said no: 1 too few learned, 2 inconsistent
double g_reject_cons = 0.0;
char g_diag[200] = {};
bool decode_like(const FileTex &t, std::uint32_t F, const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h, std::vector<std::uint32_t> &hi) {
    double sum[256][4] = {};
    std::uint32_t cnt[256] = {};
    const std::uint32_t rowsF = std::min(t.rows / F, h);
    const auto pure_at = [&](std::uint32_t x, std::uint32_t y, std::uint8_t &k) {
        const std::uint8_t *p = &t.idx[static_cast<std::size_t>(F * y) * t.w + F * x];
        k = p[0];
        for (std::uint32_t yy = 0; yy < F; ++yy)
            for (std::uint32_t xx = 0; xx < F; ++xx) if (p[static_cast<std::size_t>(yy) * t.w + xx] != k) return false;
        return true;
    };
    for (std::uint32_t y = 0; y < rowsF; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t k = 0;
            if (!pure_at(x, y, k)) continue;
            const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x];
            for (int ch = 0; ch < 4; ++ch) sum[k][ch] += (c >> (8 * ch)) & 255u;
            ++cnt[k];
        }
    // Held-out test: colours learned from half of the blocks (checkerboard) must predict the other half.  A real downscale
    // shares big areas per palette index and passes; an unrelated texture fitted with 256 free colours (a noisy interior atlas
    // against a tree) does not.
    {
        double se[256][3] = {}; std::uint32_t ce[256] = {};
        for (std::uint32_t y = 0; y < rowsF; ++y)
            for (std::uint32_t x = (y & 1u); x < w; x += 2u) {
                std::uint8_t k = 0;
                if (!pure_at(x, y, k)) continue;
                const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x];
                for (int ch = 0; ch < 3; ++ch) se[k][ch] += (c >> (8 * ch)) & 255u;
                ++ce[k];
            }
        double mean[3] = {0, 0, 0}; std::uint64_t n = 0;
        for (std::uint32_t y = 0; y < rowsF; ++y)
            for (std::uint32_t x = 1u - (y & 1u); x < w; x += 2u) {
                std::uint8_t k = 0;
                if (!pure_at(x, y, k)) continue;
                const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x];
                for (int ch = 0; ch < 3; ++ch) mean[ch] += (c >> (8 * ch)) & 255u;
                ++n;
            }
        if (n < 64u) { g_reject_reason = 5; g_reject_cons = 0.0; return false; }
        for (double &m : mean) m /= static_cast<double>(n);
        std::uint64_t seen = 0; double resid = 0.0, spread = 0.0;
        for (std::uint32_t y = 0; y < rowsF; ++y)
            for (std::uint32_t x = 1u - (y & 1u); x < w; x += 2u) {
                std::uint8_t k = 0;
                if (!pure_at(x, y, k)) continue;
                const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x];
                for (int ch = 0; ch < 3; ++ch) spread += std::fabs(static_cast<double>((c >> (8 * ch)) & 255u) - mean[ch]);
                if (ce[k] == 0u) { spread -= 0.0; continue; }
                ++seen;
                for (int ch = 0; ch < 3; ++ch) resid += std::fabs(static_cast<double>((c >> (8 * ch)) & 255u) - se[k][ch] / ce[k]);
            }
        const double seen_frac = static_cast<double>(seen) / n;
        const double ratio = (seen > 0u && spread > 0.0) ? (resid / seen) / (spread / n) : 1.0;
        g_reject_cons = seen_frac;
        std::snprintf(g_diag, sizeof g_diag, "heldout seen=%.2f ratio=%.2f n=%llu", seen_frac, ratio, static_cast<unsigned long long>(n));
        static const bool no_held = std::getenv("RR_TEXHD_NOHELD") != nullptr;
        if (!no_held && (seen_frac < 0.7 || ratio > 0.3)) { g_reject_reason = 6; return false; }
    }
    std::uint32_t pure_idx = 0; for (int k = 0; k < 256; ++k) pure_idx += cnt[k] != 0u ? 1u : 0u;
    // indices that never fill a whole block: learn them from blocks where they are the majority (more than half)
    {
        double sum2[256][4] = {}, sum3[256][4] = {}, wt3[256] = {};
        std::uint32_t cnt2[256] = {};
        for (std::uint32_t y = 0; y < rowsF; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t hist_k[16]; std::uint32_t hist_n[16]; int nh = 0;
                for (std::uint32_t yy = 0; yy < F; ++yy)
                    for (std::uint32_t xx = 0; xx < F; ++xx) {
                        const std::uint8_t k = t.idx[static_cast<std::size_t>(F * y + yy) * t.w + F * x + xx];
                        int j = 0; while (j < nh && hist_k[j] != k) ++j;
                        if (j == nh) { hist_k[nh] = k; hist_n[nh] = 0; ++nh; }
                        ++hist_n[j];
                    }
                for (int j = 0; j < nh; ++j) {
                    if (cnt[hist_k[j]] != 0u) continue;
                    const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x];
                    if (hist_n[j] * 2u > F * F) {
                        for (int ch = 0; ch < 4; ++ch) sum2[hist_k[j]][ch] += (c >> (8 * ch)) & 255u;
                        ++cnt2[hist_k[j]];
                    } else {   // (last resort: any block holding it, weighted by its share)
                        const double wgt = static_cast<double>(hist_n[j]) / (F * F);
                        for (int ch = 0; ch < 4; ++ch) sum3[hist_k[j]][ch] += wgt * ((c >> (8 * ch)) & 255u);
                        wt3[hist_k[j]] += wgt;
                    }
                }
            }
        for (int k = 0; k < 256; ++k) {
            if (cnt[k] != 0u) continue;
            if (cnt2[k] != 0u) { for (int ch = 0; ch < 4; ++ch) sum[k][ch] = sum2[k][ch]; cnt[k] = cnt2[k]; }
            else if (wt3[k] > 0.0) { for (int ch = 0; ch < 4; ++ch) sum[k][ch] = sum3[k][ch] / wt3[k]; cnt[k] = 1u; }
        }
    }
    std::uint32_t learned = 0, pal[256];
    for (int k = 0; k < 256; ++k) {
        if (cnt[k] == 0u) { pal[k] = t.pal[k]; continue; }
        ++learned;
        std::uint32_t c = 0;
        for (int ch = 0; ch < 4; ++ch) c |= static_cast<std::uint32_t>(std::lround(sum[k][ch] / cnt[k])) << (8 * ch);
        pal[k] = c;
    }
    if (learned < 8u) { g_reject_reason = 1; return false; }
    // A real match is consistent: the texels under one index all have about the same colour.  Fitting 256 free colours to an
    // unrelated texture leaves them scattered (true match ~97 % within 16, unrelated ones ~70 %).
    std::uint64_t pure = 0, close = 0;
    double mean[3] = {0, 0, 0}, resid = 0.0, spread = 0.0;
    std::uint64_t pure_norm = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (std::uint32_t y = 0; y < rowsF; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t k = 0;
                if (!pure_at(x, y, k)) continue;
                const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x], m = pal[k];
                if (pass == 0) { for (int ch = 0; ch < 3; ++ch) mean[ch] += (c >> (8 * ch)) & 255u; ++pure; continue; }
                if (pure_norm == 0u) { pure_norm = pure; for (int ch = 0; ch < 3; ++ch) mean[ch] /= static_cast<double>(pure); }
                int d = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    const int v = static_cast<int>((c >> (8 * ch)) & 255u);
                    d += std::abs(v - static_cast<int>((m >> (8 * ch)) & 255u));
                    spread += std::fabs(v - mean[ch]);
                }
                resid += d; close += d < 48 ? 1u : 0u;
            }
        // (pass 0 counted the blocks and summed the colours)
    // explained structure: the residual must be small next to the texture's own spread of colours; low-contrast textures
    // (dark trees against a dark interior atlas) otherwise look "consistent" by default
    const bool flat_fit = pure > 0u && resid > 0.3 * spread;
    // Too few blocks per colour means 256 free colours can fit almost anything (an atlas of tiny noisy pieces matched trees).
    if (pure < 6u * pure_idx) { g_reject_reason = 3; g_reject_cons = 0.0; return false; }
    (void)flat_fit;
    {
        const size_t l = std::strlen(g_diag);
        std::snprintf(g_diag + l, sizeof g_diag - l, " pure=%llu idx=%u close=%.2f fit=%.2f", static_cast<unsigned long long>(pure), pure_idx, pure ? static_cast<double>(close) / pure : 0.0, spread > 0 ? resid / spread : 0.0);
    }
    if (pure < 64u || close < pure * 4u / 5u) { g_reject_reason = 2; g_reject_cons = pure ? static_cast<double>(close) / pure : 0.0; return false; }
    // Decal colours (palette 96..255) straight from the model: exact, where the averages learned from the soft race texture
    // come out greyish (white plates).  The paint ramp (0..95) is the car's colour, filled in by the game: learned.
    // ...but only where the file colour explains the race texture at least as well as the learned one (some decals carry
    // other colours in the race palette: the file's would turn white text on black grey on grey).
    if (t.own_pal) {
        static double err_l[256], err_f[256];
        std::fill(err_l, err_l + 256, 0.0); std::fill(err_f, err_f + 256, 0.0);
        for (std::uint32_t y = 0; y < rowsF; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t hk[16]; std::uint32_t hn[16]; int nh = 0;
                double base[3] = {0, 0, 0};
                for (std::uint32_t yy = 0; yy < F; ++yy)
                    for (std::uint32_t xx = 0; xx < F; ++xx) {
                        const std::uint8_t k = t.idx[static_cast<std::size_t>(F * y + yy) * t.w + F * x + xx];
                        for (int ch = 0; ch < 3; ++ch) base[ch] += (pal[k] >> (8 * ch)) & 255u;
                        int j = 0; while (j < nh && hk[j] != k) ++j;
                        if (j == nh) { if (nh == 16) continue; hk[nh] = k; hn[nh] = 0; ++nh; }
                        ++hn[j];
                    }
                const std::uint32_t c = rgba[static_cast<std::size_t>(y) * w + x];
                const double n = static_cast<double>(F * F);
                for (int j = 0; j < nh; ++j) {
                    const std::uint8_t k = hk[j];
                    if (k < 96u || hn[j] * 4u < F * F) continue;
                    double el = 0.0, ef = 0.0;
                    for (int ch = 0; ch < 3; ++ch) {
                        const double v = static_cast<double>((c >> (8 * ch)) & 255u);
                        const double pl = base[ch] / n;
                        const double pf = (base[ch] - hn[j] * static_cast<double>((pal[k] >> (8 * ch)) & 255u) + hn[j] * static_cast<double>((t.pal[k] >> (8 * ch)) & 255u)) / n;
                        el += std::fabs(v - pl); ef += std::fabs(v - pf);
                    }
                    const double wgt = hn[j] / n;
                    err_l[k] += wgt * el; err_f[k] += wgt * ef;
                }
            }
        for (int k = 96; k < 256; ++k)
            if (cnt[k] == 0u || err_f[k] <= err_l[k]) pal[k] = t.pal[k];
    }
    const std::uint32_t W = F * w;
    hi.assign(static_cast<std::size_t>(W) * F * h, 0u);
    for (std::uint32_t y = 0; y < t.rows && y < F * h; ++y)
        for (std::uint32_t x = 0; x < W; ++x) hi[static_cast<std::size_t>(y) * W + x] = pal[t.idx[static_cast<std::size_t>(y) * t.w + x]];
    return true;
}

} // namespace

void preload_disc(const fs::path &rrp_dat) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_disc_path = rrp_dat;
    // read it now, in the background: ready long before the first race
    if (!g_disc_started && !g_disc_path.empty()) { g_disc_started = true; std::thread(load_disc, g_disc_path).detach(); }
}

int generation() { return g_generation.load(); }

void set_cache_dir(const fs::path &dir) { std::lock_guard<std::mutex> lk(g_mu); g_dir = dir; }

void offer(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    if (rgba == nullptr || !candidate_size(w, h)) return;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const std::uint64_t hs = hash_of(rgba, n);
    std::lock_guard<std::mutex> lk(g_mu);
    scan_disk();
    if (g_known.count(hs) != 0u) return;
    Entry e; e.hash = hs; e.w = w; e.h = h; e.rgba.assign(rgba, rgba + n);
    if (!g_dir.empty() && g_known.size() < 1500u) {   // keep it for later sessions (written in the background; at most ~1500 files)
        std::error_code ec;
        fs::create_directories(g_dir, ec);
        char nm[64]; std::snprintf(nm, sizeof nm, "%ux%u_%016llx.rgba", w, h, static_cast<unsigned long long>(hs));
        std::thread([p = g_dir / nm, data = e.rgba] {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size() * 4u));
        }).detach();
    }
    add_entry(std::move(e));
}


bool upgrade(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h, std::vector<std::uint32_t> &out, std::uint32_t &out_w, std::uint32_t &out_h, std::string &why) {
    if (rgba == nullptr || w < 16u || h < 16u || (w & 7u) != 0u || (h & 7u) != 0u) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_disc_started && !g_disc_path.empty()) { g_disc_started = true; std::thread(load_disc, g_disc_path).detach(); }
    // The renderer uploads a texture again after it was purged or its palette changed back: remember every answer (by the
    // texels; reset when the disc textures arrive, since they can turn a "no" into a "yes").
    struct Memo { std::vector<std::uint32_t> img; std::uint32_t w{}, h{}; std::string why; std::uint64_t used{}; };
    static std::unordered_map<std::uint64_t, Memo> memo;
    static std::uint64_t memo_clock = 0;
    static bool memo_had_disc = false;
    if (memo_had_disc != g_disc_done) { memo.clear(); memo_had_disc = g_disc_done; }
    const std::uint64_t key = hash_of(rgba, static_cast<std::size_t>(w) * h) ^ (static_cast<std::uint64_t>(w) << 48) ^ (static_cast<std::uint64_t>(h) << 32);
    if (auto it = memo.find(key); it != memo.end()) {
        it->second.used = ++memo_clock;
        if (it->second.img.empty()) return false;
        out = it->second.img; out_w = it->second.w; out_h = it->second.h; why.clear();
        return true;
    }
    const auto remember = [&](bool ok) {
        if (memo.size() >= 96u) {   // drop the least recently used
            auto victim = memo.begin();
            for (auto it = memo.begin(); it != memo.end(); ++it) if (it->second.used < victim->second.used) victim = it;
            memo.erase(victim);
        }
        Memo m; m.used = ++memo_clock;
        if (ok) { m.img = out; m.w = out_w; m.h = out_h; m.why = why; }
        memo[key] = std::move(m);
        return ok;
    };
    scan_disk();
    if (!g_on_disk.empty()) load_size(2u * w, 2u * h);
    const std::uint32_t tx = w / 8u, ty = h / 8u;
    std::vector<std::uint8_t> detail(static_cast<std::size_t>(tx) * ty, 0u);
    for (std::uint32_t by = 0; by < ty; ++by)
        for (std::uint32_t bx = 0; bx < tx; ++bx) {
            float s[3] = {0, 0, 0}, q[3] = {0, 0, 0};
            for (std::uint32_t y = 0; y < 8u; ++y)
                for (std::uint32_t x = 0; x < 8u; ++x) {
                    const std::uint32_t c = rgba[static_cast<std::size_t>(by * 8u + y) * w + bx * 8u + x];
                    for (int k = 0; k < 3; ++k) { const float v = static_cast<float>((c >> (8 * k)) & 255u); s[k] += v; q[k] += v * v; }
                }
            float var = 0.0f;
            for (int k = 0; k < 3; ++k) var += q[k] / 64.0f - (s[k] / 64.0f) * (s[k] / 64.0f);
            detail[static_cast<std::size_t>(by) * tx + bx] = var > 3.0f * 36.0f ? 1u : 0u;
        }
    // tile match of a big candidate (Fw x Fh RGBA) against the small texture: FxF box averages vs texels
    const auto score = [&](const std::uint32_t *big, std::uint32_t F, std::vector<std::uint8_t> &match, int &det) {
        match.assign(static_cast<std::size_t>(tx) * ty, 0u);
        det = 0; int ok = 0;
        const std::uint32_t W = F * w;
        for (std::uint32_t by = 0; by < ty; ++by)
            for (std::uint32_t bx = 0; bx < tx; ++bx) {
                double d = 0.0;
                for (std::uint32_t y = 0; y < 8u; ++y)
                    for (std::uint32_t x = 0; x < 8u; ++x) {
                        const std::uint32_t X = bx * 8u + x, Y = by * 8u + y;
                        const std::uint32_t c = rgba[static_cast<std::size_t>(Y) * w + X];
                        const std::uint32_t *b = &big[static_cast<std::size_t>(F * Y) * W + F * X];
                        for (int k = 0; k < 3; ++k) {
                            const int sh = 8 * k;
                            std::uint32_t acc = 0;
                            for (std::uint32_t yy = 0; yy < F; ++yy) for (std::uint32_t xx = 0; xx < F; ++xx) acc += (b[static_cast<std::size_t>(yy) * W + xx] >> sh) & 255u;
                            d += std::abs(static_cast<int>((c >> sh) & 255u) - static_cast<int>(acc / (F * F)));
                        }
                    }
                d /= 64.0 * 3.0;
                const std::size_t i = static_cast<std::size_t>(by) * tx + bx;
                match[i] = d < 22.0 ? 1u : 0u;
                if (detail[i]) { ++det; ok += d < 32.0 ? 1 : 0; }
            }
        return det > 0 ? static_cast<double>(ok) / det : 0.0;
    };
    std::vector<std::uint32_t> best_img, tmp;
    std::vector<std::uint8_t> best_match, match;
    double best_frac = 0.0; int best_det = 0; std::string best_name; std::uint32_t best_F = 2, best_rows = 0;
    const auto consider = [&](const std::uint32_t *big, std::uint32_t F, const std::string &name, std::vector<std::uint32_t> *own, std::uint32_t rows) {
        int det = 0;
        const double frac = score(big, F, match, det);
        if (det >= 4 && frac > best_frac + (F > best_F ? 0.0 : 0.0)) {
            best_frac = frac; best_det = det; best_name = name; best_match.swap(match); best_F = F; best_rows = rows;
            if (own != nullptr) best_img.swap(*own); else best_img.assign(big, big + static_cast<std::size_t>(F * F) * w * h);
        }
    };
    for (Entry &e : g_entries) {
        if (e.w != 2u * w || e.h != 2u * h) continue;
        char nm[40]; std::snprintf(nm, sizeof nm, "cache %016llx", static_cast<unsigned long long>(e.hash));
        consider(e.rgba.data(), 2u, nm, nullptr, 0u);
    }
    static const bool diag = std::getenv("RR_TEXHD_DIAG") != nullptr;
    int n_cand = 0; double best_cons_rej = 0.0; std::string best_rej_name;
    for (const std::uint32_t F : {2u, 4u})
        for (const FileTex &t : g_files) {
            if (t.w != F * w || t.h != F * h) continue;
            ++n_cand;
            g_diag[0] = 0; g_reject_reason = 0;
            const bool dec = decode_like(t, F, rgba, w, h, tmp);
            if (diag && w >= 64u) {
                double fr = 0.0; int det = 0; std::vector<std::uint8_t> mm;
                if (dec) fr = score(tmp.data(), F, mm, det);
                char b[400]; std::snprintf(b, sizeof b, "TEXHD diag %ux%u vs %s F%u: %s reason=%d %s tiles=%.2f/%d", w, h, t.name.c_str(), F, dec ? "decoded" : "rejected", g_reject_reason, g_diag, fr, det);
                log_line(category::kGe, b);
            }
            if (!dec) { if (g_reject_cons > best_cons_rej) { best_cons_rej = g_reject_cons; best_rej_name = t.name; } continue; }
            consider(tmp.data(), F, t.name, &tmp, t.rows);
        }
    if (best_img.empty() || best_frac < 0.6) {
        static int told = 0;
        if (n_cand > 0 && told < 60) {
            ++told;
            char b[300];
            std::snprintf(b, sizeof b, "TEXHD no match for %ux%u: %d candidates, best tile match %.0f%% (%s), best rejected consistency %.0f%% (%s)", w, h, n_cand,
                          best_frac * 100.0, best_name.c_str(), best_cons_rej * 100.0, best_rej_name.c_str());
            log_line(category::kGe, b);
        }
        return remember(false);
    }
    const std::uint32_t F = best_F, W = F * w;
    if (const char *raw = std::getenv("RR_TEXHD_RAW"); raw != nullptr) {   // (tests) the decoded showroom texture and the race texture
        char nm[300];
        std::snprintf(nm, sizeof nm, "%s/big_%ux%u.rgba", raw, W, F * h);
        std::ofstream(nm, std::ios::binary).write(reinterpret_cast<const char *>(best_img.data()), static_cast<std::streamsize>(best_img.size() * 4u));
        std::snprintf(nm, sizeof nm, "%s/small_%ux%u.rgba", raw, w, h);
        std::ofstream(nm, std::ios::binary).write(reinterpret_cast<const char *>(rgba), static_cast<std::streamsize>(static_cast<std::size_t>(w) * h * 4u));
    }
    std::vector<std::uint8_t> bad(best_match.size(), 0u), offs(best_match.size(), 0u);
    // The candidate is confirmed: take every tile that is close (looser than the test above; ~97 % of a body's tiles), the rest
    // (parts the race texture holds and the showroom one does not) enlarged smoothly from the race texture.
    {
        int det = 0;
        std::vector<std::uint8_t> m2;
        (void)score(best_img.data(), F, m2, det);
        // re-run with the loose threshold
        for (std::uint32_t by = 0; by < ty; ++by)
            for (std::uint32_t bx = 0; bx < tx; ++bx) {
                double d = 0.0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
                int big_off = 0;   // areas far off (a different shape there: the race model's own part, e.g. a light)
                int sub_a[16][3] = {}, sub_b[16][3] = {};
                for (std::uint32_t y = 0; y < 8u; ++y)
                    for (std::uint32_t x = 0; x < 8u; ++x) {
                        const std::uint32_t X = bx * 8u + x, Y = by * 8u + y;
                        const std::uint32_t c = rgba[static_cast<std::size_t>(Y) * w + X];
                        const std::uint32_t *bp = &best_img[static_cast<std::size_t>(F * Y) * W + F * X];
                        double la = 0, lb = 0;
                        for (int k = 0; k < 3; ++k) {
                            const int sh = 8 * k; std::uint32_t acc = 0;
                            for (std::uint32_t yy = 0; yy < F; ++yy) for (std::uint32_t xx = 0; xx < F; ++xx) acc += (bp[static_cast<std::size_t>(yy) * W + xx] >> sh) & 255u;
                            const int va = static_cast<int>((c >> sh) & 255u), vb = static_cast<int>(acc / (F * F));
                            d += std::abs(va - vb); la += va; lb += vb;
                        }
                        for (int k = 0; k < 3; ++k) {   // (2x2-texel sub-blocks: sharp text differs texel by texel after scaling, a different shape over areas)
                            const int sh = 8 * k; std::uint32_t acc = 0;
                            for (std::uint32_t yy = 0; yy < F; ++yy) for (std::uint32_t xx = 0; xx < F; ++xx) acc += (bp[static_cast<std::size_t>(yy) * W + xx] >> sh) & 255u;
                            sub_a[(y / 2u) * 4u + x / 2u][k] += static_cast<int>((c >> sh) & 255u);
                            sub_b[(y / 2u) * 4u + x / 2u][k] += static_cast<int>(acc / (F * F));
                        }
                        sa += la; sb += lb; saa += la * la; sbb += lb * lb; sab += la * lb;
                    }
                d /= 64.0 * 3.0;
                // high-contrast decals (black text on white) differ more texel by texel after scaling, but keep their shape:
                // accept those by correlation
                const double cov = sab / 64.0 - (sa / 64.0) * (sb / 64.0), va = saa / 64.0 - (sa / 64.0) * (sa / 64.0), vb = sbb / 64.0 - (sb / 64.0) * (sb / 64.0);
                const double corr = (va > 1e-6 && vb > 1e-6) ? cov / std::sqrt(va * vb) : 0.0;
                const bool contrasty = va > 9.0 * 30.0 * 30.0;   // (luminance sum of 3 channels: std above ~30 per channel)
                // Default to the showroom texture; leave out only tiles that clearly differ (a decal or part the showroom livery
                // does not have there): far off in colour and not the same shape.
                (void)contrasty;
                // Compared over 4x4-texel quarters: the race decals are often redrawn by hand at race size (the letters do not
                // match texel by texel, the shapes do), while a different part (a light's outline) differs over a whole area.
                for (int cy = 0; cy < 2; ++cy)
                    for (int cx = 0; cx < 2; ++cx) {
                        int dd = 0;
                        for (int k = 0; k < 3; ++k) {
                            int a = 0, b2 = 0;
                            for (int qy = 0; qy < 2; ++qy) for (int qx = 0; qx < 2; ++qx) { const int q = (cy * 2 + qy) * 4 + cx * 2 + qx; a += sub_a[q][k]; b2 += sub_b[q][k]; }
                            dd += std::abs(a - b2);
                        }
                        big_off = std::max(big_off, dd / 16);
                    }
                const std::size_t ti = static_cast<std::size_t>(by) * tx + bx;
                offs[ti] = big_off >= 50 ? 1u : 0u;
                if (big_off > 100) { bad[ti] = 1u; best_match[ti] = 0u; }
                else if (!(d > 36.0 && corr < 0.6)) best_match[ti] = 1u;
            }
    }
    // A tile that is somewhat off next to a differing tile is the edge of that same part (a light's outline): leave it out too.
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<std::uint8_t> was = bad;
        for (std::uint32_t by = 0; by < ty; ++by)
            for (std::uint32_t bx = 0; bx < tx; ++bx) {
                const std::size_t i = static_cast<std::size_t>(by) * tx + bx;
                if (was[i] || offs[i] == 0u) continue;
                const bool nb = (bx > 0 && was[i - 1]) || (bx + 1 < tx && was[i + 1]) || (by > 0 && was[i - tx]) || (by + 1 < ty && was[i + tx]);
                if (nb) { bad[i] = 1u; best_match[i] = 0u; }
            }
    }
    // Parts of the race texture with no showroom counterpart (the strip below the body: tail lights, badges, grille, packed
    // by the game at race size) are enlarged with a Catmull-Rom filter (sharper than bilinear, no blocks).
    const auto smooth = [&](std::uint32_t X, std::uint32_t Y) {
        const float fx = (static_cast<float>(X) + 0.5f) / F - 0.5f, fy = (static_cast<float>(Y) + 0.5f) / F - 0.5f;
        const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
        const float ax = fx - static_cast<float>(ix), ay = fy - static_cast<float>(iy);
        const auto cr = [](float t, float k[4]) {
            const float t2 = t * t, t3 = t2 * t;
            k[0] = -0.5f * t3 + t2 - 0.5f * t; k[1] = 1.5f * t3 - 2.5f * t2 + 1.0f; k[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t; k[3] = 0.5f * t3 - 0.5f * t2;
        };
        float kx[4], ky[4]; cr(ax, kx); cr(ay, ky);
        float acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < 4; ++j) {
            const int yy = std::clamp(iy - 1 + j, 0, static_cast<int>(h) - 1);
            for (int i = 0; i < 4; ++i) {
                const int xx = std::clamp(ix - 1 + i, 0, static_cast<int>(w) - 1);
                const std::uint32_t c = rgba[static_cast<std::size_t>(yy) * w + xx];
                const float wgt = kx[i] * ky[j];
                for (int k = 0; k < 4; ++k) acc[k] += wgt * static_cast<float>((c >> (8 * k)) & 255u);
            }
        }
        std::uint32_t r = 0;
        for (int k = 0; k < 4; ++k) r |= static_cast<std::uint32_t>(std::clamp(static_cast<int>(acc[k] + 0.5f), 0, 255)) << (8 * k);
        return r;
    };
    // Tiles to take: close to the race texture (the loose test above) and inside the body rows.
    std::vector<std::uint8_t> keep(best_match.size(), 0u);
    const std::uint32_t body_ty = best_rows != 0u ? std::min(ty, best_rows / (8u * F)) : ty;
    for (std::uint32_t by = 0; by < body_ty; ++by)
        for (std::uint32_t bx = 0; bx < tx; ++bx) {
            // a tile is taken when it matches, or when it sits between matching tiles (left+right or above+below): those are
            // scaling noise inside one decal, not a different decal
            const auto mt = [&](int yy, int xx) { return yy >= 0 && xx >= 0 && yy < static_cast<int>(body_ty) && xx < static_cast<int>(tx) && best_match[static_cast<std::size_t>(yy) * tx + xx] != 0u; };
            const int iy = static_cast<int>(by), ix = static_cast<int>(bx);
            const bool ok = mt(iy, ix) || (!bad[static_cast<std::size_t>(by) * tx + bx] && ((mt(iy, ix - 1) && mt(iy, ix + 1)) || (mt(iy - 1, ix) && mt(iy + 1, ix))));
            keep[static_cast<std::size_t>(by) * tx + bx] = ok ? 1u : 0u;
        }
    out.resize(static_cast<std::size_t>(W) * F * h);
    for (std::uint32_t y = 0; y < F * h; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            // The body area of a confirmed showroom texture is taken whole (tile-by-tile choices cut badges in half); below it,
            // and for in-memory candidates, only the matching tiles.
            const bool m = keep[static_cast<std::size_t>(y / (8u * F)) * tx + x / (8u * F)] != 0u;
            static const bool tint = std::getenv("RR_TEXHD_TINT") != nullptr;   // (tests) smoothed parts in magenta
            out[static_cast<std::size_t>(y) * W + x] = m ? best_img[static_cast<std::size_t>(y) * W + x] : (tint ? ((smooth(x, y) & 0xFF00FF00u) | 0x00FF00FFu) : smooth(x, y));
        }
    out_w = W; out_h = F * h;
    std::size_t used = 0; for (const auto b : keep) used += b;
    std::size_t n_bad = 0; for (const auto b : bad) n_bad += b;
    char b[260];
    std::snprintf(b, sizeof b, "TEXHD %ux%u -> %ux%u from %s (%.0f%% of %d detailed tiles match, %zu of %zu tiles taken, %zu differ)", w, h, W, F * h, best_name.c_str(),
                  best_frac * 100.0, best_det, used, best_match.size(), n_bad);
    why = b;
    return remember(true);
}

} // namespace rr::texhd
