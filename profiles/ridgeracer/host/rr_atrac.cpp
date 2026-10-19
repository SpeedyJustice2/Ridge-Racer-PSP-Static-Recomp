#include "rr_atrac.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "rr_atrac_decoder.hpp"
#include "rr_log.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rr {
namespace {

constexpr std::uint32_t kBadData = 0x80630002u, kBadId = 0x80630003u, kAllDecoded = 0x80630024u;

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return u16(b, at) | (static_cast<std::uint32_t>(u16(b, at + 2)) << 16);
}

struct Header {
    std::uint32_t size{}, data_offset{}, data_size{}, rate{}, samples{}, block{}, frame_samples{}, sample_offset{};
    std::int32_t loop_start{}, loop_end{-1};
    std::uint32_t channels{2};
    bool plus{true};
    std::vector<std::uint8_t> extra;   // ATRAC3 codec extra data (14 bytes from the fmt chunk)
};

bool parse(std::span<const std::uint8_t> bytes, Header &h) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) return false;
    const auto size = static_cast<std::uint64_t>(u32(bytes, 4)) + 8;
    if (size > 128ull * 1024 * 1024 || size < 44) return false;
    h = {};
    h.size = static_cast<std::uint32_t>(size);
    std::uint32_t encoder_offset = 0, loop_offset = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const auto length = u32(bytes, at + 4);
        const auto begin = at + 8;
        if (!std::memcmp(bytes.data() + at, "data", 4)) { h.data_offset = static_cast<std::uint32_t>(begin); h.data_size = length; break; }
        if (length > bytes.size() - begin) return false;
        if (!std::memcmp(bytes.data() + at, "fmt ", 4) && length >= 16) {
            const auto tag = u16(bytes, begin);
            if (tag != 0xFFFE && tag != 0x270) return false;
            h.channels = u16(bytes, begin + 2);
            if (h.channels != 1 && h.channels != 2) return false;
            h.rate = u32(bytes, begin + 4);
            h.block = u16(bytes, begin + 12);
            h.plus = tag != 0x270;
            if (!h.plus && length >= 18 + 14 && u16(bytes, begin + 16) >= 14)
                h.extra.assign(bytes.begin() + begin + 18, bytes.begin() + begin + 18 + 14);
            h.frame_samples = tag == 0x270 ? 1024 : 2048;
            h.sample_offset = tag == 0x270 ? 69 : 368;
        } else if (!std::memcmp(bytes.data() + at, "fact", 4) && length >= 4) {
            h.samples = u32(bytes, begin);
            if (length >= 8) encoder_offset = u32(bytes, begin + 4);
            loop_offset = length >= 12 ? u32(bytes, begin + 8) : encoder_offset;
        } else if (!std::memcmp(bytes.data() + at, "smpl", 4) && length >= 60 && u32(bytes, begin + 28)) {
            h.loop_start = static_cast<std::int32_t>(u32(bytes, begin + 44));
            h.loop_end = static_cast<std::int32_t>(u32(bytes, begin + 48));
        }
        at = begin + length + (length & 1);
    }
    if (!h.block || !h.rate || !h.data_offset || h.data_offset > h.size || h.data_size > h.size - h.data_offset) return false;
    const auto encoded_samples = static_cast<std::uint64_t>(h.data_size / h.block) * h.frame_samples;
    const auto offset = static_cast<std::uint64_t>(h.sample_offset) + encoder_offset;
    if (offset >= encoded_samples || offset > 0xFFFFFFFFu) return false;
    h.sample_offset = static_cast<std::uint32_t>(offset);
    if (h.loop_end >= 0) {
        h.loop_start = static_cast<std::int32_t>(static_cast<std::int64_t>(h.loop_start) - loop_offset);
        h.loop_end = static_cast<std::int32_t>(static_cast<std::int64_t>(h.loop_end) - loop_offset);
    }
    if (!h.samples) h.samples = static_cast<std::uint32_t>(encoded_samples - h.sample_offset);
    return h.samples != 0;
}

