#include "rr_pmf.hpp"

#include <algorithm>
#include <cstdio>

namespace rr {
namespace {

std::uint64_t read_ts(const std::uint8_t *p) {
    return (static_cast<std::uint64_t>((p[0] >> 1) & 7u) << 30) | (static_cast<std::uint64_t>(p[1]) << 22) |
           (static_cast<std::uint64_t>(p[2] >> 1) << 15) | (static_cast<std::uint64_t>(p[3]) << 7) | (p[4] >> 1);
}


struct StampRec { std::size_t offset; std::uint64_t pts, dts; };

// Splits the Annex-B stream into access units.  A new unit starts at an access-unit delimiter / SPS / PPS / SEI that
// follows a picture, or at a slice with first_mb_in_slice == 0 that follows a slice.  Time stamps come from the PES
// packets (a stamp belongs to the first unit that starts at or after its packet); units without one continue at the
// usual 29.97 fps spacing.
void split_access_units(PmfVideo &v, const std::vector<StampRec> &stamps) {
    const std::vector<std::uint8_t> &es = v.es;
    std::vector<std::size_t> starts;
    bool have_vcl = false;
    for (std::size_t i = 0; i + 4 < es.size(); ++i) {
        if (!(es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1)) continue;
        std::size_t nal_at = i;
        if (nal_at > 0 && es[nal_at - 1] == 0) --nal_at;   // four-byte start code
        const unsigned type = es[i + 3] & 0x1Fu;
        const bool first_slice = (type == 1u || type == 5u) && i + 4 < es.size() && (es[i + 4] & 0x80u) != 0u;
        if (type == 6u || type == 7u || type == 8u || type == 9u) {
            if (have_vcl) { starts.push_back(nal_at); have_vcl = false; }
            else if (starts.empty()) starts.push_back(nal_at);
        } else if (type == 1u || type == 5u) {
            if (have_vcl && first_slice) starts.push_back(nal_at);
            else if (starts.empty()) starts.push_back(nal_at);
            have_vcl = true;
        }
        i += 2;
    }
    if (starts.empty()) starts.push_back(0);
    v.units.clear();
    for (std::size_t k = 0; k < starts.size(); ++k) {
        PmfAccessUnit au;
        au.offset = starts[k];
        au.size = (k + 1 < starts.size() ? starts[k + 1] : es.size()) - starts[k];
        v.units.push_back(au);
    }
    std::vector<bool> stamped(v.units.size(), false);
    for (const StampRec &s : stamps) {
        std::size_t lo = 0, hi = v.units.size();
        while (lo < hi) { const std::size_t mid = (lo + hi) / 2; if (v.units[mid].offset < s.offset) lo = mid + 1; else hi = mid; }
        if (lo < v.units.size() && !stamped[lo]) { v.units[lo].pts = s.pts; v.units[lo].dts = s.dts; stamped[lo] = true; }
    }
    std::uint64_t pts = 0, dts = 0;
    bool seen = false;
    for (std::size_t k = 0; k < v.units.size(); ++k) {
        if (stamped[k]) { pts = v.units[k].pts; dts = v.units[k].dts; seen = true; }
        else if (seen) { pts += 3003u; dts += 3003u; v.units[k].pts = pts; v.units[k].dts = dts; }
    }
}


// Splits the audio elementary stream into ATRAC3+ frames.  Every frame starts with 0x0F 0xD0 and a 10-bit length field
// in the following two bytes; the exact frame size is taken as the candidate that lands on the next sync marker.
void split_audio_frames(PmfAudio &a, const std::vector<StampRec> &stamps) {
    const std::vector<std::uint8_t> &es = a.es;
    std::size_t p = 0;
    while (p + 8 <= es.size()) {
        if (!(es[p] == 0x0F && es[p + 1] == 0xD0)) { ++p; continue; }
        const std::size_t units = (static_cast<std::size_t>(es[p + 2] & 3u) << 8) | es[p + 3];
        std::size_t size = 0;
        for (const std::size_t extra : {8u, 16u, 0u, 24u, 32u}) {
            const std::size_t cand = units * 8u + extra;
            if (cand < 16u || p + cand > es.size()) continue;
            if (p + cand == es.size() || (p + cand + 1 < es.size() && es[p + cand] == 0x0F && es[p + cand + 1] == 0xD0)) { size = cand; break; }
        }
        if (size == 0) { ++p; continue; }
        PmfAudioFrame f;
        f.offset = p; f.size = size;
        a.frames.push_back(f);
        p += size;
    }
    // time stamps: a stamp belongs to the first frame that starts at or after its packet; later frames follow at 2048 samples
    std::vector<bool> stamped(a.frames.size(), false);
    for (const StampRec &s : stamps) {
        std::size_t lo = 0, hi = a.frames.size();
        while (lo < hi) { const std::size_t mid = (lo + hi) / 2; if (a.frames[mid].offset < s.offset) lo = mid + 1; else hi = mid; }
        if (lo < a.frames.size() && !stamped[lo]) { a.frames[lo].pts = s.pts; stamped[lo] = true; }
    }
    double pts = 0.0;
    bool seen = false;
    for (std::size_t k = 0; k < a.frames.size(); ++k) {
        if (stamped[k]) { pts = static_cast<double>(a.frames[k].pts); seen = true; }
        else if (seen) { pts += 2048.0 * 90000.0 / 44100.0; a.frames[k].pts = static_cast<std::uint64_t>(pts + 0.5); }
        if (stamped[k] || !seen) continue;
    }
}

template <class T> std::vector<StampRec> stamps_to_pairs(const T &in) {
    std::vector<StampRec> out;
    for (const auto &s : in) out.push_back({s.offset, s.pts, s.dts});
    return out;
}

} // namespace

bool pmf_demux_video(const std::uint8_t *data, std::size_t size, std::size_t begin, PmfVideo &out, PmfAudio *audio) {
    out = PmfVideo{};
    if (audio != nullptr) *audio = PmfAudio{};
    std::vector<StampRec> audio_stamps;
    std::size_t i = begin;
    struct Stamp { std::size_t offset; std::uint64_t pts, dts; };
    std::vector<Stamp> stamps;
    while (i + 4 <= size) {
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) { ++i; continue; }
        const std::uint8_t id = data[i + 3];
        if (id == 0xBA) {   // pack header
            if (i + 14 > size) break;
            if ((data[i + 4] & 0xC0u) == 0x40u) i += 14u + (data[i + 13] & 7u);   // MPEG-2
            else i += 12;                                                          // MPEG-1
            continue;
        }
        if (id == 0xB9) break;   // end code
        if (id < 0xBB) { i += 4; continue; }   // sequence / picture start codes outside any PES: not a program-stream packet
        if (i + 6 > size) break;
        const std::size_t length = (static_cast<std::size_t>(data[i + 4]) << 8) | data[i + 5];
        const std::size_t end = length ? i + 6 + length : size;
        if (id >= 0xE0 && id <= 0xEF) {   // video PES
            if (i + 9 > size) break;
            std::size_t p = i + 6;
            if ((data[p] & 0xC0u) != 0x80u) { i = std::min(end, size); continue; }   // not an MPEG-2 PES header
            const std::uint8_t flags = data[p + 1];
            const std::size_t header_len = data[p + 2];
            std::uint64_t pts = 0, dts = 0;
            bool have_pts = false;
            if ((flags & 0x80u) && p + 3 + 5 <= size) { pts = read_ts(data + p + 3); dts = pts; have_pts = true; }
            if ((flags & 0x40u) && p + 3 + 10 <= size) dts = read_ts(data + p + 8);
            const std::size_t payload = p + 3 + header_len;
            const std::size_t payload_end = std::min(end, size);
            if (payload < payload_end) {
                if (have_pts) stamps.push_back({out.es.size(), pts, dts});
                out.es.insert(out.es.end(), data + payload, data + payload_end);
            }
        }
        else if (id == 0xBD && audio != nullptr && i + 9 <= size) {   // private stream 1: the ATRAC3+ audio
            const std::size_t p = i + 6;
            if ((data[p] & 0xC0u) == 0x80u) {
                const std::uint8_t flags = data[p + 1];
                const std::size_t payload = p + 3 + data[p + 2];
                const std::size_t payload_end = std::min(end, size);
                std::uint64_t pts = 0;
                const bool have_pts = (flags & 0x80u) && p + 3 + 5 <= size;
                if (have_pts) pts = read_ts(data + p + 3);
                // payload: sub-stream id (0x40..), then three bytes of frame bookkeeping, then audio frames
                if (payload + 4 < payload_end && (data[payload] <= 0x0Fu || (data[payload] >= 0x40u && data[payload] <= 0x4Fu))) {
                    if (have_pts) audio_stamps.push_back({audio->es.size(), pts, pts});
                    audio->es.insert(audio->es.end(), data + payload + 4, data + payload_end);
                }
            }
        }
        i = std::min(end, size);
        if (i == size) break;
    }
    if (audio != nullptr) split_audio_frames(*audio, audio_stamps);
    if (out.es.empty()) return false;
    split_access_units(out, stamps_to_pairs(stamps));
    return !out.units.empty();
}

bool pmf_load_video(const std::string &path, std::size_t begin, PmfVideo &out, PmfAudio *audio) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> buf(n > 0 ? static_cast<std::size_t>(n) : 0u);
    const std::size_t got = buf.empty() ? 0u : std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size() || buf.empty()) return false;
    return pmf_demux_video(buf.data(), buf.size(), begin, out, audio);
}

} // namespace rr
