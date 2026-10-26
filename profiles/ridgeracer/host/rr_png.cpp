#include "rr_png.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace rr {
namespace {

// ---------------------------------------------------------------- CRC / Adler

const std::uint32_t *crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256u; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1u) : c >> 1u;
            t[n] = c;
        }
        return t;
    }();
    return table.data();
}
std::uint32_t crc(const std::uint8_t *data, std::size_t n, std::uint32_t c = 0xFFFFFFFFu) {
    const std::uint32_t *t = crc_table();
    for (std::size_t i = 0; i < n; ++i) c = t[(c ^ data[i]) & 255u] ^ (c >> 8u);
    return c;
}
std::uint32_t adler32(const std::uint8_t *data, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    while (n > 0) {
        const std::size_t k = std::min<std::size_t>(n, 5552u);
        for (std::size_t i = 0; i < k; ++i) { a += data[i]; b += a; }
        a %= 65521u; b %= 65521u;
        data += k; n -= k;
    }
    return (b << 16u) | a;
}
void put32(std::vector<std::uint8_t> &v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24u)); v.push_back(static_cast<std::uint8_t>(x >> 16u));
    v.push_back(static_cast<std::uint8_t>(x >> 8u)); v.push_back(static_cast<std::uint8_t>(x));
}
void chunk(std::vector<std::uint8_t> &out, const char *type, const std::vector<std::uint8_t> &data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

// ------------------------------------------------------------------- Deflate
// A small RFC 1951 compressor: hash-chain LZ77 with one step of lazy matching, then dynamic Huffman blocks.  It is
// slower than zlib -1 but much smaller than "stored" blocks, and it runs on the background screenshot thread.

struct BitWriter {
    std::vector<std::uint8_t> &out;
    std::uint64_t acc{0};
    int n{0};
    explicit BitWriter(std::vector<std::uint8_t> &o) : out(o) {}
    void put(std::uint32_t value, int bits) {
        acc |= static_cast<std::uint64_t>(value) << n;
        n += bits;
        while (n >= 8) { out.push_back(static_cast<std::uint8_t>(acc & 255u)); acc >>= 8u; n -= 8; }
    }
    void flush() {
        if (n > 0) out.push_back(static_cast<std::uint8_t>(acc & 255u));
        acc = 0; n = 0;
    }
};

constexpr int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr int kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Tables {
    std::uint8_t len_code[259];     // match length (3..258) -> index 0..28
    std::vector<std::uint8_t> dist_code;   // distance (1..32768) -> index 0..29
    Tables() : dist_code(32769u) {
        for (int c = 0; c < 29; ++c) {
            const int hi = c == 28 ? 258 : kLenBase[c + 1] - 1;
            for (int l = kLenBase[c]; l <= hi && l <= 258; ++l) len_code[l] = static_cast<std::uint8_t>(c);
        }
        len_code[258] = 28;
        for (int c = 0; c < 30; ++c) {
            const int hi = c == 29 ? 32768 : kDistBase[c + 1] - 1;
            for (int d = kDistBase[c]; d <= hi; ++d) dist_code[static_cast<std::size_t>(d)] = static_cast<std::uint8_t>(c);
        }
    }
};
const Tables &tables() { static const Tables t; return t; }

// Code lengths (<= max_len) for the given frequencies; unused symbols get 0.  The length limit uses the usual
// "move codes down until the Kraft sum fits" fix-up.
void build_lengths(const std::uint32_t *freq, int n, int max_len, std::uint8_t *len) {
    std::fill(len, len + n, std::uint8_t{0});
    std::vector<int> syms;
    for (int i = 0; i < n; ++i) if (freq[i] != 0u) syms.push_back(i);
    if (syms.empty()) return;
    if (syms.size() == 1u) { len[syms[0]] = 1; return; }
    const int m = static_cast<int>(syms.size());
    std::vector<std::uint64_t> w(static_cast<std::size_t>(2 * m));
    std::vector<int> parent(static_cast<std::size_t>(2 * m), -1);
    using Item = std::pair<std::uint64_t, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (int k = 0; k < m; ++k) { w[static_cast<std::size_t>(k)] = freq[syms[static_cast<std::size_t>(k)]]; pq.push({w[static_cast<std::size_t>(k)], k}); }
    int next = m;
    while (pq.size() > 1u) {
        const Item a = pq.top(); pq.pop();
        const Item b = pq.top(); pq.pop();
        w[static_cast<std::size_t>(next)] = a.first + b.first;
        parent[static_cast<std::size_t>(a.second)] = next;
        parent[static_cast<std::size_t>(b.second)] = next;
        pq.push({w[static_cast<std::size_t>(next)], next});
        ++next;
    }
    std::vector<int> num(static_cast<std::size_t>(max_len) + 1u, 0);
    for (int k = 0; k < m; ++k) {
        int d = 0;
        for (int x = k; parent[static_cast<std::size_t>(x)] != -1; x = parent[static_cast<std::size_t>(x)]) ++d;
        ++num[static_cast<std::size_t>(std::min(d, max_len))];
    }
    std::uint64_t total = 0;
    for (int i = 1; i <= max_len; ++i) total += static_cast<std::uint64_t>(num[static_cast<std::size_t>(i)]) << (max_len - i);
    while (total > (1ull << max_len)) {
        --num[static_cast<std::size_t>(max_len)];
        for (int i = max_len - 1; i > 0; --i) {
            if (num[static_cast<std::size_t>(i)] != 0) { --num[static_cast<std::size_t>(i)]; num[static_cast<std::size_t>(i) + 1u] += 2; break; }
        }
        --total;
    }
    // most frequent symbols get the shortest codes
    std::sort(syms.begin(), syms.end(), [&](int a, int b) { return freq[a] != freq[b] ? freq[a] > freq[b] : a < b; });
    std::size_t idx = 0;
    for (int i = 1; i <= max_len; ++i)
        for (int c = 0; c < num[static_cast<std::size_t>(i)]; ++c) len[syms[idx++]] = static_cast<std::uint8_t>(i);
}

// Decoders (zlib's included) reject incomplete codes except in very special cases, so always hand out at least two.
void ensure_two(std::uint8_t *len, int n) {
    int used = 0, first = -1;
    for (int i = 0; i < n; ++i) if (len[i] != 0) { ++used; if (first < 0) first = i; }
    if (used >= 2) return;
    if (used == 0) { len[0] = 1; len[1] = 1; return; }
    len[first == 0 ? 1 : 0] = 1;
}

std::uint32_t reverse_bits(std::uint32_t v, int bits) {
    std::uint32_t r = 0;
    for (int i = 0; i < bits; ++i) { r = (r << 1u) | (v & 1u); v >>= 1u; }
    return r;
}
void make_codes(const std::uint8_t *len, int n, std::uint16_t *code) {
    int count[16] = {};
    for (int i = 0; i < n; ++i) ++count[len[i]];
    count[0] = 0;
    int next[16] = {};
    int c = 0;
    for (int b = 1; b <= 15; ++b) { c = (c + count[b - 1]) << 1; next[b] = c; }
    for (int i = 0; i < n; ++i) {
        code[i] = 0;
        if (len[i] != 0) code[i] = static_cast<std::uint16_t>(reverse_bits(static_cast<std::uint32_t>(next[len[i]]++), len[i]));
    }
}

// Token: literal = byte value; match = bit 31 | (length-3) << 16 | (distance-1)
constexpr std::uint32_t kMatchFlag = 0x80000000u;

void emit_block(BitWriter &bw, const std::vector<std::uint32_t> &tok, bool final_block) {
    const Tables &T = tables();
    std::uint32_t lf[286] = {};
    std::uint32_t df[30] = {};
    for (const std::uint32_t t : tok) {
        if (t & kMatchFlag) {
            const int length = static_cast<int>((t >> 16u) & 0xFFu) + 3;
            const int dist = static_cast<int>(t & 0xFFFFu) + 1;
            ++lf[257 + T.len_code[length]];
            ++df[T.dist_code[static_cast<std::size_t>(dist)]];
        } else {
            ++lf[t & 255u];
        }
    }
    lf[256] = 1;
    std::uint8_t ll[286], dl[30];
    build_lengths(lf, 286, 15, ll);
    build_lengths(df, 30, 15, dl);
    ensure_two(ll, 286);
    ensure_two(dl, 30);
    int nlit = 286;
    while (nlit > 257 && ll[nlit - 1] == 0) --nlit;
    int ndist = 30;
    while (ndist > 1 && dl[ndist - 1] == 0) --ndist;

    // code-length sequence with run-length codes 16 / 17 / 18
    struct ClSym { std::uint8_t sym, extra_bits; std::uint8_t extra; };
    std::vector<ClSym> cls;
    std::vector<std::uint8_t> seq(ll, ll + nlit);
    seq.insert(seq.end(), dl, dl + ndist);
    for (std::size_t i = 0; i < seq.size();) {
        const std::uint8_t v = seq[i];
        std::size_t run = 1;
        while (i + run < seq.size() && seq[i + run] == v) ++run;
        i += run;
        if (v == 0) {
            while (run >= 11) { const std::size_t k = std::min<std::size_t>(run, 138); cls.push_back({18, 7, static_cast<std::uint8_t>(k - 11)}); run -= k; }
            if (run >= 3) { cls.push_back({17, 3, static_cast<std::uint8_t>(run - 3)}); run = 0; }
            while (run-- > 0) cls.push_back({0, 0, 0});
        } else {
            cls.push_back({v, 0, 0});
            --run;
            while (run >= 3) { const std::size_t k = std::min<std::size_t>(run, 6); cls.push_back({16, 2, static_cast<std::uint8_t>(k - 3)}); run -= k; }
            while (run-- > 0) cls.push_back({v, 0, 0});
        }
    }
    std::uint32_t cf[19] = {};
    for (const ClSym &c : cls) ++cf[c.sym];
    std::uint8_t cl[19];
    build_lengths(cf, 19, 7, cl);
    ensure_two(cl, 19);
    int hclen = 19;
    while (hclen > 4 && cl[kClOrder[hclen - 1]] == 0) --hclen;

    std::uint16_t lcode[286], dcode[30], ccode[19];
    make_codes(ll, 286, lcode);
    make_codes(dl, 30, dcode);
    make_codes(cl, 19, ccode);

    bw.put(final_block ? 1u : 0u, 1);
    bw.put(2u, 2);
    bw.put(static_cast<std::uint32_t>(nlit - 257), 5);
    bw.put(static_cast<std::uint32_t>(ndist - 1), 5);
    bw.put(static_cast<std::uint32_t>(hclen - 4), 4);
    for (int i = 0; i < hclen; ++i) bw.put(cl[kClOrder[i]], 3);
    for (const ClSym &c : cls) {
        bw.put(ccode[c.sym], cl[c.sym]);
        if (c.extra_bits != 0) bw.put(c.extra, c.extra_bits);
    }
    for (const std::uint32_t t : tok) {
        if (t & kMatchFlag) {
            const int length = static_cast<int>((t >> 16u) & 0xFFu) + 3;
            const int dist = static_cast<int>(t & 0xFFFFu) + 1;
            const int lc = T.len_code[length];
            bw.put(lcode[257 + lc], ll[257 + lc]);
            if (kLenExtra[lc] != 0) bw.put(static_cast<std::uint32_t>(length - kLenBase[lc]), kLenExtra[lc]);
            const int dc = T.dist_code[static_cast<std::size_t>(dist)];
            bw.put(dcode[dc], dl[dc]);
            if (kDistExtra[dc] != 0) bw.put(static_cast<std::uint32_t>(dist - kDistBase[dc]), kDistExtra[dc]);
        } else {
            bw.put(lcode[t & 255u], ll[t & 255u]);
        }
    }
    bw.put(lcode[256], ll[256]);
}

std::vector<std::uint8_t> zlib_compress(const std::uint8_t *data, std::size_t size) {
    std::vector<std::uint8_t> out;
    out.reserve(size / 2u + 1024u);
    out.push_back(0x78); out.push_back(0x9C);
    BitWriter bw(out);

    constexpr int kHashBits = 15;
    constexpr std::size_t kWindow = 32768u;
    constexpr int kMaxChain = 48;
    constexpr int kNiceLength = 128;
    constexpr int kLazyBelow = 32;
    constexpr std::size_t kBlockTokens = 1u << 15u;

    std::vector<std::int64_t> head(1u << kHashBits, -1);
    std::vector<std::int64_t> prev(kWindow, -1);
    const auto hash = [&](std::size_t p) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[p]) | (static_cast<std::uint32_t>(data[p + 1]) << 8u) | (static_cast<std::uint32_t>(data[p + 2]) << 16u);
        return static_cast<std::size_t>((v * 2654435761u) >> (32 - kHashBits));
    };
    const auto insert = [&](std::size_t p) {
        if (p + 2u < size) {
            const std::size_t h = hash(p);
            prev[p & (kWindow - 1u)] = head[h];
            head[h] = static_cast<std::int64_t>(p);
        }
    };
    const auto longest = [&](std::size_t p, int &best_dist) -> int {
        if (p + 2u >= size) return 0;
        const std::size_t max_len = std::min<std::size_t>(258u, size - p);
        std::size_t best = 0;
        std::int64_t cand = head[hash(p)];
        int chain = kMaxChain;
        while (cand >= 0 && chain-- > 0) {
            const std::size_t dist = p - static_cast<std::size_t>(cand);
            if (dist == 0 || dist > kWindow) break;
            if (data[static_cast<std::size_t>(cand) + best] == data[p + best]) {
                std::size_t l = 0;
                while (l < max_len && data[static_cast<std::size_t>(cand) + l] == data[p + l]) ++l;
                if (l > best) {
                    best = l;
                    best_dist = static_cast<int>(dist);
                    if (l >= static_cast<std::size_t>(kNiceLength) || l == max_len) break;
                }
            }
            const std::int64_t nxt = prev[static_cast<std::size_t>(cand) & (kWindow - 1u)];
            if (nxt >= cand) break;
            cand = nxt;
        }
        return best >= 3u ? static_cast<int>(best) : 0;
    };

    std::vector<std::uint32_t> tok;
    tok.reserve(kBlockTokens + 2u);
    const auto flush_if_full = [&]() {
        if (tok.size() >= kBlockTokens) { emit_block(bw, tok, false); tok.clear(); }
    };
    std::size_t p = 0;
    while (p < size) {
        int dist = 0;
        const int len = longest(p, dist);
        if (len >= 3) {
            bool inserted = false;
            int len2 = 0, dist2 = 0;
            if (len < kLazyBelow && p + 1u < size) {
                insert(p);
                inserted = true;
                len2 = longest(p + 1u, dist2);
            }
            if (len2 > len) {
                tok.push_back(data[p]);
                ++p;
                flush_if_full();
                continue;
            }
            tok.push_back(kMatchFlag | (static_cast<std::uint32_t>(len - 3) << 16u) | static_cast<std::uint32_t>(dist - 1));
            for (std::size_t q = p + (inserted ? 1u : 0u); q < p + static_cast<std::size_t>(len); ++q) insert(q);
            p += static_cast<std::size_t>(len);
        } else {
            insert(p);
            tok.push_back(data[p]);
            ++p;
        }
        flush_if_full();
    }
    emit_block(bw, tok, true);
    bw.flush();
    put32(out, adler32(data, size));
    return out;
}