struct Stream {
    Header header;
    std::uint32_t buffer{}, capacity{}, file_offset{};
    std::uint64_t position{}, decoded_frames{};
    std::int32_t loops{};
    bool ended{};
    std::uint32_t chunk_start{}, chunk_len{};     // which file bytes the guest buffer currently holds
    std::unique_ptr<AtracDecoder> decoder;
    bool decoder_tried{};
    std::vector<std::int16_t> pcm;                // last decoded frame (interleaved)
    std::int64_t pcm_frame{-1}, last_decoded{-2};
    bool logged{};
};

// Decodes frame `f` of the stream out of the guest buffer into s.pcm. False when the data isn't resident / no decoder.
bool decode_frame(psprecomp::GuestMemory &memory, Stream &s, std::int64_t f) {
    if (s.pcm_frame == f) return true;
    if (!s.decoder_tried) {
        s.decoder_tried = true;
        s.decoder = create_atrac_decoder(s.header.plus, static_cast<int>(s.header.channels), static_cast<int>(s.header.rate),
                                         static_cast<int>(s.header.block), s.header.extra.data(), s.header.extra.size());
        log_line(category::kHle, std::string("atrac: ") + (s.decoder ? "FFmpeg decoder ready" : "no decoder (silence)"));
    }
    if (!s.decoder) return false;
    const auto feed = [&](std::int64_t idx, bool keep) {
        const std::uint64_t at = static_cast<std::uint64_t>(s.header.data_offset) + static_cast<std::uint64_t>(idx) * s.header.block;
        if (at < s.chunk_start || at + s.header.block > static_cast<std::uint64_t>(s.chunk_start) + s.chunk_len) return false;
        const auto gaddr = s.buffer + static_cast<std::uint32_t>(at - s.chunk_start);
        if (!memory.contains(gaddr, s.header.block)) return false;
        std::vector<std::uint8_t> raw(s.header.block);
        memory.copy_out(gaddr, raw);
        std::vector<std::int16_t> out(static_cast<std::size_t>(s.header.frame_samples) * 2);
        const int n = s.decoder->decode(raw.data(), raw.size(), out.data(), static_cast<int>(s.header.frame_samples));
        if (n <= 0) return false;
        if (keep) {
            // The game always gets stereo from sceAtracDecodeData (mono sources are duplicated into both channels).
            s.pcm.resize(static_cast<std::size_t>(n) * 2);
            if (s.decoder->channels() == 1)
                for (int i = 0; i < n; ++i) s.pcm[2 * i] = s.pcm[2 * i + 1] = out[i];
            else
                std::copy(out.begin(), out.begin() + static_cast<std::size_t>(n) * 2, s.pcm.begin());
            s.pcm_frame = idx;
        }
        return true;
    };
    if (f != s.last_decoded + 1) {
        s.decoder->flush();
        if (f > 0) feed(f - 1, false);   // warm up the overlap state
    }
    if (!feed(f, true)) { s.pcm_frame = -1; return false; }
    s.last_decoded = f;
    return true;
}

std::map<std::uint32_t, Stream> g_streams;
std::uint32_t g_next_id = 0;

void output(psprecomp::GuestMemory &memory, std::uint32_t at, std::uint32_t value) {
    if (at != 0u && memory.contains(at, 4u)) memory.store32(at, value);
}

} // namespace

