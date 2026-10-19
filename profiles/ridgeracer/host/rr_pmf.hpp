#pragma once

// Minimal MPEG-2 program-stream demuxer for PSP movies (.pmf): pulls the H.264 video elementary stream out of the
// PES packets and splits it into access units with their presentation / decode time stamps (90 kHz ticks).

#include <cstdint>
#include <string>
#include <vector>

namespace rr {

struct PmfAccessUnit {
    std::uint64_t pts{}, dts{};
    std::size_t offset{}, size{};   // into PmfVideo::es (Annex-B byte stream)
};

struct PmfVideo {
    std::vector<std::uint8_t> es;
    std::vector<PmfAccessUnit> units;
};

struct PmfAudioFrame {
    std::uint64_t pts{};
    std::size_t offset{}, size{};   // into PmfAudio::es; the frame includes its 8-byte 0x0F 0xD0 header
};

// The ATRAC3+ audio of a movie (private stream 1): the raw elementary stream split into frames.
struct PmfAudio {
    std::vector<std::uint8_t> es;
    std::vector<PmfAudioFrame> frames;
};

// Parses an in-memory program stream.  `begin` is where the first pack starts (the PSMF header's stream offset).
// Returns false when no video PES packet was found.
bool pmf_demux_video(const std::uint8_t *data, std::size_t size, std::size_t begin, PmfVideo &out, PmfAudio *audio = nullptr);
// Same, reading the file from disk.
bool pmf_load_video(const std::string &path, std::size_t begin, PmfVideo &out, PmfAudio *audio = nullptr);

} // namespace rr