// -------------------------------------------------------------------- Filters

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

// RGB rows with a per-row PNG filter (None / Sub / Up / Average / Paeth), chosen by the smallest sum of |value|.
std::vector<std::uint8_t> filter_rows(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height) {
    const std::size_t stride = static_cast<std::size_t>(width) * 3u;
    std::vector<std::uint8_t> raw((stride + 1u) * height);
    std::vector<std::uint8_t> zero(stride, 0), cur(stride), tmp[5];
    for (auto &t : tmp) t.resize(stride);
    const std::uint8_t *prior = zero.data();
    std::vector<std::uint8_t> prior_row(stride, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *src = rgba + static_cast<std::size_t>(y) * width * 4u;
        for (std::uint32_t x = 0; x < width; ++x) { cur[x * 3u] = src[x * 4u]; cur[x * 3u + 1u] = src[x * 4u + 1u]; cur[x * 3u + 2u] = src[x * 4u + 2u]; }
        std::uint64_t score[5] = {};
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= 3u ? cur[i - 3u] : 0;
            const int b = prior[i];
            const int c = i >= 3u ? prior[i - 3u] : 0;
            const int x = cur[i];
            const std::uint8_t f[5] = {
                static_cast<std::uint8_t>(x),
                static_cast<std::uint8_t>(x - a),
                static_cast<std::uint8_t>(x - b),
                static_cast<std::uint8_t>(x - ((a + b) >> 1)),
                static_cast<std::uint8_t>(x - paeth(a, b, c)),
            };
            for (int k = 0; k < 5; ++k) {
                tmp[k][i] = f[k];
                const int sv = static_cast<std::int8_t>(f[k]);
                score[k] += static_cast<std::uint64_t>(sv < 0 ? -sv : sv);
            }
        }
        int best = 0;
        for (int k = 1; k < 5; ++k) if (score[k] < score[best]) best = k;
        std::uint8_t *dst = raw.data() + static_cast<std::size_t>(y) * (stride + 1u);
        dst[0] = static_cast<std::uint8_t>(best);
        std::memcpy(dst + 1, tmp[best].data(), stride);
        prior_row = cur;
        prior = prior_row.data();
    }
    return raw;
}

} // namespace