void install_atrac_hle(psprecomp::Runtime &runtime) {
    g_streams.clear();
    g_next_id = 0;
    const auto bind = [&](std::uint32_t nid, psprecomp::Runtime::HleFunction handler) {
        runtime.register_hle("sceAtrac3plus", nid, std::move(handler));
    };

    // sceAtracSetDataAndGetID(buffer, bufferSize)
    bind(0x7A20E7AFu, [](psprecomp::Runtime &r, psprecomp::AllegrexContext &c) {
        const std::uint32_t buffer = c.gpr[4], capacity = c.gpr[5];
        if (capacity < 128u || !r.memory().contains(buffer, capacity)) { c.set_gpr(2, kBadData); return; }
        std::vector<std::uint8_t> bytes(std::min(65536u, capacity));
        r.memory().copy_out(buffer, bytes);
        Header header;
        if (!parse(bytes, header)) {
            log_line(category::kHle, "sceAtracSetDataAndGetID: buffer " + psprecomp::hex32(buffer) +
                                         " is not a RIFF ATRAC stream; first bytes " +
                                         psprecomp::hex32(bytes.size() >= 4 ? u32(bytes, 0) : 0u));
            c.set_gpr(2, kBadData);
            return;
        }
        if (g_streams.size() >= 6u) { c.set_gpr(2, 0x80630022u); return; }
        std::uint32_t id = 0;
        while (g_streams.contains(id)) ++id;
        Stream stream;
        stream.header = header;
        stream.buffer = buffer;
        stream.capacity = capacity;
        stream.file_offset = std::min(capacity, header.size);
        stream.chunk_start = 0;
        stream.chunk_len = stream.file_offset;
        g_streams[id] = std::move(stream);
        log_line(category::kHle, "sceAtracSetDataAndGetID id=" + std::to_string(id) + " rate=" + std::to_string(header.rate) +
                                     " channels=" + std::to_string(header.channels) + " samples=" + std::to_string(header.samples) +
                                     " frame_samples=" + std::to_string(header.frame_samples) +
                                     " size=" + std::to_string(header.size) + " buffer=" + std::to_string(capacity));
        c.set_gpr(2, id);
    });
    // sceAtracReleaseAtracID(id)
    bind(0x61EB33F5u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &c) {
        c.set_gpr(2, g_streams.erase(c.gpr[4]) ? 0u : kBadId);
    });
    // sceAtracSetLoopNum(id, loops)
    bind(0x868120B5u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &c) {
        const auto it = g_streams.find(c.gpr[4]);
        if (it == g_streams.end()) { c.set_gpr(2, kBadId); return; }
        it->second.loops = static_cast<std::int32_t>(c.gpr[5]);
        log_line(category::kHle, "sceAtracSetLoopNum id=" + std::to_string(c.gpr[4]) + " loops=" +
                                     std::to_string(static_cast<std::int32_t>(c.gpr[5])) +
                                     " file_loop_end=" + std::to_string(it->second.header.loop_end));
        c.set_gpr(2, 0u);
    });
    // sceAtracGetRemainFrame(id, int *remain)
    bind(0x9AE849A7u, [](psprecomp::Runtime &r, psprecomp::AllegrexContext &c) {
        const auto it = g_streams.find(c.gpr[4]);
        if (it == g_streams.end()) { c.set_gpr(2, kBadId); return; }
        const Stream &s = it->second;
        const std::uint32_t remaining = static_cast<std::uint32_t>(
            (s.header.samples > s.position ? s.header.samples - s.position : 0u) / s.header.frame_samples);
        output(r.memory(), c.gpr[5], remaining);
        c.set_gpr(2, 0u);
    });
    // sceAtracGetStreamDataInfo(id, u8 **writePtr, u32 *writableBytes, u32 *readOffset)
    bind(0x5D268707u, [](psprecomp::Runtime &r, psprecomp::AllegrexContext &c) {
        const auto it = g_streams.find(c.gpr[4]);
        if (it == g_streams.end()) { c.set_gpr(2, kBadId); return; }
        const Stream &s = it->second;
        const std::uint32_t remaining = s.header.size - s.file_offset;
        output(r.memory(), c.gpr[5], s.buffer);
        output(r.memory(), c.gpr[6], std::min(s.capacity, remaining));
        output(r.memory(), c.gpr[7], s.file_offset);
        c.set_gpr(2, 0u);
    });
    // sceAtracAddStreamData(id, bytesToAdd)
    bind(0x7DB31251u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &c) {
        const auto it = g_streams.find(c.gpr[4]);
        if (it == g_streams.end()) { c.set_gpr(2, kBadId); return; }
        Stream &s = it->second;
        const std::uint32_t count = c.gpr[5];
        if (count > s.capacity || count > s.header.size - s.file_offset) { c.set_gpr(2, kBadData); return; }
        s.chunk_start = s.file_offset;
        s.chunk_len = count;
        s.file_offset += count;
        c.set_gpr(2, 0u);
    });
    // sceAtracDecodeData(id, s16 *out, int *samples, int *finished, int *remainFrames)
    bind(0x6A8C3CD5u, [](psprecomp::Runtime &r, psprecomp::AllegrexContext &c) {
        const auto it = g_streams.find(c.gpr[4]);
        if (it == g_streams.end()) { c.set_gpr(2, kBadId); return; }
        Stream &s = it->second;
        output(r.memory(), c.gpr[6], 0u);
        output(r.memory(), c.gpr[7], 0u);
        if (s.ended) { output(r.memory(), c.gpr[7], 1u); c.set_gpr(2, kAllDecoded); return; }
        if (!r.memory().contains(c.gpr[5], s.header.frame_samples * 4u)) { c.set_gpr(2, kBadData); return; }
        const std::uint32_t boundary = s.loops && s.header.loop_end >= 0
            ? std::min(s.header.samples, static_cast<std::uint32_t>(s.header.loop_end) + 1u)
            : s.header.samples;
        const std::uint64_t available = boundary > s.position ? boundary - s.position : 0ull;
        const std::uint64_t frame_left = s.header.frame_samples - (s.header.sample_offset + s.position) % s.header.frame_samples;
        const std::uint64_t samples = std::min<std::uint64_t>(frame_left, available);
        if (!samples) { s.ended = true; output(r.memory(), c.gpr[7], 1u); c.set_gpr(2, 0u); return; }
        {
            const std::uint64_t abs = s.header.sample_offset + s.position;
            const auto f = static_cast<std::int64_t>(abs / s.header.frame_samples);
            const auto intra = static_cast<std::size_t>(abs % s.header.frame_samples);
            const std::size_t ch = 2;
            if (decode_frame(r.memory(), s, f) && (intra + samples) * ch <= s.pcm.size()) {
                r.memory().copy_in(c.gpr[5], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(s.pcm.data() + intra * ch), static_cast<std::size_t>(samples) * ch * 2));
                if (!s.logged) { s.logged = true; log_line(category::kHle, "atrac: decoding real audio"); }
            } else {
                r.memory().zero(c.gpr[5], static_cast<std::size_t>(samples) * 4u);   // silence
            }
        }
        const auto count = static_cast<std::uint32_t>(samples);
        s.position += count;
        ++s.decoded_frames;
        output(r.memory(), c.gpr[6], count);
        if (s.position >= boundary) {
            if (s.loops) {
                if (s.loops > 0) --s.loops;
                s.position = static_cast<std::uint32_t>(std::max(0, s.header.loop_start));
                s.file_offset = s.header.data_offset +
                    static_cast<std::uint32_t>((s.header.sample_offset + s.position) / s.header.frame_samples) * s.header.block;
            } else {
                s.ended = true;
                output(r.memory(), c.gpr[7], 1u);
                log_line(category::kHle, "sceAtracDecodeData id=" + std::to_string(c.gpr[4]) + " reached the end (finished=1)");
            }
        }
        const std::uint32_t remaining = boundary > s.position ? static_cast<std::uint32_t>((boundary - s.position) / s.header.frame_samples) : 0u;
        output(r.memory(), c.gpr[8], remaining);
        c.set_gpr(2, 0u);
    });
}

} // namespace rr
