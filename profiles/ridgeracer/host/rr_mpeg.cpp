#include "rr_mpeg.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "rr_atrac_decoder.hpp"
#include "rr_log.hpp"
#include "rr_pmf.hpp"
#include "rr_video.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rr {
namespace {

using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;
using psprecomp::hex32;

constexpr std::uint32_t kNoData = 0x80618001u;
constexpr std::uint32_t kInvalidValue = 0x806101FEu;
constexpr std::uint32_t kPacketSize = 2048u;
constexpr std::uint32_t kVideoFrameTicks = 3003u;   // 90 kHz ticks per 29.97 fps frame
constexpr std::uint32_t kAtracFrameTicks = 4180u;   // 2048 samples at 44.1 kHz

// SceMpegRingBuffer field offsets.
constexpr std::uint32_t kRbPackets = 0, kRbRead = 4, kRbWritten = 8, kRbFree = 12, kRbPacketSize = 16,
                        kRbData = 20, kRbCallback = 24, kRbCbArg = 28, kRbUpper = 32, kRbSema = 36, kRbMpeg = 40;

struct Movie {
    std::uint32_t stream_offset{}, stream_size{}, first_ts{}, last_ts{}, width{480}, height{272};
    bool valid{};
};

struct Mpeg {
    std::uint32_t handle{}, ring{};
    Movie movie;
    std::uint64_t packets_put{};
    std::uint64_t frames_total{};
    std::uint64_t frame_index{};
    double consumed_fraction{};
    std::uint64_t video_pts{}, audio_pts{};
    std::map<std::uint32_t, std::uint32_t> streams;   // id -> type
    std::uint32_t next_stream{1};
};

Mpeg g_mpeg;
std::string g_movie_path;                      // host path of the last .pmf the game opened
PmfVideo g_video;                              // demuxed video of the current movie
std::unique_ptr<VideoDecoder> g_decoder;       // null: black frames
std::size_t g_next_au = 0;                     // next access unit to feed the decoder
PmfAudio g_audio;                              // ATRAC3+ frames of the movie soundtrack
std::unique_ptr<AtracDecoder> g_atrac;
std::size_t g_audio_next = 0;                  // next frame GetAtracAu hands out
std::size_t g_audio_pending = static_cast<std::size_t>(-1);   // frame the following AtracDecode decodes
std::uint32_t g_audio_decoded = 0, g_au_ok = 0, g_au_nodata = 0, g_dec_calls = 0, g_dec_nopending = 0;
int g_audio_peak = 0;
std::map<std::uint32_t, std::deque<std::vector<std::int16_t>>> g_pcm_by_buffer;   // decode order, per output buffer address
constexpr std::uint32_t kAtracOutBytes = 0x2000u;
bool g_drained = false;
std::uint32_t g_real_frames = 0;
Movie g_last_movie;   // most recently inspected PSMF header
std::uint32_t g_decoded_frames = 0, g_logs = 0;

std::uint32_t be32(GuestMemory &m, std::uint32_t at) {
    const std::uint32_t v = m.load32(at);
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

void note(const std::string &message) {
    if (g_logs++ < 400u) log_line(category::kHle, message);
}

bool parse_psmf(GuestMemory &m, std::uint32_t at, Movie &movie) {
    if (!m.contains(at, 0x100u) || m.load32(at) != 0x464D5350u /* "PSMF" little-endian */) return false;
    movie = {};
    movie.stream_offset = be32(m, at + 8);
    movie.stream_size = be32(m, at + 12);
    movie.first_ts = be32(m, at + 0x54);
    movie.last_ts = be32(m, at + 0x5C);
    const std::uint32_t w = m.load8(at + 0x8E) * 16u, h = m.load8(at + 0x8F) * 16u;
    if (w >= 16u && w <= 1024u && h >= 16u && h <= 512u) { movie.width = w; movie.height = h; }
    movie.valid = true;
    return true;
}

void write_au(GuestMemory &m, std::uint32_t au, std::uint64_t pts, std::uint64_t dts, std::uint32_t es, std::uint32_t es_size) {
    if (!m.contains(au, 24u)) return;
    m.store32(au + 0, static_cast<std::uint32_t>(pts >> 32));
    m.store32(au + 4, static_cast<std::uint32_t>(pts));
    m.store32(au + 8, static_cast<std::uint32_t>(dts >> 32));
    m.store32(au + 12, static_cast<std::uint32_t>(dts));
    m.store32(au + 16, es);
    m.store32(au + 20, es_size);
}

void out32(GuestMemory &m, std::uint32_t at, std::uint32_t value) {
    if (at != 0u && m.contains(at, 4u)) m.store32(at, value);
}

std::uint32_t ring_used(GuestMemory &m, std::uint32_t ring) {
    return m.load32(ring + kRbPackets) - m.load32(ring + kRbFree);
}

// Loads and demuxes the current movie and opens the decoder.  Falls back to the black stand-in on any failure.
void load_real_movie() {
    g_decoder.reset();
    g_video = PmfVideo{};
    g_audio = PmfAudio{};
    g_atrac.reset();
    g_audio_next = 0;
    g_audio_pending = static_cast<std::size_t>(-1);
    g_pcm_by_buffer.clear();
    g_audio_decoded = 0; g_au_ok = 0; g_au_nodata = 0; g_dec_calls = 0; g_dec_nopending = 0; g_audio_peak = 0;
    g_next_au = 0;
    g_drained = false;
    g_real_frames = 0;
    if (g_movie_path.empty() || !g_mpeg.movie.valid) return;
    if (!pmf_load_video(g_movie_path, g_mpeg.movie.stream_offset, g_video, &g_audio)) {
        note("movie: could not demux " + g_movie_path + " - using black frames");
        return;
    }
    g_decoder = create_h264_decoder(static_cast<int>(g_mpeg.movie.width), static_cast<int>(g_mpeg.movie.height));
    note("movie: " + g_movie_path + ": " + std::to_string(g_video.units.size()) + " video access units, decoder " +
         (g_decoder ? "ready" : "unavailable - using black frames"));
    if (!g_decoder) g_video = PmfVideo{};
    note("movie: " + std::to_string(g_audio.frames.size()) + " audio frames");
    if (!g_audio.frames.empty()) {
        const auto &f0 = g_audio.frames[0];
        std::string hex;
        for (std::size_t i = 0; i < 16 && i < f0.size; ++i) { char b[8]; std::snprintf(b, sizeof b, "%02X ", g_audio.es[f0.offset + i]); hex += b; }
        note("movie: first audio frame size " + std::to_string(f0.size) + " bytes: " + hex);
    }
}

// Next decoded picture (display order); feeds the decoder as needed.  False when none is available.
bool next_real_frame(std::vector<std::uint8_t> &rgba, int &w, int &h) {
    if (!g_decoder) return false;
    for (int guard = 0; guard < 64; ++guard) {
        if (g_decoder->pop(rgba, w, h)) return true;
        if (g_next_au < g_video.units.size()) {
            const PmfAccessUnit &au = g_video.units[g_next_au++];
            g_decoder->push(g_video.es.data() + au.offset, au.size, au.dts);
        } else if (!g_drained) {
            g_drained = true;
            g_decoder->drain();
        } else {
            return false;
        }
    }
    return false;
}

void reset_playback() {
    g_mpeg.packets_put = 0;
    g_mpeg.frame_index = 0;
    g_mpeg.consumed_fraction = 0;
    g_mpeg.video_pts = g_mpeg.movie.first_ts;
    g_mpeg.audio_pts = g_mpeg.movie.first_ts;
    const auto span = g_mpeg.movie.last_ts > g_mpeg.movie.first_ts ? g_mpeg.movie.last_ts - g_mpeg.movie.first_ts : 0u;
    g_mpeg.frames_total = span ? std::max<std::uint64_t>(1u, span / kVideoFrameTicks) : 0u;
    if (!g_video.units.empty()) {
        g_mpeg.frames_total = g_video.units.size();
        g_mpeg.video_pts = g_video.units[0].pts;
        g_mpeg.audio_pts = g_mpeg.video_pts;
    }
}

} // namespace

void mpeg_note_movie_file(const std::string &host_path) { g_movie_path = host_path; }

bool mpeg_take_audio(std::uint32_t address, std::uint32_t frames, std::int16_t *out) {
    if (g_pcm_by_buffer.empty() || frames == 0u) return false;
    auto it = g_pcm_by_buffer.upper_bound(address);
    if (it == g_pcm_by_buffer.begin()) return false;
    --it;
    const std::uint32_t base = it->first;
    if (address - base >= kAtracOutBytes || it->second.empty()) return false;
    const std::vector<std::int16_t> &pcm = it->second.front();
    const std::size_t first = static_cast<std::size_t>(address - base) / 4u;   // stereo frames
    const std::size_t total = pcm.size() / 2u;
    if (first >= total) return false;
    const std::size_t n = std::min<std::size_t>(frames, total - first);
    std::memcpy(out, pcm.data() + first * 2u, n * 4u);
    if (n < frames) std::memset(out + n * 2u, 0, (frames - n) * 4u);
    if (first + frames >= total) it->second.pop_front();
    return true;
}

void install_mpeg_hle(Runtime &runtime) {
    g_mpeg = Mpeg{};
    g_last_movie = Movie{};
    g_decoded_frames = 0;
    g_logs = 0;
    g_decoder.reset();
    g_video = PmfVideo{};
    const auto bind = [&](std::uint32_t nid, Runtime::HleFunction handler) {
        runtime.register_hle("sceMpeg", nid, std::move(handler));
    };
    const auto ok = [](AllegrexContext &c) { c.set_gpr(2, 0u); };

    bind(0x682A619Bu, [ok](Runtime &, AllegrexContext &c) { ok(c); });                    // sceMpegInit
    bind(0x874624D6u, [ok](Runtime &, AllegrexContext &c) { ok(c); });                    // sceMpegFinish
    bind(0xC132E22Fu, [](Runtime &, AllegrexContext &c) { c.set_gpr(2, 0x10000u); });     // sceMpegQueryMemSize
    bind(0xD7A29F46u, [](Runtime &, AllegrexContext &c) {                                // sceMpegRingbufferQueryMemSize
        c.set_gpr(2, c.gpr[4] * (104u + kPacketSize));
    });

    // sceMpegRingbufferConstruct(ring, packets, data, size, callback, cbparam)
    bind(0x37295ED8u, [ok](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        const std::uint32_t ring = c.gpr[4], packets = c.gpr[5], data = c.gpr[6];
        if (!m.contains(ring, 48u)) { c.set_gpr(2, kInvalidValue); return; }
        m.store32(ring + kRbPackets, packets);
        m.store32(ring + kRbRead, 0);
        m.store32(ring + kRbWritten, 0);
        m.store32(ring + kRbFree, packets);
        m.store32(ring + kRbPacketSize, kPacketSize);
        m.store32(ring + kRbData, data);
        m.store32(ring + kRbCallback, c.gpr[8]);
        m.store32(ring + kRbCbArg, c.gpr[9]);
        m.store32(ring + kRbUpper, data + packets * kPacketSize);
        m.store32(ring + kRbSema, 0);
        m.store32(ring + kRbMpeg, 0);
        g_mpeg.ring = ring;
        reset_playback();
        note("sceMpegRingbufferConstruct ring=" + hex32(ring) + " packets=" + std::to_string(packets) +
             " data=" + hex32(data) + " callback=" + hex32(c.gpr[8]));
        ok(c);
    });
    bind(0x13407F13u, [ok](Runtime &, AllegrexContext &c) { ok(c); });                    // sceMpegRingbufferDestruct
    // sceMpegRingbufferAvailableSize(ring)
    bind(0xB5F6DC87u, [](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        c.set_gpr(2, m.contains(c.gpr[4], 48u) ? m.load32(c.gpr[4] + kRbFree) : 0u);
    });
    // sceMpegRingbufferPut(ring, numPackets, available): the guest callback is
    // not run; the packets are accounted as written until the movie's own
    // stream size is exhausted, which is how the real callback reports EOF.
    bind(0xB240A59Eu, [](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        const std::uint32_t ring = c.gpr[4];
        const auto wanted = static_cast<std::int32_t>(c.gpr[5]);
        if (!m.contains(ring, 48u) || wanted <= 0) { c.set_gpr(2, kInvalidValue); return; }
        const std::uint32_t free_packets = m.load32(ring + kRbFree);
        std::uint64_t add = std::min<std::uint64_t>(static_cast<std::uint32_t>(wanted), free_packets);
        if (g_mpeg.movie.valid && g_mpeg.movie.stream_size) {
            const std::uint64_t total = g_mpeg.movie.stream_size / kPacketSize;
            add = std::min<std::uint64_t>(add, total > g_mpeg.packets_put ? total - g_mpeg.packets_put : 0u);
        }
        g_mpeg.packets_put += add;
        m.store32(ring + kRbFree, free_packets - static_cast<std::uint32_t>(add));
        m.store32(ring + kRbWritten, m.load32(ring + kRbWritten) + static_cast<std::uint32_t>(add));
        c.set_gpr(2, static_cast<std::uint32_t>(add));
    });

    // sceMpegCreate(mpeg*, data, size, ring, frameWidth, mode, ddrTop)
    bind(0xD8C5F121u, [ok](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        g_mpeg.handle = c.gpr[5] + 0x30u;
        g_mpeg.ring = c.gpr[7];
        g_mpeg.streams.clear();
        g_mpeg.next_stream = 1;
        g_mpeg.movie = g_last_movie;
        load_real_movie();
        reset_playback();
        if (g_mpeg.ring != 0u && m.contains(g_mpeg.ring, 48u)) m.store32(g_mpeg.ring + kRbMpeg, g_mpeg.handle);
        out32(m, c.gpr[4], g_mpeg.handle);
        note("sceMpegCreate handle=" + hex32(g_mpeg.handle) + " ring=" + hex32(g_mpeg.ring) +
             " movie=" + std::to_string(g_mpeg.movie.width) + "x" + std::to_string(g_mpeg.movie.height) +
             " frames=" + std::to_string(g_mpeg.frames_total));
        ok(c);
    });
    bind(0x606A4649u, [ok](Runtime &, AllegrexContext &c) { ok(c); });                    // sceMpegDelete
    // sceMpegQueryStreamOffset(mpeg, header, *offset)
    bind(0x21FF80E4u, [](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        Movie movie;
        if (!parse_psmf(m, c.gpr[5], movie)) { out32(m, c.gpr[6], 0u); c.set_gpr(2, kInvalidValue); return; }
        g_last_movie = movie;
        g_mpeg.movie = movie;
        load_real_movie();   // the game opened the .pmf just before asking for its stream offset
        reset_playback();
        out32(m, c.gpr[6], movie.stream_offset);
        note("sceMpegQueryStreamOffset: PSMF offset=" + hex32(movie.stream_offset) + " size=" + hex32(movie.stream_size) +
             " pts=" + std::to_string(movie.first_ts) + ".." + std::to_string(movie.last_ts) +
             " " + std::to_string(movie.width) + "x" + std::to_string(movie.height));
        c.set_gpr(2, 0u);
    });
    // sceMpegQueryStreamSize(header, *size)
    bind(0x611E9E11u, [](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        Movie movie;
        if (!parse_psmf(m, c.gpr[4], movie)) { out32(m, c.gpr[5], 0u); c.set_gpr(2, kInvalidValue); return; }
        g_last_movie = movie;
        out32(m, c.gpr[5], movie.stream_size);
        c.set_gpr(2, 0u);
    });

    // sceMpegRegistStream(mpeg, type, num) / sceMpegUnRegistStream(mpeg, stream)
    bind(0x42560F23u, [](Runtime &, AllegrexContext &c) {
        const std::uint32_t id = g_mpeg.next_stream++;
        g_mpeg.streams[id] = c.gpr[5];
        note("sceMpegRegistStream type=" + std::to_string(c.gpr[5]) + " num=" + std::to_string(c.gpr[6]) + " -> " + std::to_string(id));
        c.set_gpr(2, id);
    });
    bind(0x591A4AA2u, [ok](Runtime &, AllegrexContext &c) { g_mpeg.streams.erase(c.gpr[5]); ok(c); });
    bind(0xA780CF7Eu, [](Runtime &, AllegrexContext &c) { c.set_gpr(2, 1u); });           // sceMpegMallocAvcEsBuf
    bind(0xCEB870B1u, [ok](Runtime &, AllegrexContext &c) { ok(c); });                    // sceMpegFreeAvcEsBuf
    // sceMpegQueryAtracEsSize(mpeg, *esSize, *outSize)
    bind(0xF8DCB679u, [ok](Runtime &r, AllegrexContext &c) {
        out32(r.memory(), c.gpr[5], 0x2000u);
        out32(r.memory(), c.gpr[6], 0x2000u);
        ok(c);
    });
    // sceMpegInitAu(mpeg, buffer, au)
    bind(0x167AFD9Eu, [ok](Runtime &r, AllegrexContext &c) {
        write_au(r.memory(), c.gpr[6], ~0ull, ~0ull, c.gpr[5], 0u);
        ok(c);
    });
    bind(0x707B7629u, [ok](Runtime &, AllegrexContext &c) { ok(c); });                    // sceMpegFlushAllStream

    // sceMpegGetAvcAu(mpeg, stream, au, *attr)
    bind(0xFE246728u, [](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        const std::uint32_t ring = g_mpeg.ring;
        if (ring == 0u || !m.contains(ring, 48u) || ring_used(m, ring) == 0u) { c.set_gpr(2, kNoData); return; }
        // A frame consumes its share of the movie's packets.
        const double total = g_mpeg.movie.valid && g_mpeg.movie.stream_size
                                 ? static_cast<double>(g_mpeg.movie.stream_size / kPacketSize) : 0.0;
        double share = (total > 0.0 && g_mpeg.frames_total) ? total / static_cast<double>(g_mpeg.frames_total) : 2.0;
        g_mpeg.consumed_fraction += share;
        auto whole = static_cast<std::uint32_t>(g_mpeg.consumed_fraction);
        g_mpeg.consumed_fraction -= whole;
        const std::uint32_t used = ring_used(m, ring);
        const bool last = g_mpeg.frames_total && g_mpeg.frame_index + 1u >= g_mpeg.frames_total;
        if (last) whole = used;
        whole = std::min(whole, used);
        m.store32(ring + kRbFree, m.load32(ring + kRbFree) + whole);
        m.store32(ring + kRbRead, m.load32(ring + kRbRead) + whole);
        std::uint64_t pts = g_mpeg.video_pts, dts = pts > kVideoFrameTicks ? pts - kVideoFrameTicks : 0u;
        if (g_mpeg.frame_index < g_video.units.size()) {   // the movie's real time stamps (they start at 90000, not at the header's value)
            pts = g_video.units[g_mpeg.frame_index].pts;
            dts = g_video.units[g_mpeg.frame_index].dts;
        }
        write_au(m, c.gpr[6], pts, dts, 1u, 0x1000u);
        out32(m, c.gpr[7], 1u);
        g_mpeg.video_pts = pts + kVideoFrameTicks;
        ++g_mpeg.frame_index;
        c.set_gpr(2, 0u);
    });
    // sceMpegGetAtracAu(mpeg, stream, au, *attr): audio trails video by <= 0.1 s
    bind(0xE1CE83A7u, [](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        const std::uint32_t ring = g_mpeg.ring;
        const bool data = ring != 0u && m.contains(ring, 48u) && ring_used(m, ring) != 0u;
        if (!data) { ++g_au_nodata; c.set_gpr(2, kNoData); return; }
        if (!g_audio.frames.empty()) {
            if (g_audio_next >= g_audio.frames.size()) { ++g_au_nodata; c.set_gpr(2, kNoData); return; }
            const PmfAudioFrame &fr = g_audio.frames[g_audio_next];
            if (fr.pts > g_mpeg.video_pts + 9000u) { ++g_au_nodata; c.set_gpr(2, kNoData); return; }
            if (++g_au_ok % 200u == 1u)
                note("movie: GetAtracAu ok=" + std::to_string(g_au_ok) + " nodata=" + std::to_string(g_au_nodata) + " frame=" + std::to_string(g_audio_next) +
                     " audio_pts=" + std::to_string(fr.pts) + " video_pts=" + std::to_string(g_mpeg.video_pts));
            write_au(m, c.gpr[6], fr.pts, fr.pts, 1u, 0x800u);
            out32(m, c.gpr[7], 0u);
            g_audio_pending = g_audio_next++;
            c.set_gpr(2, 0u);
            return;
        }
        if (g_mpeg.audio_pts > g_mpeg.video_pts + 9000u) { c.set_gpr(2, kNoData); return; }
        write_au(m, c.gpr[6], g_mpeg.audio_pts, g_mpeg.audio_pts, 1u, 0x800u);
        out32(m, c.gpr[7], 0u);
        g_mpeg.audio_pts += kAtracFrameTicks;
        c.set_gpr(2, 0u);
    });
    // sceMpegAvcDecode(mpeg, au, frameWidth, *buffer, *init): black frame
    bind(0x0E3C2E9Du, [](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        const std::uint32_t width = std::clamp<std::uint32_t>(c.gpr[6], 16u, 1024u);
        const std::uint32_t frame = m.contains(c.gpr[7], 4u) ? m.load32(c.gpr[7]) : 0u;
        const std::uint32_t rows = g_mpeg.movie.height;
        const std::uint32_t shown = std::min(g_mpeg.movie.width, width);
        if (frame != 0u && m.contains(frame, static_cast<std::size_t>(width) * rows * 4u)) {
            std::vector<std::uint8_t> rgba;
            int fw = 0, fh = 0;
            if (next_real_frame(rgba, fw, fh)) {
                const std::uint32_t copy_w = std::min<std::uint32_t>(static_cast<std::uint32_t>(fw), width);
                const std::uint32_t copy_h = std::min<std::uint32_t>(static_cast<std::uint32_t>(fh), rows);
                for (std::uint32_t y = 0; y < copy_h; ++y)
                    m.copy_in(frame + y * width * 4u, std::span<const std::uint8_t>(rgba.data() + static_cast<std::size_t>(y) * fw * 4u, copy_w * 4u));
                if (++g_real_frames == 1u) note("movie: first real frame decoded");
            } else if (!g_decoder) {
                for (std::uint32_t y = 0; y < rows; ++y)
                    for (std::uint32_t x = 0; x < shown; ++x) m.store32(frame + (y * width + x) * 4u, 0xFF000000u);
            }
        }
        if (++g_decoded_frames == 1u) note(std::string("sceMpegAvcDecode: first frame") + (g_decoder ? "" : " (black stand-in)"));
        out32(m, c.gpr[8], 1u);
        c.set_gpr(2, 0u);
    });
    // sceMpegAvcDecodeStop(mpeg, frameWidth, *buffer, *status)
    bind(0x740FCCD1u, [ok](Runtime &r, AllegrexContext &c) {
        out32(r.memory(), c.gpr[7], 0u);
        ok(c);
    });
    // sceMpegAtracDecode(mpeg, au, buffer, init): silence
    bind(0x800C44DFu, [ok](Runtime &r, AllegrexContext &c) {
        auto &m = r.memory();
        if (g_dec_calls < 2u) {
            // diagnostic: raw guest code leading up to the call site, and the object passed as the first argument
            std::string code;
            const std::uint32_t from = (c.gpr[31] & ~3u) - 0xA0u;
            if (m.contains(from, 0xC0u)) for (std::uint32_t i = 0; i < 0x30u; ++i) code += hex32(m.load32(from + i * 4u)).substr(2) + " ";
            note("movie: code@" + hex32(from) + ": " + code);
            std::string obj;
            if (m.contains(c.gpr[4], 0x60u)) for (std::uint32_t i = 0; i < 24u; ++i) obj += hex32(m.load32(c.gpr[4] + i * 4u)).substr(2) + " ";
            note("movie: a0 object: " + obj);
        }
        if (g_dec_calls == 0u || g_dec_calls == 2u || g_dec_calls == 40u) {
            std::string fn, obj;
            if (g_dec_calls == 0u && m.contains(0x08890DC8u, 0x100u)) {
                for (std::uint32_t i = 0; i < 0x40u; ++i) fn += hex32(m.load32(0x08890DC8u + i * 4u)).substr(2) + " ";
                note("movie: fn@08890DC8: " + fn);
            }
            const std::uint32_t q = m.contains(0x08A46B88u, 4u) ? m.load32(0x08A46B88u) : 0u;
            if (q != 0u && m.contains(q, 0x40u)) {
                for (std::uint32_t i = 0; i < 16u; ++i) obj += hex32(m.load32(q + i * 4u)).substr(2) + " ";
                const std::uint32_t r = m.load32(q + 0x14u), w = m.load32(q + 0x18u);
                note("movie: queue@" + hex32(q) + ": " + obj + " readidx=" + (m.contains(r, 4u) ? hex32(m.load32(r)) : "?") +
                     " writeidx=" + (m.contains(w, 4u) ? hex32(m.load32(w)) : "?"));
            } else note("movie: queue ptr " + hex32(q));
            {
                const std::uint32_t base = q != 0u && m.contains(q, 0x10u) ? m.load32(q + 8u) : 0u;
                std::string arr, st;
                if (base != 0u && m.contains(base, 0x40u)) for (std::uint32_t i = 0; i < 16u; ++i) arr += hex32(m.load32(base + i * 4u)).substr(2) + " ";
                if (m.contains(0x08A46BC0u, 0x40u)) for (std::uint32_t i = 0; i < 16u; ++i) st += hex32(m.load32(0x08A46BC0u + i * 4u)).substr(2) + " ";
                note("movie: array@" + hex32(base) + ": " + arr);
                note("movie: state@08A46BC0: " + st);
            }
            std::string g;
            if (m.contains(0x08A46B60u, 0x40u)) for (std::uint32_t i = 0; i < 16u; ++i) g += hex32(m.load32(0x08A46B60u + i * 4u)).substr(2) + " ";
            note("movie: globals@08A46B60: " + g);
        }
        if (g_dec_calls < 8u) {
            std::string w;
            if (m.contains(c.gpr[5], 24u)) for (std::uint32_t i = 0; i < 6u; ++i) w += hex32(m.load32(c.gpr[5] + i * 4u)) + " ";
            note("movie: AtracDecode args a0=" + hex32(c.gpr[4]) + " a1=" + hex32(c.gpr[5]) + " a2=" + hex32(c.gpr[6]) + " a3=" + hex32(c.gpr[7]) +
                 " ra=" + hex32(c.gpr[31]) + " au=[" + w + "]");
        }
        if (++g_dec_calls % 200u == 1u)
            note("movie: AtracDecode calls=" + std::to_string(g_dec_calls) + " decoded=" + std::to_string(g_audio_decoded) + " peak=" + std::to_string(g_audio_peak) +
                 " buf=" + hex32(c.gpr[6]));
        if (g_audio_pending >= g_audio.frames.size()) ++g_dec_nopending;
        if (c.gpr[6] != 0u && m.contains(c.gpr[6], 0x2000u)) {
            m.zero(c.gpr[6], 0x2000u);
            if (g_audio_pending < g_audio.frames.size()) {
                const PmfAudioFrame &fr = g_audio.frames[g_audio_pending];
                g_audio_pending = static_cast<std::size_t>(-1);
                if (fr.size > 8u) {
                    if (!g_atrac) {
                        g_atrac = create_atrac_decoder(true, 2, 44100, static_cast<int>(fr.size - 8u), nullptr, 0);
                        note(std::string("movie: audio decoder ") + (g_atrac ? "ready" : "unavailable (silence)"));
                    }
                    if (g_atrac) {
                        std::vector<std::int16_t> pcm(2048u * 2u);
                        const int n = g_atrac->decode(g_audio.es.data() + fr.offset + 8u, fr.size - 8u, pcm.data(), 2048);
                        if (n > 0) {
                            const std::size_t bytes = std::min<std::size_t>(0x2000u, static_cast<std::size_t>(n) * g_atrac->channels() * 2u);
                            m.copy_in(c.gpr[6], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(pcm.data()), bytes));
                            if (c.gpr[7] == 0u) {   // the init call decodes into the buffer the next call reuses; the game never plays it
                                auto &q = g_pcm_by_buffer[c.gpr[6]];
                                q.emplace_back(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(bytes / 2u));
                                while (q.size() > 32u) q.pop_front();
                            }
                            for (int i = 0; i < n * 2; ++i) g_audio_peak = std::max(g_audio_peak, std::abs(static_cast<int>(pcm[i])));
                            if (++g_audio_decoded == 1u) note("movie: first audio frame decoded, samples=" + std::to_string(n));
                        } else if (g_audio_decoded == 0u) {
                            note("movie: audio decode failed on frame (size " + std::to_string(fr.size) + ")");
                        }
                    }
                }
            }
        }
        out32(m, c.gpr[7], 1u);
        ok(c);
    });
}

} // namespace rr
