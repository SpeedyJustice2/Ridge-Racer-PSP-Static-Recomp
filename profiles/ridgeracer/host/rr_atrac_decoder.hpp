#pragma once

// ATRAC3 / ATRAC3+ frame decoding through FFmpeg's libavcodec (loaded only when the build found FFmpeg: RR_HAVE_FFMPEG).
// Without it create() returns nullptr and the callers keep playing silence.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rr {

class AtracDecoder {
public:
    virtual ~AtracDecoder() = default;
    // Decodes one frame (`size` bytes of codec data, no container header).  Writes interleaved int16 samples to `out`
    // (room for `max_frames` sample frames) and returns the number of sample frames produced, 0 on failure.
    virtual int decode(const std::uint8_t *data, std::size_t size, std::int16_t *out, int max_frames) = 0;
    // Forgets the overlap state (after a seek / loop jump).
    virtual void flush() = 0;
    [[nodiscard]] virtual int channels() const = 0;
};

// plus: ATRAC3+ (otherwise ATRAC3, which needs the 14 bytes of codec extra data from the WAV fmt chunk).
std::unique_ptr<AtracDecoder> create_atrac_decoder(bool plus, int channels, int sample_rate, int block_align,
                                                   const std::uint8_t *extradata, std::size_t extradata_size);

} // namespace rr
