#pragma once

// H.264 video decoding for the PSP movies.  The Windows build uses the Media Foundation decoder that ships with the
// OS (no extra libraries); other platforms get a null decoder until their native backend (e.g. Android MediaCodec)
// is written.

#include <cstdint>
#include <memory>
#include <vector>

namespace rr {

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Feeds one access unit (Annex-B byte stream).  `pts` is in 90 kHz ticks.
    virtual bool push(const std::uint8_t *data, std::size_t size, std::uint64_t pts) = 0;
    // No more input: lets the decoder release the frames it is still holding.
    virtual void drain() = 0;
    // Next decoded picture in display order as tightly packed RGBA8 (width*height*4 bytes); false when none is ready.
    virtual bool pop(std::vector<std::uint8_t> &rgba, int &width, int &height) = 0;
};

// nullptr when the platform has no decoder or it could not be created.
std::unique_ptr<VideoDecoder> create_h264_decoder(int width, int height);

} // namespace rr