std::vector<std::uint8_t> deflate_raw(const std::uint8_t *data, std::size_t size) {
    std::vector<std::uint8_t> z = zlib_compress(data, size);   // 2-byte zlib header + deflate data + 4-byte Adler-32
    if (z.size() < 6u) return {};
    return std::vector<std::uint8_t>(z.begin() + 2, z.end() - 4);
}

bool write_png(const char *path, const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height) {
    if (width == 0u || height == 0u) return false;
    const std::vector<std::uint8_t> raw = filter_rows(rgba, width, height);
    const std::vector<std::uint8_t> z = zlib_compress(raw.data(), raw.size());
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.reserve(z.size() + 128u);
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, width); put32(ihdr, height);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

namespace {
struct AsyncWrites {
    std::mutex m;
    std::vector<std::future<void>> pending;
    ~AsyncWrites() { for (auto &f : pending) if (f.valid()) f.wait(); }
};
AsyncWrites &async_writes() { static AsyncWrites a; return a; }
} // namespace

void write_png_async(std::string path, std::vector<std::uint8_t> rgba, std::uint32_t width, std::uint32_t height) {
    AsyncWrites &a = async_writes();
    std::lock_guard<std::mutex> lock(a.m);
    a.pending.erase(std::remove_if(a.pending.begin(), a.pending.end(), [](std::future<void> &f) {
        return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }), a.pending.end());
    a.pending.push_back(std::async(std::launch::async, [path = std::move(path), rgba = std::move(rgba), width, height]() {
        write_png(path.c_str(), rgba.data(), width, height);
    }));
}

void write_file_async(std::string path, std::vector<std::uint8_t> bytes) {
    AsyncWrites &a = async_writes();
    std::lock_guard<std::mutex> lock(a.m);
    a.pending.push_back(std::async(std::launch::async, [path = std::move(path), bytes = std::move(bytes)]() {
        if (std::FILE *f = std::fopen(path.c_str(), "wb")) { std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f); }
    }));
}

}
